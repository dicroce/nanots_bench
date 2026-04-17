// Workload: multi_stream_write
//
// WHAT IT MEASURES:
//   K writer threads each write exclusively to their own stream simultaneously.
//   K sweeps over {1, 2, 4, 8}.  Each value of K runs for 10 seconds.
//
//   This directly exercises an architectural property of nanots:
//     - nanots: each write_context maps to its own blocks.  Writers to
//       different streams never touch each other's state.  Total throughput
//       scales (near-)linearly with K.
//     - SQLite: WAL mode serialises all writers through a single write lock.
//       Total throughput stays flat as K grows; per-writer throughput halves
//       with each doubling of K.
//
//   This is the companion to concurrent_readers.  Together they show:
//     concurrent_readers  — readers don't block the writer.
//     multi_stream_write  — writers don't block each other.
//
// THREAD SAFETY CONTRACT:
//   backend.write() is called from K threads simultaneously with K distinct
//   stream names.  Backends serialise internally as needed.
//   nanots: only the write_context map lookup is locked; writes themselves
//   are lock-free because each stream's context is owned by one thread.
//   SQLite: entire write() call is serialised by a mutex (WAL only allows
//   one writer anyway; the mutex makes that constraint measurable).
//
// NOTE ON sync_boundary():
//   Not called during the measurement window.  The focus is multi-writer
//   scaling, not durability.  nanots flushes naturally at block boundaries.

#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/timing.h"
#include "bench/workload.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Simple countdown latch (C++17 compatible)
// ---------------------------------------------------------------------------
class MswLatch {
public:
    explicit MswLatch(int n) : _count(n) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lk(_mu);
        if (--_count == 0) _cv.notify_all();
        else _cv.wait(lk, [this]{ return _count == 0; });
    }
private:
    std::mutex              _mu;
    std::condition_variable _cv;
    int                     _count;
};

// ---------------------------------------------------------------------------
// MultiStreamWrite workload
// ---------------------------------------------------------------------------
class MultiStreamWrite : public Workload {
private:
    struct SubResult {
        int      k             = 0;
        uint64_t total_ops     = 0;
        double   total_ops_sec = 0;
        LatencyHistogram::Stats latency;
    };

    static void emit_table(const std::vector<SubResult>& subs) {
        fprintf(stderr, "\n%-6s  %-16s  %-16s  %s\n",
                "K", "total w/s", "per-writer w/s", "scale factor");
        fprintf(stderr, "------  ----------------  ----------------  ------------\n");
        double base = subs.empty() ? 1.0 : subs[0].total_ops_sec;
        for (const auto& sr : subs) {
            double per_writer = sr.k > 0 ? sr.total_ops_sec / sr.k : 0.0;
            double factor     = base > 0 ? sr.total_ops_sec / base : 0.0;
            fprintf(stderr, "%-6d  %-16.0f  %-16.0f  %.2fx\n",
                    sr.k, sr.total_ops_sec, per_writer, factor);
        }
        fprintf(stderr, "\n");
    }

public:
    std::string name() const override { return "multi_stream_write"; }

    WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) override {
        WorkloadResult result;
        result.backend_name    = backend.name();
        result.backend_version = backend.version();
        result.backend_config  = backend.config_summary();
        result.workload_name   = name();
        result.fairness_axis   = cfg.fairness_axis;

        const int window_sec = 10;

        result.workload_config["window_sec_per_k"] = std::to_string(window_sec);
        result.workload_config["frame_size"]        = std::to_string(cfg.frame_size);
        result.workload_config["durability_bytes"]  = std::to_string(cfg.durability_bytes);

        const std::vector<int> writer_counts = {1, 2, 4, 8};

        std::vector<SubResult> subs;
        subs.reserve(writer_counts.size());

        std::ostringstream scaling_ss;
        bool first = true;

        for (int K : writer_counts) {
            SubResult sr;
            sr.k = K;

            // Unique stream names per K so timestamps never collide across sweeps.
            auto stream_name = [&](int i) {
                return "msw_k" + std::to_string(K) + "_s" + std::to_string(i);
            };

            // Pre-warm: create write contexts before threads start so the
            // context-map lookup never races with writes during measurement.
            {
                std::vector<uint8_t> buf(cfg.frame_size, 0xAB);
                for (int i = 0; i < K; ++i) {
                    Frame f{};
                    f.data = buf.data(); f.size = buf.size();
                    f.timestamp_us = 1;  f.flags = 0;
                    backend.write(stream_name(i), f);
                }
            }

            std::vector<std::atomic<uint64_t>> op_counts(K);
            for (auto& c : op_counts) c.store(0, std::memory_order_relaxed);
            std::vector<LatencyHistogram> hists(K);
            for (auto& h : hists) h.reserve(100'000);

            MswLatch latch(K);
            std::atomic<bool> stop{false};

            std::vector<std::thread> threads;
            std::vector<std::exception_ptr> thread_ex(K, nullptr);
            threads.reserve(K);

            for (int i = 0; i < K; ++i) {
                threads.emplace_back([&, i]{
                    try {
                        std::string stream = stream_name(i);
                        std::vector<uint8_t> buf(cfg.frame_size, 0xAB);
                        uint64_t ts = 2; // warmup wrote ts=1

                        latch.arrive_and_wait();

                        while (!stop.load(std::memory_order_relaxed)) {
                            Frame f{};
                            f.data = buf.data(); f.size = buf.size();
                            f.timestamp_us = ts++; f.flags = 0;

                            double lat = measure_us([&]{ backend.write(stream, f); });
                            hists[i].record(lat);
                            op_counts[i].fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        thread_ex[i] = std::current_exception();
                        stop.store(true, std::memory_order_relaxed);
                    }
                });
            }

            std::this_thread::sleep_for(std::chrono::seconds(window_sec));
            stop.store(true, std::memory_order_relaxed);
            for (auto& t : threads) t.join();

            // Propagate first thread exception (if any) back to the caller.
            for (int i = 0; i < K; ++i)
                if (thread_ex[i]) std::rethrow_exception(thread_ex[i]);

            for (int i = 0; i < K; ++i)
                sr.total_ops += op_counts[i].load(std::memory_order_relaxed);
            sr.total_ops_sec = static_cast<double>(sr.total_ops) / window_sec;

            // Merge per-writer latency histograms into one Stats for the result.
            LatencyHistogram merged;
            merged.reserve(static_cast<size_t>(K) * 4);
            for (int i = 0; i < K; ++i) {
                auto s = hists[i].compute();
                merged.record(s.p50_us);
                merged.record(s.p95_us);
                merged.record(s.p99_us);
                merged.record(s.max_us);
            }
            sr.latency = merged.compute();

            subs.push_back(sr);

            if (!first) scaling_ss << " | ";
            first = false;
            scaling_ss << "k=" << K << ": "
                       << static_cast<long long>(sr.total_ops_sec) << " total w/s";
        }

        result.workload_config["writer_scaling"] = scaling_ss.str();

        emit_table(subs);

        // Aggregate metrics at K=8 (the most revealing data point).
        const SubResult& peak = subs.back();
        result.total_ops     = peak.total_ops;
        result.total_seconds = window_sec;
        result.ops_per_sec   = peak.total_ops_sec;
        result.latency       = peak.latency;

        // thread_results: one entry per K value for the scaling chart.
        for (const auto& sr : subs) {
            ThreadResult tr;
            tr.thread_id     = sr.k;
            tr.role          = 0;
            tr.total_ops     = sr.total_ops;
            tr.total_seconds = window_sec;
            tr.ops_per_sec   = sr.total_ops_sec;
            tr.latency       = sr.latency;
            result.thread_results.push_back(tr);
        }

        return result;
    }
};

REGISTER_WORKLOAD(MultiStreamWrite, "multi_stream_write");

} // namespace bench

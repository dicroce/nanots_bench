// Workload: write_read_contention
//
// WHAT IT MEASURES:
//   K writer threads each write to their own stream while K reader threads
//   simultaneously seek and scan their own pre-filled streams.
//   K sweeps over {1, 2, 4, 8}.  Each value of K runs for 10 seconds.
//
//   This is nanots's killer scenario:
//     - nanots: writers and readers are completely independent.  Each
//       write_context owns its own mmap region; each nanots_iterator opens its
//       own SQLite connection.  No shared lock anywhere in the hot path.
//       Total throughput (writes + reads) scales linearly with K.
//     - SQLite: WAL mode serialises all writers through a single write lock AND
//       every reader shares the same sqlite3* connection under a mutex.
//       Adding readers hurts writers (WAL grows, checkpoint pressure) and
//       adding writers hurts readers (WAL lock stalls both sides).
//
//   Expected result:
//     nanots: writer w/s ≈ multi_stream_write K=n; reader seeks/s grows with K.
//             Neither side degrades the other.
//     SQLite: writer w/s falls as K grows (WAL lock).  Reader seeks/s also
//             falls because the shared connection serialises all read calls.
//
// READER DESIGN:
//   Each reader pre-fills its own dedicated stream (wrc_r_{i}) once before the
//   sweep begins, then loops: find(ts) → scan SCAN_FRAMES frames → advance ts.
//   When ts wraps past the pre-filled range it resets to 1.  Each find() call
//   counts as one "read operation".
//
// WRITER DESIGN:
//   Same as multi_stream_write: each writer writes ts=2,3,... to its own stream
//   wrc_w_k{K}_{i}.  Streams are unique per K so timestamps never collide across
//   sweeps.  Pre-warm (ts=1) is done before threads start.

#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/timing.h"
#include "bench/workload.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Simple countdown latch (C++17 compatible)
// ---------------------------------------------------------------------------
class WrcLatch {
public:
    explicit WrcLatch(int n) : _count(n) {}
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
// WriteReadContention workload
// ---------------------------------------------------------------------------
class WriteReadContention : public Workload {
private:
    // Number of frames pre-filled into each reader stream.
    // 10 000 × 4 KB = 40 MB per stream, 320 MB for 8 streams — manageable.
    static constexpr int READ_PREFILL  = 10'000;
    // Frames scanned per seek before the reader issues the next find().
    static constexpr int SCAN_FRAMES   = 32;

    struct SubResult {
        int      k              = 0;
        uint64_t writer_ops     = 0;
        double   writer_ops_sec = 0;
        uint64_t reader_ops     = 0;
        double   reader_ops_sec = 0;
        LatencyHistogram::Stats write_latency;
        LatencyHistogram::Stats read_latency;
    };

    static void emit_table(const std::vector<SubResult>& subs) {
        fprintf(stderr, "\n%-6s  %-16s  %-16s  %s\n",
                "K", "writer w/s", "reader seeks/s", "combined w/s");
        fprintf(stderr, "------  ----------------  ----------------  ------------\n");
        for (const auto& sr : subs) {
            double combined = sr.writer_ops_sec + sr.reader_ops_sec;
            fprintf(stderr, "%-6d  %-16.0f  %-16.0f  %.0f\n",
                    sr.k, sr.writer_ops_sec, sr.reader_ops_sec, combined);
        }
        fprintf(stderr, "\n");
    }

public:
    std::string name() const override { return "write_read_contention"; }

    WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) override {
        WorkloadResult result;
        result.backend_name    = backend.name();
        result.backend_version = backend.version();
        result.backend_config  = backend.config_summary();
        result.workload_name   = name();
        result.fairness_axis   = cfg.fairness_axis;

        const int window_sec = 10;
        const int MAX_K      = 8;

        result.workload_config["window_sec_per_k"] = std::to_string(window_sec);
        result.workload_config["frame_size"]        = std::to_string(cfg.frame_size);
        result.workload_config["durability_bytes"]  = std::to_string(cfg.durability_bytes);
        result.workload_config["read_prefill"]      = std::to_string(READ_PREFILL);
        result.workload_config["scan_frames"]       = std::to_string(SCAN_FRAMES);

        // Names for reader streams (shared across all K values — pre-filled once).
        auto read_stream = [](int i) {
            return "wrc_r_" + std::to_string(i);
        };
        // Names for writer streams (unique per K to avoid timestamp conflicts).
        auto write_stream = [](int K, int i) {
            return "wrc_w_k" + std::to_string(K) + "_" + std::to_string(i);
        };

        // ------------------------------------------------------------------
        // Pre-fill reader streams once, before the sweep.
        // ------------------------------------------------------------------
        fprintf(stderr, "Pre-filling %d reader streams (%d frames each)...\n",
                MAX_K, READ_PREFILL);
        {
            std::vector<uint8_t> buf(cfg.frame_size, 0xCD);
            for (int i = 0; i < MAX_K; ++i) {
                for (int f = 0; f < READ_PREFILL; ++f) {
                    Frame fr{};
                    fr.data         = buf.data();
                    fr.size         = buf.size();
                    fr.timestamp_us = static_cast<uint64_t>(f + 1);
                    fr.flags        = 0;
                    backend.write(read_stream(i), fr);
                }
            }
        }

        // Pre-warm writer contexts for all K values (ts=1 sentinel).
        {
            std::vector<uint8_t> buf(cfg.frame_size, 0xAB);
            for (int K : {1, 2, 4, 8}) {
                for (int i = 0; i < K; ++i) {
                    Frame f{};
                    f.data = buf.data(); f.size = buf.size();
                    f.timestamp_us = 1; f.flags = 0;
                    backend.write(write_stream(K, i), f);
                }
            }
        }

        const std::vector<int> counts = {1, 2, 4, 8};
        std::vector<SubResult> subs;
        subs.reserve(counts.size());

        std::ostringstream writer_ss, reader_ss;
        bool first = true;

        for (int K : counts) {
            SubResult sr;
            sr.k = K;

            std::vector<std::atomic<uint64_t>> writer_ops(K), reader_ops(K);
            for (auto& c : writer_ops) c.store(0, std::memory_order_relaxed);
            for (auto& c : reader_ops) c.store(0, std::memory_order_relaxed);

            std::vector<LatencyHistogram> write_hists(K), read_hists(K);
            for (auto& h : write_hists) h.reserve(50'000);
            for (auto& h : read_hists)  h.reserve(50'000);

            WrcLatch latch(K * 2);  // K writers + K readers
            std::atomic<bool> stop{false};
            std::vector<std::exception_ptr> thread_ex(K * 2, nullptr);
            std::vector<std::thread> threads;
            threads.reserve(K * 2);

            // Writer threads
            for (int i = 0; i < K; ++i) {
                threads.emplace_back([&, i]{
                    try {
                        std::string stream = write_stream(K, i);
                        std::vector<uint8_t> buf(cfg.frame_size, 0xAB);
                        uint64_t ts = 2;  // warmup wrote ts=1

                        latch.arrive_and_wait();

                        while (!stop.load(std::memory_order_relaxed)) {
                            Frame f{};
                            f.data = buf.data(); f.size = buf.size();
                            f.timestamp_us = ts++; f.flags = 0;

                            double lat = measure_us([&]{ backend.write(stream, f); });
                            write_hists[i].record(lat);
                            writer_ops[i].fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        thread_ex[i] = std::current_exception();
                        stop.store(true, std::memory_order_relaxed);
                    }
                });
            }

            // Reader threads
            for (int i = 0; i < K; ++i) {
                threads.emplace_back([&, i]{
                    try {
                        auto iter = backend.iterate(read_stream(i));
                        uint64_t seek_ts = 1;

                        latch.arrive_and_wait();

                        while (!stop.load(std::memory_order_relaxed)) {
                            double lat = measure_us([&]{
                                bool found = iter->find(seek_ts);
                                if (!found || !iter->valid()) {
                                    seek_ts = 1;
                                    return;
                                }
                                for (int f = 0; f < SCAN_FRAMES && iter->valid(); ++f)
                                    iter->next();
                                seek_ts += SCAN_FRAMES;
                                if (seek_ts + SCAN_FRAMES >= static_cast<uint64_t>(READ_PREFILL))
                                    seek_ts = 1;
                            });
                            read_hists[i].record(lat);
                            reader_ops[i].fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        thread_ex[K + i] = std::current_exception();
                        stop.store(true, std::memory_order_relaxed);
                    }
                });
            }

            std::this_thread::sleep_for(std::chrono::seconds(window_sec));
            stop.store(true, std::memory_order_relaxed);
            for (auto& t : threads) t.join();

            for (int i = 0; i < K * 2; ++i)
                if (thread_ex[i]) std::rethrow_exception(thread_ex[i]);

            for (int i = 0; i < K; ++i)
                sr.writer_ops += writer_ops[i].load(std::memory_order_relaxed);
            for (int i = 0; i < K; ++i)
                sr.reader_ops += reader_ops[i].load(std::memory_order_relaxed);

            sr.writer_ops_sec = static_cast<double>(sr.writer_ops) / window_sec;
            sr.reader_ops_sec = static_cast<double>(sr.reader_ops) / window_sec;

            // Merge write latency
            {
                LatencyHistogram m;
                m.reserve(static_cast<size_t>(K) * 4);
                for (int i = 0; i < K; ++i) {
                    auto s = write_hists[i].compute();
                    m.record(s.p50_us); m.record(s.p95_us);
                    m.record(s.p99_us); m.record(s.max_us);
                }
                sr.write_latency = m.compute();
            }
            // Merge read latency
            {
                LatencyHistogram m;
                m.reserve(static_cast<size_t>(K) * 4);
                for (int i = 0; i < K; ++i) {
                    auto s = read_hists[i].compute();
                    m.record(s.p50_us); m.record(s.p95_us);
                    m.record(s.p99_us); m.record(s.max_us);
                }
                sr.read_latency = m.compute();
            }

            subs.push_back(sr);

            if (!first) { writer_ss << " | "; reader_ss << " | "; }
            first = false;
            writer_ss << "k=" << K << ": " << static_cast<long long>(sr.writer_ops_sec) << " w/s";
            reader_ss << "k=" << K << ": " << static_cast<long long>(sr.reader_ops_sec) << " seeks/s";
        }

        result.workload_config["writer_scaling"] = writer_ss.str();
        result.workload_config["reader_scaling"] = reader_ss.str();

        emit_table(subs);

        // Aggregate at K=8.
        const SubResult& peak = subs.back();
        result.total_ops     = peak.writer_ops + peak.reader_ops;
        result.total_seconds = window_sec;
        result.ops_per_sec   = peak.writer_ops_sec + peak.reader_ops_sec;
        result.latency       = peak.write_latency;  // write latency is primary

        // thread_results: writer entries (role=0) then reader entries (role=1), one per K.
        for (const auto& sr : subs) {
            ThreadResult tw, tr;
            tw.thread_id     = sr.k; tw.role = 0;
            tw.total_ops     = sr.writer_ops;
            tw.total_seconds = window_sec;
            tw.ops_per_sec   = sr.writer_ops_sec;
            tw.latency       = sr.write_latency;
            result.thread_results.push_back(tw);

            tr.thread_id     = sr.k; tr.role = 1;
            tr.total_ops     = sr.reader_ops;
            tr.total_seconds = window_sec;
            tr.ops_per_sec   = sr.reader_ops_sec;
            tr.latency       = sr.read_latency;
            result.thread_results.push_back(tr);
        }

        return result;
    }
};

REGISTER_WORKLOAD(WriteReadContention, "write_read_contention");

} // namespace bench

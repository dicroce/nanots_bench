// Workload: concurrent_readers
//
// This is the headline workload for the nanots benchmark story.
//
// WHAT IT MEASURES:
//   One writer thread writes frames continuously to "stream_0" for the entire
//   duration.  K reader threads simultaneously perform seek+scan loops against
//   the same stream.  K sweeps over cfg.reader_counts (default: 1,2,4,8,16,32).
//
//   For each value of K we run an independent sub-experiment:
//     • Prefill the stream to a known baseline depth.
//     • Start all K readers + 1 writer simultaneously via a shared barrier.
//     • Run for a wall-clock window (default: 10 seconds per K value).
//     • Stop all threads cleanly.
//     • Record per-thread throughput and latency.
//
// RESULT FORMAT:
//   • thread_results[] contains one entry per thread (role="writer" or "reader").
//   • aggregate metrics (ops_per_sec etc.) reflect the writer.
//   • The JSON also includes a "reader_scaling" table in workload_config
//     that makes the writer-throughput-vs-reader-count relationship obvious
//     at a glance:
//
//       "reader_scaling": "k=1: 82345 w/s | k=2: 81902 w/s | k=4: 81100 w/s ..."
//
// SYNCHRONIZATION DESIGN:
//   • A std::barrier (C++20) synchronizes thread startup so all threads begin
//     measuring at exactly the same moment.  Falls back to a latch idiom on
//     C++17 compilers via a manual countdown latch.
//   • A std::atomic<bool> stop_flag drives graceful shutdown.
//   • Each thread owns its LatencyHistogram; no cross-thread sharing of mutable
//     state during the measurement window.
//   • The writer holds no lock between writes; backend.write() must be safe
//     for concurrent readers (same contract as nanots).

#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/timing.h"
#include "bench/workload.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Simple countdown latch (C++17 compatible; replaces std::barrier / std::latch)
// ---------------------------------------------------------------------------
class CountdownLatch {
public:
    explicit CountdownLatch(int n) : _count(n) {}

    // Called by each thread when it is ready.  Blocks until all threads arrive.
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lk(_mu);
        --_count;
        if (_count == 0) {
            _cv.notify_all();
        } else {
            _cv.wait(lk, [this]{ return _count == 0; });
        }
    }

private:
    std::mutex              _mu;
    std::condition_variable _cv;
    int                     _count;
};

// ---------------------------------------------------------------------------
// Per-sub-experiment result
// ---------------------------------------------------------------------------
struct SubResult {
    int                          k             = 0;
    double                       writer_ops_sec = 0;
    uint64_t                     writer_ops     = 0;
    std::vector<double>          reader_ops_sec; // one per reader thread
    std::vector<LatencyHistogram::Stats> reader_latency;
    LatencyHistogram::Stats      writer_latency;
};

// ---------------------------------------------------------------------------
// ConcurrentReaders workload
// ---------------------------------------------------------------------------
class ConcurrentReaders : public Workload {
    // Frames written before measurement begins.
    static constexpr uint64_t      PREFILL_FRAMES = 100'000;
    // Reader counts to sweep.
    static constexpr int           READER_COUNTS[] = {1, 2, 4, 8, 16, 32};

public:
    std::string name() const override { return "concurrent_readers"; }

    WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) override {
        WorkloadResult result;
        result.backend_name    = backend.name();
        result.backend_version = backend.version();
        result.backend_config  = backend.config_summary();
        result.workload_name   = name();
        result.fairness_axis   = cfg.fairness_axis;

        const int    window_sec    = 10;   // seconds per K value
        const size_t scan_frames   = 64;   // frames per reader seek+scan loop

        result.workload_config["window_sec_per_k"]  = std::to_string(window_sec);
        result.workload_config["scan_frames"]        = std::to_string(scan_frames);
        result.workload_config["prefill_frames"]     = std::to_string(PREFILL_FRAMES);
        result.workload_config["frame_size"]         = std::to_string(cfg.frame_size);
        result.workload_config["durability_bytes"]   = std::to_string(cfg.durability_bytes);
        result.workload_config["reader_counts"]      = "1,2,4,8,16,32";

        // Prefill so readers always have data to scan from frame 0.
        prefill(backend, cfg, PREFILL_FRAMES);

        std::vector<SubResult> sub_results;
        sub_results.reserve(std::size(READER_COUNTS));

        // Build a scaling summary string as we go.
        std::ostringstream scaling_table;
        bool first_k = true;

        double best_writer_ops_sec = 0;
        bool   have_baseline       = false;

        for (int k : READER_COUNTS) {
            SubResult sr = run_sub(backend, cfg, k, window_sec, scan_frames);
            sub_results.push_back(sr);

            if (!first_k) scaling_table << " | ";
            first_k = false;
            scaling_table << "k=" << k << ": "
                          << static_cast<long long>(sr.writer_ops_sec) << " w/s";

            if (!have_baseline) {
                best_writer_ops_sec = sr.writer_ops_sec;
                have_baseline = true;
            }
        }

        result.workload_config["reader_scaling"] = scaling_table.str();

        // Aggregate metrics: writer throughput at maximum reader count.
        const SubResult& heaviest = sub_results.back();
        result.total_ops     = heaviest.writer_ops;
        result.total_seconds = static_cast<double>(window_sec);
        result.ops_per_sec   = heaviest.writer_ops_sec;
        result.latency       = heaviest.writer_latency;

        // Per-thread results: last sub-experiment (highest K) for detail.
        result.thread_results.clear();
        {
            ThreadResult wr;
            wr.thread_id     = 0;
            wr.role          = 0;
            wr.total_ops     = heaviest.writer_ops;
            wr.total_seconds = static_cast<double>(window_sec);
            wr.ops_per_sec   = heaviest.writer_ops_sec;
            wr.latency       = heaviest.writer_latency;
            result.thread_results.push_back(wr);
        }
        for (int i = 0; i < static_cast<int>(heaviest.reader_ops_sec.size()); ++i) {
            ThreadResult rr;
            rr.thread_id     = i + 1;
            rr.role          = 1;
            rr.total_ops     = static_cast<uint64_t>(heaviest.reader_ops_sec[i] * window_sec);
            rr.total_seconds = static_cast<double>(window_sec);
            rr.ops_per_sec   = heaviest.reader_ops_sec[i];
            rr.latency       = (i < (int)heaviest.reader_latency.size())
                                 ? heaviest.reader_latency[i]
                                 : LatencyHistogram::Stats{};
            result.thread_results.push_back(rr);
        }

        emit_scaling_table(sub_results, best_writer_ops_sec);

        return result;
    }

private:
    // ----------------------------------------------------------------
    // Prefill phase: write |n| frames to "stream_0".
    // ----------------------------------------------------------------
    void prefill(Backend& backend, const WorkloadConfig& cfg, uint64_t n) {
        std::vector<uint8_t> payload(cfg.frame_size, 0xEF);
        size_t   bytes_since_sync = 0;
        uint64_t ts_us = 1'000'000;

        for (uint64_t i = 0; i < n; ++i) {
            Frame f{};
            f.data         = payload.data();
            f.size         = payload.size();
            f.timestamp_us = ts_us;
            f.flags        = 0;
            backend.write("stream_0", f);
            ts_us += 1'000;
            bytes_since_sync += cfg.frame_size;
            if (bytes_since_sync >= cfg.durability_bytes) {
                backend.sync_boundary(bytes_since_sync);
                bytes_since_sync = 0;
            }
        }
        if (bytes_since_sync > 0) backend.sync_boundary(bytes_since_sync);

        _next_write_ts.store(ts_us, std::memory_order_relaxed);
        _prefill_frames = n;
    }

    // ----------------------------------------------------------------
    // Run one sub-experiment with K readers + 1 writer for |window_sec|.
    // ----------------------------------------------------------------
    SubResult run_sub(Backend& backend, const WorkloadConfig& cfg,
                      int k, int window_sec, size_t scan_frames)
    {
        SubResult sr;
        sr.k = k;

        const int total_threads = k + 1; // k readers + 1 writer
        CountdownLatch start_latch(total_threads);
        std::atomic<bool> stop_flag{false};

        // Per-writer shared state
        std::atomic<uint64_t> writer_op_count{0};
        LatencyHistogram writer_hist;
        writer_hist.reserve(200'000);
        std::mutex writer_hist_mu; // only for final flush
        std::exception_ptr writer_ex;

        // Per-reader accumulators (indexed 0..k-1)
        std::vector<std::atomic<uint64_t>> reader_op_counts(k);
        for (auto& c : reader_op_counts) c.store(0, std::memory_order_relaxed);

        std::vector<LatencyHistogram> reader_hists(k);
        for (auto& h : reader_hists) h.reserve(50'000);

        // ----- Writer thread -----
        std::thread writer_thread([&]{
            try {
                std::vector<uint8_t> payload(cfg.frame_size, 0xAB);
                size_t bytes_since_sync = 0;

                start_latch.arrive_and_wait();  // sync start

                while (!stop_flag.load(std::memory_order_relaxed)) {
                    uint64_t ts = _next_write_ts.fetch_add(1'000, std::memory_order_relaxed);

                    Frame f{};
                    f.data         = payload.data();
                    f.size         = payload.size();
                    f.timestamp_us = ts;
                    f.flags        = 0;

                    double lat_us = measure_us([&]{ backend.write("stream_0", f); });
                    writer_hist.record(lat_us);
                    writer_op_count.fetch_add(1, std::memory_order_relaxed);

                    bytes_since_sync += cfg.frame_size;
                    if (bytes_since_sync >= cfg.durability_bytes) {
                        backend.sync_boundary(bytes_since_sync);
                        bytes_since_sync = 0;
                    }
                }
                if (bytes_since_sync > 0) backend.sync_boundary(bytes_since_sync);
            } catch (...) {
                writer_ex = std::current_exception();
                stop_flag.store(true, std::memory_order_relaxed);
            }
        });

        // ----- Reader threads -----
        std::vector<std::thread> reader_threads;
        reader_threads.reserve(k);
        for (int r = 0; r < k; ++r) {
            reader_threads.emplace_back([&, r]{
                std::mt19937_64 rng(static_cast<uint64_t>(r) * 6364136223846793005ULL + 1);

                start_latch.arrive_and_wait();  // sync start

                auto iter = backend.iterate("stream_0");
                uint64_t max_ts = 1'000'000 + (_prefill_frames - scan_frames - 1) * 1'000;
                std::uniform_int_distribution<uint64_t> dist(1'000'000, max_ts);

                while (!stop_flag.load(std::memory_order_relaxed)) {
                    uint64_t target_ts = dist(rng);

                    double lat_us = measure_us([&]{
                        if (iter->find(target_ts)) {
                            for (size_t n = 0; n < scan_frames && iter->valid(); ++n) {
                                (void)iter->current();
                                iter->next();
                            }
                        }
                    });
                    reader_hists[r].record(lat_us);
                    reader_op_counts[r].fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Let the experiment run for |window_sec| seconds.
        std::this_thread::sleep_for(std::chrono::seconds(window_sec));
        stop_flag.store(true, std::memory_order_relaxed);

        // Join all threads.
        writer_thread.join();
        for (auto& t : reader_threads) t.join();

        if (writer_ex) std::rethrow_exception(writer_ex);

        // Collect results.
        sr.writer_ops     = writer_op_count.load(std::memory_order_relaxed);
        sr.writer_ops_sec = static_cast<double>(sr.writer_ops) / static_cast<double>(window_sec);
        sr.writer_latency = writer_hist.compute();

        for (int r = 0; r < k; ++r) {
            uint64_t rops = reader_op_counts[r].load(std::memory_order_relaxed);
            sr.reader_ops_sec.push_back(
                static_cast<double>(rops) / static_cast<double>(window_sec));
            sr.reader_latency.push_back(reader_hists[r].compute());
        }

        return sr;
    }

    // ----------------------------------------------------------------
    static void emit_scaling_table(const std::vector<SubResult>& srs,
                                   double baseline_ops_sec)
    {
        // Print to stderr so it appears even when stdout is redirected to JSON.
        fprintf(stderr, "\n%-6s  %-14s  %-10s  %s\n",
                "K", "writer w/s", "vs K=min", "avg reader seeks/s");
        fprintf(stderr, "------  --------------  ----------  ------------------\n");

        for (size_t i = 0; i < srs.size(); ++i) {
            const auto& sr = srs[i];
            double pct = (baseline_ops_sec > 0)
                ? (sr.writer_ops_sec / baseline_ops_sec * 100.0) : 0.0;

            double avg_reader = 0;
            if (!sr.reader_ops_sec.empty()) {
                for (double r : sr.reader_ops_sec) avg_reader += r;
                avg_reader /= static_cast<double>(sr.reader_ops_sec.size());
            }

            fprintf(stderr, "%-6d  %-14.0f  %8.1f%%  %.0f\n",
                    sr.k, sr.writer_ops_sec, pct, avg_reader);
        }
        fprintf(stderr, "\n");
    }

    static std::string reader_counts_str(const std::vector<int>& v) {
        std::ostringstream ss;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) ss << ",";
            ss << v[i];
        }
        return ss.str();
    }

    // Shared monotonic timestamp counter (microseconds).
    // Writer fetches-and-adds atomically; initial value set by prefill().
    std::atomic<uint64_t> _next_write_ts{1'000'000};
    uint64_t              _prefill_frames{0};
};

REGISTER_WORKLOAD(ConcurrentReaders, "concurrent_readers");

} // namespace bench

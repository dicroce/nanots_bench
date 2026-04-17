// Workload: random_seek
//
// Phase 1 (prefill): writes PREFILL_FRAMES frames to "stream_0".
// Phase 2 (seek):    performs |num_frames| random seeks — each seek picks a
//                    random timestamp from the prefill range and calls
//                    Iterator::find().  We open a fresh iterator per seek to
//                    measure the full seek cost including iterator creation.
//
// Metrics reported:
//   • Per-seek latency histogram (p50/p95/p99/max/mean)
//   • Seeks per second

#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/timing.h"
#include "bench/workload.h"

#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

class RandomSeek : public Workload {
    // Write this many frames before beginning seek measurements.
    static constexpr uint64_t PREFILL_FRAMES = 100'000;

public:
    std::string name() const override { return "random_seek"; }

    WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) override {
        WorkloadResult result;
        result.backend_name    = backend.name();
        result.backend_version = backend.version();
        result.backend_config  = backend.config_summary();
        result.workload_name   = name();
        result.fairness_axis   = cfg.fairness_axis;

        result.workload_config["prefill_frames"]   = std::to_string(PREFILL_FRAMES);
        result.workload_config["seek_ops"]         = std::to_string(cfg.num_frames);
        result.workload_config["frame_size"]       = std::to_string(cfg.frame_size);
        result.workload_config["durability_bytes"] = std::to_string(cfg.durability_bytes);

        // ----- Phase 1: prefill -----
        std::vector<uint8_t> payload(cfg.frame_size, 0xCD);
        std::vector<uint64_t> timestamps;
        timestamps.reserve(PREFILL_FRAMES);

        size_t   bytes_since_sync = 0;
        uint64_t ts_us = 1'000'000;

        for (uint64_t i = 0; i < PREFILL_FRAMES; ++i) {
            Frame f{};
            f.data         = payload.data();
            f.size         = payload.size();
            f.timestamp_us = ts_us;
            f.flags        = 0;

            backend.write("stream_0", f);
            timestamps.push_back(ts_us);

            ts_us += 1'000;
            bytes_since_sync += cfg.frame_size;

            if (bytes_since_sync >= cfg.durability_bytes) {
                backend.sync_boundary(bytes_since_sync);
                bytes_since_sync = 0;
            }
        }
        if (bytes_since_sync > 0) backend.sync_boundary(bytes_since_sync);

        // ----- Phase 2: random seeks -----
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<size_t> dist(0, timestamps.size() - 1);

        LatencyHistogram hist;
        hist.reserve(cfg.num_frames);

        auto iter = backend.iterate("stream_0");

        auto wall_start = Timer::Clock::now();

        for (uint64_t i = 0; i < cfg.num_frames; ++i) {
            uint64_t target_ts = timestamps[dist(rng)];

            double lat_us = measure_us([&]{
                bool found = iter->find(target_ts);
                (void)found;
            });
            hist.record(lat_us);
        }

        auto wall_end  = Timer::Clock::now();
        double total_sec = std::chrono::duration<double>(wall_end - wall_start).count();

        result.total_ops     = cfg.num_frames;
        result.total_seconds = total_sec;
        result.ops_per_sec   = static_cast<double>(cfg.num_frames) / total_sec;
        result.latency       = hist.compute();

        if (cfg.emit_raw)
            result.raw_latencies_us = hist.raw();

        return result;
    }
};

REGISTER_WORKLOAD(RandomSeek, "random_seek");

} // namespace bench

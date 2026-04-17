// Workload: sustained_write
//
// Single writer.  Writes |num_frames| frames of |frame_size| bytes to a
// single stream "stream_0".  Calls backend.sync_boundary() every time
// cumulative bytes written crosses a multiple of |durability_bytes|.
//
// Metrics reported:
//   • Per-write latency histogram (p50/p95/p99/max/mean)
//   • Total throughput (writes/sec, MB/sec)

#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/timing.h"
#include "bench/workload.h"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

class SustainedWrite : public Workload {
public:
    std::string name() const override { return "sustained_write"; }

    WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) override {
        WorkloadResult result;
        result.backend_name    = backend.name();
        result.backend_version = backend.version();
        result.backend_config  = backend.config_summary();
        result.workload_name   = name();
        result.fairness_axis   = cfg.fairness_axis;

        // Snapshot config into result
        result.workload_config["num_frames"]       = std::to_string(cfg.num_frames);
        result.workload_config["frame_size"]       = std::to_string(cfg.frame_size);
        result.workload_config["durability_bytes"] = std::to_string(cfg.durability_bytes);

        // Build a fixed payload of frame_size bytes.
        std::vector<uint8_t> payload(cfg.frame_size, 0xAB);

        LatencyHistogram hist;
        hist.reserve(cfg.num_frames);

        size_t   bytes_since_sync = 0;
        uint64_t ts_us            = 1'000'000;  // start at t=1s so seeks can go before it

        auto wall_start = Timer::Clock::now();

        for (uint64_t i = 0; i < cfg.num_frames; ++i) {
            Frame f{};
            f.data         = payload.data();
            f.size         = payload.size();
            f.timestamp_us = ts_us;
            f.flags        = 0;

            double lat_us = measure_us([&]{ backend.write("stream_0", f); });
            hist.record(lat_us);

            ts_us         += 1'000;   // 1 ms between frames
            bytes_since_sync += cfg.frame_size;

            if (bytes_since_sync >= cfg.durability_bytes) {
                backend.sync_boundary(bytes_since_sync);
                bytes_since_sync = 0;
            }
        }

        // Final sync to flush the last partial window
        if (bytes_since_sync > 0) {
            backend.sync_boundary(bytes_since_sync);
        }

        auto wall_end = Timer::Clock::now();
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

REGISTER_WORKLOAD(SustainedWrite, "sustained_write");

} // namespace bench

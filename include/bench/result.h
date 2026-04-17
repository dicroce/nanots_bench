#pragma once
#include "timing.h"
#include <map>
#include <string>
#include <vector>

namespace bench {

// Per-thread throughput record (used by concurrent_readers).
struct ThreadResult {
    int    thread_id       = 0;
    int    role            = 0;      // 0=writer, 1=reader
    double ops_per_sec     = 0;
    double total_seconds   = 0;
    uint64_t total_ops     = 0;
    LatencyHistogram::Stats latency;
};

// A complete result for one (workload × backend × fairness-axis) run.
struct WorkloadResult {
    // --- identity ---
    std::string backend_name;
    std::string backend_version;
    std::string backend_config;

    std::string workload_name;
    std::string fairness_axis;

    // --- workload config snapshot (key=value pairs for JSON) ---
    std::map<std::string, std::string> workload_config;

    // --- environment (filled by runner) ---
    std::string env_cpu;
    std::string env_os;
    std::string env_compiler;
    std::string env_timestamp;
    std::string env_flush_mechanism; // OS-level sync primitive used at durability boundaries

    // --- aggregate metrics ---
    uint64_t total_ops     = 0;
    double   total_seconds = 0;
    double   ops_per_sec   = 0;
    LatencyHistogram::Stats latency;

    // --- per-thread breakdown (concurrent_readers only) ---
    std::vector<ThreadResult> thread_results;

    // --- raw samples (gated on emit_raw) ---
    std::vector<double> raw_latencies_us;

    // --- error (non-empty on failure) ---
    std::string error;
};

// Serialize a WorkloadResult to a JSON string.
std::string to_json(const WorkloadResult& r);

// Write JSON to a file, or stdout if path is empty.
void write_result(const WorkloadResult& r, const std::string& path);

} // namespace bench

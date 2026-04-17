#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

// CLI-driven parameters shared across all workloads.
struct WorkloadConfig {
    uint64_t num_frames       = 100'000;   // frames for sustained_write; prefill hint for others
    uint32_t frame_size       = 4096;      // bytes per frame payload
    size_t   durability_bytes = 10 * 1024 * 1024; // bytes between sync_boundary calls

    // Output control
    bool     emit_raw         = false;     // include raw latency array in JSON
    std::string fairness_axis = "";        // optional result tag
    std::string output_path   = "";        // write JSON here (empty = stdout)
};

// Forward declaration.
class Backend;
struct WorkloadResult;

// Abstract workload.  One implementation per benchmark scenario.
class Workload {
public:
    virtual ~Workload() = default;

    virtual std::string    name() const = 0;
    virtual WorkloadResult run(Backend& backend, const WorkloadConfig& cfg) = 0;
};

} // namespace bench

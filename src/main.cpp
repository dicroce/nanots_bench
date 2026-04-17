#include "bench/backend.h"
#include "bench/registry.h"
#include "bench/result.h"
#include "bench/workload.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Defined in runner.cpp
namespace bench { void fill_environment(WorkloadResult&); }

// ---------------------------------------------------------------------------
// Minimal argument parser
// ---------------------------------------------------------------------------
struct Args {
    std::string backend;
    std::string workload;
    std::string output;
    std::string fairness_axis;
    std::string sqlite_synchronous = "normal";
    std::string data_dir           = "./bench_data";
    uint32_t    nanots_num_blocks  = 0;
    bool        nanots_auto_reclaim = false;

    // 10 MB default block size puts nanots in its sweet spot (large blocks →
    // few fsyncs → high throughput) and gives SQLite WAL a comparable window.
    uint64_t num_frames       = 100'000;
    uint32_t frame_size       = 4096;
    size_t   durability_bytes = 10 * 1024 * 1024;

    bool emit_raw        = false;
    bool list_backends   = false;
    bool list_workloads  = false;
    bool help            = false;
};

static void usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Selection:\n"
        "  --backend <name>            Backend to use (see --list-backends)\n"
        "  --workload <name>           Workload to run (see --list-workloads)\n"
        "\n"
        "Workload parameters:\n"
        "  --num-frames <N>            Frames for sustained_write; prefill hint for\n"
        "                              seek/reader workloads  [default: 100000]\n"
        "  --frame-size <bytes>        Payload bytes per frame [default: 4096]\n"
        "  --durability-bytes <bytes>  Durability window: nanots block size, and how\n"
        "                              often sync_boundary() is called for SQLite\n"
        "                              [default: 10485760 = 10 MB]\n"
        "\n"
        "Backend configuration:\n"
        "  --sqlite-synchronous <mode> off | normal | full | extra [default: normal]\n"
        "  --nanots-num-blocks <N>     nanots block count (0 = auto-derive) [default: 0]\n"
        "  --nanots-auto-reclaim       Recycle oldest blocks when full — required for\n"
        "                              time-based workloads (multi_stream_write,\n"
        "                              write_read_contention)\n"
        "  --data-dir <path>           Where to create the database [default: ./bench_data]\n"
        "\n"
        "Output:\n"
        "  --output <path>             Write JSON result here (default: stdout)\n"
        "  --fairness-axis <label>     Optional tag stored in result JSON\n"
        "  --emit-raw                  Include raw per-op latency array in JSON\n"
        "\n"
        "Info:\n"
        "  --list-backends\n"
        "  --list-workloads\n"
        "  --help\n";
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (flag == "--backend")              a.backend              = next();
        else if (flag == "--workload")             a.workload             = next();
        else if (flag == "--output")               a.output               = next();
        else if (flag == "--fairness-axis")        a.fairness_axis        = next();
        else if (flag == "--sqlite-synchronous")   a.sqlite_synchronous   = next();
        else if (flag == "--data-dir")             a.data_dir             = next();
        else if (flag == "--num-frames")           a.num_frames           = std::stoull(next());
        else if (flag == "--frame-size")           a.frame_size           = static_cast<uint32_t>(std::stoul(next()));
        else if (flag == "--durability-bytes")     a.durability_bytes     = std::stoull(next());
        else if (flag == "--nanots-num-blocks")    a.nanots_num_blocks    = static_cast<uint32_t>(std::stoul(next()));
        else if (flag == "--nanots-auto-reclaim")  a.nanots_auto_reclaim  = true;
        else if (flag == "--emit-raw")             a.emit_raw             = true;
        else if (flag == "--list-backends")        a.list_backends        = true;
        else if (flag == "--list-workloads")       a.list_workloads       = true;
        else if (flag == "--help" || flag == "-h") a.help                 = true;
        else {
            std::cerr << "Unknown argument: " << flag << "\n";
            std::exit(1);
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    auto& breg = bench::BackendRegistry::instance();
    auto& wreg = bench::WorkloadRegistry::instance();

    if (a.help) {
        usage(argv[0]);
        return 0;
    }

    if (a.list_backends) {
        std::cout << "Available backends:\n";
        for (const auto& n : breg.names()) std::cout << "  " << n << "\n";
        return 0;
    }

    if (a.list_workloads) {
        std::cout << "Available workloads:\n";
        for (const auto& n : wreg.names()) std::cout << "  " << n << "\n";
        return 0;
    }

    if (a.backend.empty()) { std::cerr << "--backend is required\n"; return 1; }
    if (a.workload.empty()) { std::cerr << "--workload is required\n"; return 1; }

    // Build backend config
    bench::BackendConfig bcfg;
    bcfg.durability_bytes      = a.durability_bytes;
    bcfg.sqlite_synchronous    = a.sqlite_synchronous;
    bcfg.nanots_num_blocks     = a.nanots_num_blocks;
    bcfg.nanots_auto_reclaim   = a.nanots_auto_reclaim;
    // Size the nanots file generously: num_frames × frame_size × 4 safety factor.
    bcfg.expected_write_bytes  = a.num_frames * a.frame_size * 4;

    // Build workload config
    bench::WorkloadConfig wcfg;
    wcfg.num_frames       = a.num_frames;
    wcfg.frame_size       = a.frame_size;
    wcfg.durability_bytes = a.durability_bytes;
    wcfg.emit_raw         = a.emit_raw;
    wcfg.fairness_axis    = a.fairness_axis;
    wcfg.output_path      = a.output;

    // Create backend and workload
    std::unique_ptr<bench::Backend>  backend;
    std::unique_ptr<bench::Workload> workload;

    try {
        backend  = breg.create(a.backend);
        workload = wreg.create(a.workload);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // Set up data directory
    {
#if defined(_WIN32)
        std::string cmd = "mkdir \"" + a.data_dir + "\" 2>nul";
#else
        std::string cmd = "mkdir -p \"" + a.data_dir + "\"";
#endif
        (void)std::system(cmd.c_str());
    }

    std::string db_path = a.data_dir + "/" + a.backend + "_" + a.workload;

    // Setup
    try {
        backend->setup(db_path, bcfg);
    } catch (const std::exception& e) {
        std::cerr << "Backend setup failed: " << e.what() << "\n";
        return 1;
    }

    // Run
    bench::WorkloadResult result;
    try {
        result = workload->run(*backend, wcfg);
    } catch (const std::exception& e) {
        result.error = e.what();
        std::cerr << "Workload error: " << e.what() << "\n";
    } catch (...) {
        result.error = "unknown exception";
        std::cerr << "Workload: unknown exception type\n";
    }

    // Teardown
    try {
        backend->teardown();
    } catch (const std::exception& e) {
        std::cerr << "Backend teardown failed: " << e.what() << "\n";
    }

    // Fill environment and write result
    bench::fill_environment(result);
    result.fairness_axis = a.fairness_axis;
    bench::write_result(result, a.output);

    return result.error.empty() ? 0 : 1;
}

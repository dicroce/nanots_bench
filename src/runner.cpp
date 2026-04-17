#include "bench/result.h"
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Environment detection helpers (no external deps)
// ---------------------------------------------------------------------------

namespace bench {

// Build a human-readable timestamp string (UTC).
std::string current_timestamp() {
    auto now  = std::chrono::system_clock::now();
    auto tt   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Detect OS string at compile time.
std::string detect_os() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

// Detect compiler string at compile time.
std::string detect_compiler() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "Unknown";
#endif
}

// Best-effort CPU string.  Reads /proc/cpuinfo on Linux, falls back to
// compile-time arch elsewhere.
std::string detect_cpu() {
#if defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            if (s.rfind("model name", 0) == 0) {
                fclose(f);
                auto pos = s.find(':');
                if (pos != std::string::npos) {
                    auto val = s.substr(pos + 2);
                    // strip trailing newline
                    while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
                        val.pop_back();
                    return val;
                }
            }
        }
        fclose(f);
    }
#endif
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64 (details unavailable)";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "ARM64 (details unavailable)";
#else
    return "Unknown arch";
#endif
}

// Detect the OS-level sync primitive used at durability boundaries.
// nanots calls nts_memory_map::flush(..., true) at every block boundary, which
// translates to the platform primitive below.  Both backends ultimately route
// through the same OS call, so this field is per-platform, not per-backend.
std::string detect_flush_mechanism() {
#if defined(_WIN32)
    return "FlushFileBuffers";
#elif defined(__APPLE__)
    // nanots uses msync(MS_SYNC) on macOS (no F_FULLFSYNC in nts_memory_map).
    return "msync(MS_SYNC)";
#else
    return "msync(MS_SYNC)";
#endif
}

// Populate the environment fields of a WorkloadResult.
void fill_environment(WorkloadResult& r) {
    r.env_cpu             = detect_cpu();
    r.env_os              = detect_os();
    r.env_compiler        = detect_compiler();
    r.env_timestamp       = current_timestamp();
    r.env_flush_mechanism = detect_flush_mechanism();
}

} // namespace bench

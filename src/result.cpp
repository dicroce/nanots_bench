#include "bench/result.h"
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace bench {

// ---------------------------------------------------------------------------
// Minimal JSON builder — no external deps.
// ---------------------------------------------------------------------------

static std::string json_str(const std::string& s) {
    // Escape backslash and double-quote; everything else passes through.
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else { out += c; }
    }
    out += '"';
    return out;
}

static std::string json_double(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << v;
    return ss.str();
}

static std::string latency_block(const LatencyHistogram::Stats& s, int indent) {
    std::string pad(indent, ' ');
    std::ostringstream o;
    o << "{\n"
      << pad << "  \"mean\": "  << json_double(s.mean_us) << ",\n"
      << pad << "  \"p50\":  "  << json_double(s.p50_us)  << ",\n"
      << pad << "  \"p95\":  "  << json_double(s.p95_us)  << ",\n"
      << pad << "  \"p99\":  "  << json_double(s.p99_us)  << ",\n"
      << pad << "  \"max\":  "  << json_double(s.max_us)  << "\n"
      << pad << "}";
    return o.str();
}

std::string to_json(const WorkloadResult& r) {
    std::ostringstream o;
    o << "{\n";

    // backend
    o << "  \"backend\": {\n"
      << "    \"name\":    " << json_str(r.backend_name)    << ",\n"
      << "    \"version\": " << json_str(r.backend_version) << ",\n"
      << "    \"config\":  " << json_str(r.backend_config)  << "\n"
      << "  },\n";

    // workload
    o << "  \"workload\": {\n"
      << "    \"name\": " << json_str(r.workload_name) << ",\n"
      << "    \"config\": {\n";
    bool first = true;
    for (const auto& [k, v] : r.workload_config) {
        if (!first) o << ",\n";
        first = false;
        o << "      " << json_str(k) << ": " << json_str(v);
    }
    o << "\n    }\n  },\n";

    // fairness axis
    o << "  \"fairness_axis\": " << json_str(r.fairness_axis) << ",\n";

    // environment
    o << "  \"environment\": {\n"
      << "    \"cpu\":             " << json_str(r.env_cpu)             << ",\n"
      << "    \"os\":              " << json_str(r.env_os)              << ",\n"
      << "    \"compiler\":        " << json_str(r.env_compiler)        << ",\n"
      << "    \"flush_mechanism\": " << json_str(r.env_flush_mechanism) << ",\n"
      << "    \"timestamp\":       " << json_str(r.env_timestamp)       << "\n"
      << "  },\n";

    // error (if any)
    if (!r.error.empty()) {
        o << "  \"error\": " << json_str(r.error) << ",\n";
    }

    // aggregate metrics
    o << "  \"metrics\": {\n"
      << "    \"total_ops\":     " << r.total_ops     << ",\n"
      << "    \"total_seconds\": " << json_double(r.total_seconds) << ",\n"
      << "    \"ops_per_sec\":   " << json_double(r.ops_per_sec)   << ",\n"
      << "    \"latency_us\":    " << latency_block(r.latency, 4)  << "\n"
      << "  }";

    // per-thread breakdown
    if (!r.thread_results.empty()) {
        o << ",\n  \"thread_results\": [\n";
        for (size_t i = 0; i < r.thread_results.size(); ++i) {
            const auto& t = r.thread_results[i];
            o << "    {\n"
              << "      \"thread_id\":   " << t.thread_id << ",\n"
              << "      \"role\":        " << json_str(t.role == 0 ? "writer" : "reader") << ",\n"
              << "      \"total_ops\":   " << t.total_ops << ",\n"
              << "      \"total_sec\":   " << json_double(t.total_seconds) << ",\n"
              << "      \"ops_per_sec\": " << json_double(t.ops_per_sec) << ",\n"
              << "      \"latency_us\":  " << latency_block(t.latency, 6) << "\n"
              << "    }";
            if (i + 1 < r.thread_results.size()) o << ",";
            o << "\n";
        }
        o << "  ]";
    }

    // raw latencies
    if (!r.raw_latencies_us.empty()) {
        o << ",\n  \"raw_latencies_us\": [";
        for (size_t i = 0; i < r.raw_latencies_us.size(); ++i) {
            if (i) o << ",";
            o << json_double(r.raw_latencies_us[i]);
        }
        o << "]";
    }

    o << "\n}\n";
    return o.str();
}

void write_result(const WorkloadResult& r, const std::string& path) {
    std::string json = to_json(r);
    if (path.empty()) {
        std::cout << json;
    } else {
        std::ofstream f(path);
        if (!f) {
            std::cerr << "Error: cannot open output file: " << path << "\n";
            std::cout << json;
            return;
        }
        f << json;
        std::cerr << "Result written to: " << path << "\n";
    }
}

} // namespace bench

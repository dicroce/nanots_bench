#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace bench {

// High-resolution timer.  Returns elapsed time in microseconds.
class Timer {
public:
    using Clock = std::chrono::steady_clock;

    void start() { _t0 = Clock::now(); }
    void stop()  { _t1 = Clock::now(); }

    double elapsed_us() const {
        return std::chrono::duration<double, std::micro>(_t1 - _t0).count();
    }
    double elapsed_sec() const {
        return std::chrono::duration<double>(_t1 - _t0).count();
    }

    // RAII scoped measurement: stores result in *out_us on destruction.
    struct Scope {
        Scope(Timer& t, double* out_us) : _t(t), _out(out_us) { _t.start(); }
        ~Scope() { _t.stop(); if (_out) *_out = _t.elapsed_us(); }
    private:
        Timer&  _t;
        double* _out;
    };

private:
    Clock::time_point _t0, _t1;
};

// Inline single-shot measurement returning microseconds.
template<typename Fn>
inline double measure_us(Fn&& fn) {
    auto t0 = Timer::Clock::now();
    fn();
    auto t1 = Timer::Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// LatencyHistogram: collects per-operation latencies and computes percentiles.
// Uses a sorted-vector approach so we can report accurate percentiles without
// a dependency on HdrHistogram.  For very large runs (>50M ops) callers should
// set emit_raw=false; the histogram is still accurate.
class LatencyHistogram {
public:
    void reserve(size_t n)         { _samples.reserve(n); }
    void record(double latency_us) { _samples.push_back(latency_us); }
    size_t count() const           { return _samples.size(); }

    struct Stats {
        double mean_us = 0;
        double p50_us  = 0;
        double p95_us  = 0;
        double p99_us  = 0;
        double max_us  = 0;
    };

    // Must be called before querying percentiles.  Sorts the sample vector.
    Stats compute() {
        if (_samples.empty()) return {};
        std::sort(_samples.begin(), _samples.end());

        Stats s;
        double sum = 0;
        for (double v : _samples) sum += v;
        s.mean_us = sum / static_cast<double>(_samples.size());
        s.p50_us  = percentile(0.50);
        s.p95_us  = percentile(0.95);
        s.p99_us  = percentile(0.99);
        s.max_us  = _samples.back();
        return s;
    }

    const std::vector<double>& raw() const { return _samples; }

private:
    double percentile(double p) const {
        if (_samples.empty()) return 0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(_samples.size() - 1));
        return _samples[idx];
    }

    std::vector<double> _samples;
};

} // namespace bench

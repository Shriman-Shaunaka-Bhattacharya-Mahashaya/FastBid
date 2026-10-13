#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "memory/spsc_queue.hpp"
namespace fastbid {
struct LatencyStats { uint64_t min_ns{}, max_ns{}, mean_ns{}, p50_ns{}, p99_ns{}, p999_ns{}, sample_count{}, orders_per_sec{}; };
// Engine records, IO computes. Samples are transferred, never concurrently reset.
// Diagnostic samples may drop under pressure; execution reports may not.
template <std::size_t MaxSamples = 65536>
class StatsCollector {
public:
    void record(uint64_t ns) noexcept { if (!samples_.try_push(ns)) dropped_.fetch_add(1, std::memory_order_relaxed); }
    LatencyStats compute() noexcept {
        std::size_t n = 0;
        while (n < MaxSamples && samples_.try_pop(sorted_[n])) ++n;
        LatencyStats s{}; if (!n) return s;
        std::sort(sorted_, sorted_ + n);
        s.sample_count = n; s.min_ns = sorted_[0]; s.max_ns = sorted_[n - 1];
        s.p50_ns = sorted_[(n - 1) * 50 / 100]; s.p99_ns = sorted_[(n - 1) * 99 / 100]; s.p999_ns = sorted_[(n - 1) * 999 / 1000];
        long double sum = 0; for (std::size_t i = 0; i < n; ++i) sum += sorted_[i];
        s.mean_ns = static_cast<uint64_t>(sum / n); return s;
    }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    SPSCQueue<uint64_t, MaxSamples> samples_;
    uint64_t sorted_[MaxSamples]{};
    std::atomic<uint64_t> dropped_{0};
};
}

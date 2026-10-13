#pragma once

/// @file cycle_clock.hpp
/// @brief High-resolution cycle counter abstraction.
///
/// Provides a unified interface for nanosecond-precision timing across
/// x86 (RDTSC) and ARM (CNTVCT_EL0) architectures. Falls back to
/// clock_gettime(CLOCK_MONOTONIC_RAW) when hardware counters are unavailable.
///
/// Usage:
///   auto start = CycleClock::now();
///   // ... critical path ...
///   auto end = CycleClock::now();
///   auto ns = CycleClock::to_nanoseconds(end - start);

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
    #include <x86intrin.h>
    #define FASTBID_ARCH_X86 1
#elif defined(__aarch64__)
    #define FASTBID_ARCH_ARM 1
#endif

namespace fastbid {

class CycleClock {
public:
    using Tick = uint64_t;

    /// @brief Read the current cycle counter.
    /// 
    /// On x86: RDTSC (serialized with RDTSCP for accuracy).
    /// On ARM: CNTVCT_EL0 register.
    /// Fallback: clock_gettime(CLOCK_MONOTONIC_RAW) returning nanoseconds directly.
    [[nodiscard]] static inline Tick now() noexcept {
#if defined(FASTBID_ARCH_X86)
        unsigned int aux;
        auto ticks = __rdtscp(&aux);
        _mm_lfence();
        return ticks;  // Serializing RDTSC — waits for prior instructions
#elif defined(FASTBID_ARCH_ARM)
        uint64_t val;
        asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val) :: "memory");
        return val;
#else
        // Fallback: wall-clock nanoseconds
        auto tp = std::chrono::steady_clock::now();
        return static_cast<Tick>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                tp.time_since_epoch()).count());
#endif
    }

    /// @brief Calibrate the cycle counter to nanoseconds.
    ///
    /// Call once at startup. Measures the relationship between cycle ticks
    /// and wall-clock time to enable tick-to-nanosecond conversion.
    static void calibrate() noexcept {
#if defined(FASTBID_ARCH_X86) || defined(FASTBID_ARCH_ARM)
        // Measure ticks over a known wall-clock duration
        auto wall_start = std::chrono::steady_clock::now();
        Tick tick_start = now();

        // Busy-wait for ~10ms to calibrate
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        Tick tick_end = now();
        auto wall_end = std::chrono::steady_clock::now();

        auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            wall_end - wall_start).count();
        auto ticks = tick_end - tick_start;

        if (ticks > 0 && wall_ns > 0) {
            // ns_per_tick = wall_ns / ticks (fixed-point: multiply by 2^32 for precision)
            ns_per_tick_fp_ = static_cast<uint64_t>((static_cast<__uint128_t>(wall_ns) << 32) / ticks);
        }
        calibrated_ = true;
#else
        // Fallback mode: now() already returns nanoseconds directly
        ns_per_tick_fp_ = 1ULL << 32;  // 1.0 in fixed-point
        calibrated_ = true;
#endif
    }

    /// @brief Convert a tick delta to nanoseconds.
    /// @param ticks The number of ticks (from now() difference).
    /// @return Approximate nanoseconds.
    [[nodiscard]] static inline uint64_t to_nanoseconds(Tick ticks) noexcept {
#if defined(FASTBID_ARCH_X86) || defined(FASTBID_ARCH_ARM)
        // Fixed-point multiplication: (ticks * ns_per_tick_fp) >> 32
        return static_cast<uint64_t>((static_cast<__uint128_t>(ticks) * ns_per_tick_fp_) >> 32);
#else
        return ticks;  // Already in nanoseconds
#endif
    }

    /// @brief Check if the clock has been calibrated.
    [[nodiscard]] static bool is_calibrated() noexcept { return calibrated_; }

private:
    /// Nanoseconds per tick in 32.32 fixed-point format.
    /// Initialized by calibrate().
    static inline uint64_t ns_per_tick_fp_ = 1ULL << 32;

    /// Whether calibrate() has been called.
    static inline bool calibrated_ = false;
};

} // namespace fastbid

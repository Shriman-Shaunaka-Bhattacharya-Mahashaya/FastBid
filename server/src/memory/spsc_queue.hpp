#pragma once

/// @file spsc_queue.hpp
/// @brief Lock-free single-producer, single-consumer (SPSC) ring buffer.
///
/// Zero-allocation, fixed-capacity circular buffer for passing data between
/// exactly two threads without locks. Uses std::atomic with relaxed memory
/// ordering (acquire/release) for index synchronization.
///
/// Thread Safety:
/// - Exactly ONE thread may call try_push() (the producer / IO thread).
/// - Exactly ONE thread may call try_pop() (the consumer / engine thread).
/// - No other synchronization is needed.
///
/// @tparam T        Element type (must be trivially copyable for zero-copy semantics).
/// @tparam Capacity Number of slots. MUST be a power of 2 (enforced at compile time).

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>

#include "core/types.hpp"

namespace fastbid {

template <typename T, std::size_t Capacity>
class SPSCQueue {
public:
    static_assert(Capacity > 1, "SPSCQueue capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SPSCQueue capacity must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue element type must be trivially copyable");

    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    SPSCQueue() noexcept : tail_{0}, head_{0} {}

    // Producer-only: consumer progress can only increase this value.
    [[nodiscard]] std::size_t available() const noexcept {
        const auto t = tail_.load(std::memory_order_relaxed);
        const auto h = head_.load(std::memory_order_acquire);
        return (h - t - 1) & MASK;
    }

    // Non-copyable, non-movable
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    /// @brief Try to push an element into the queue (producer only).
    /// @param item The item to enqueue.
    /// @return true if the item was enqueued, false if the queue is full.
    ///
    /// Non-blocking. O(1). Called only by the producer thread.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const auto current_tail = tail_.load(std::memory_order_relaxed);
        const auto next_tail = increment(current_tail);

        // Check if queue is full: next write position would overlap the read position.
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }

        buffer_[current_tail] = item;

        // Publish the new tail. The release fence ensures the buffer write
        // above is visible to the consumer before it sees the updated tail.
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// @brief Try to pop an element from the queue (consumer only).
    /// @param item Output parameter — populated with the dequeued item on success.
    /// @return true if an item was dequeued, false if the queue is empty.
    ///
    /// Non-blocking. O(1). Called only by the consumer thread.
    [[nodiscard]] bool try_pop(T& item) noexcept {
        const auto current_head = head_.load(std::memory_order_relaxed);

        // Check if queue is empty: read position equals write position.
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Queue empty
        }

        item = buffer_[current_head];

        // Publish the new head. The release fence ensures the buffer read
        // above completes before the producer sees the freed slot.
        head_.store(increment(current_head), std::memory_order_release);
        return true;
    }

    /// @brief Check if the queue is empty (approximate — may race with producer).
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /// @brief Approximate number of items in the queue (may race).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return (t - h) & MASK;
    }

    /// @brief Total capacity of the queue.
    /// Usable capacity is Capacity - 1 (one slot is always reserved to
    /// distinguish full from empty).
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t MASK = Capacity - 1;

    /// Increment an index with wrap-around (branchless via bitmask).
    [[nodiscard]] static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) & MASK;
    }

    /// The circular buffer storage.
    alignas(CACHE_LINE_SIZE) T buffer_[Capacity];

    /// Producer index (tail): only written by the producer thread.
    /// Padded to its own cache line to prevent false sharing with head_.
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_;

    /// Consumer index (head): only written by the consumer thread.
    /// Padded to its own cache line to prevent false sharing with tail_.
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_;
};

} // namespace fastbid

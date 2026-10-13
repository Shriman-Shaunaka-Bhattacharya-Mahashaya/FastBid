#pragma once

/// @file object_pool.hpp
/// @brief Fixed-size object pool allocator with zero-allocation hot path.
///
/// Pre-allocates a contiguous block of memory at construction. Allocation and
/// deallocation are O(1) via an intrusive free-list. No heap allocations
/// (`new`, `malloc`) occur after construction.
///
/// Thread Safety: NOT thread-safe. Designed to be owned by a single thread
/// (the engine thread). This avoids atomic operations on the hot path.
///
/// @tparam T        The object type to pool.
/// @tparam Capacity Maximum number of objects in the pool.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "core/types.hpp"

namespace fastbid {

template <typename T, std::size_t Capacity>
class ObjectPool {
public:
    static_assert(Capacity > 0, "ObjectPool capacity must be > 0");

    static_assert(Capacity < UINT32_MAX);
    static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_default_constructible_v<T>);
    static_assert(alignof(T) <= CACHE_LINE_SIZE);

    ObjectPool() noexcept {
        // Initialize the free-list: each slot points to the next free slot.
        // Slot i stores the index (i+1) as its "next free" pointer.
        // The last slot stores SENTINEL to indicate end-of-list.
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            *next_ptr(static_cast<uint32_t>(i)) = static_cast<uint32_t>(i + 1);
        }
        *next_ptr(Capacity - 1) = SENTINEL;
        free_head_ = 0;
        allocated_count_ = 0;
    }

    ~ObjectPool() = default;

    // Non-copyable, non-movable (owns a large fixed buffer)
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /// @brief Allocate an object from the pool.
    /// @return Pointer to uninitialized memory for one T, or nullptr if pool is exhausted.
    ///
    /// O(1). No syscall. No heap allocation.
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == SENTINEL) {
            return nullptr;  // Pool exhausted
        }

        const uint32_t slot = free_head_;
        free_head_ = *next_ptr(slot);
        ++allocated_count_;

        return ::new (static_cast<void*>(object_ptr(slot))) T;
    }

    /// @brief Return an object to the pool.
    /// @param ptr Pointer previously returned by allocate().
    ///
    /// O(1). No syscall. The object is NOT destructed — caller is responsible
    /// for cleanup if T has non-trivial destruction needs.
    void deallocate(T* ptr) noexcept {
        assert(ptr != nullptr);
        assert(owns(ptr) && "Pointer does not belong to this pool");

        const uint32_t slot = slot_index(ptr);
        *next_ptr(slot) = free_head_;
        free_head_ = slot;
        --allocated_count_;
    }

    /// @brief Check if a pointer belongs to this pool.
    [[nodiscard]] bool owns(const T* ptr) const noexcept {
        const auto raw = reinterpret_cast<std::uintptr_t>(ptr);
        const auto base = reinterpret_cast<std::uintptr_t>(storage_);
        return raw >= base && raw - base < sizeof(storage_) && (raw - base) % ALIGNED_SLOT_SIZE == 0;
    }

    /// @brief Number of currently allocated objects.
    [[nodiscard]] std::size_t allocated() const noexcept { return allocated_count_; }

    /// @brief Number of free slots remaining.
    [[nodiscard]] std::size_t available() const noexcept { return Capacity - allocated_count_; }

    /// @brief Total capacity of the pool.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    /// Sentinel value indicating end of free-list
    static constexpr uint32_t SENTINEL = UINT32_MAX;

    /// Each slot must be large enough to hold either a T object or a uint32_t
    /// (the free-list next pointer). We also ensure cache-line alignment.
    static constexpr std::size_t SLOT_SIZE = 
        (sizeof(T) < sizeof(uint32_t)) ? sizeof(uint32_t) : sizeof(T);

    /// Align each slot to cache line boundary for optimal access patterns.
    static constexpr std::size_t ALIGNED_SLOT_SIZE =
        (SLOT_SIZE + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);

    /// The storage block: contiguous, cache-line aligned.
    alignas(CACHE_LINE_SIZE) char storage_[ALIGNED_SLOT_SIZE * Capacity];

    /// Head of the free-list (index into storage)
    uint32_t free_head_;

    /// Number of currently allocated objects
    std::size_t allocated_count_;

    /// Get a pointer to the object at slot `index`.
    [[nodiscard]] T* object_ptr(uint32_t index) noexcept {
        return reinterpret_cast<T*>(storage_ + ALIGNED_SLOT_SIZE * index);
    }

    [[nodiscard]] const T* object_ptr(uint32_t index) const noexcept {
        return reinterpret_cast<const T*>(storage_ + ALIGNED_SLOT_SIZE * index);
    }

    /// Get a pointer to the free-list "next" field stored in slot `index`.
    /// When a slot is free, we reuse its memory to store the next-free index.
    [[nodiscard]] uint32_t* next_ptr(uint32_t index) noexcept {
        return reinterpret_cast<uint32_t*>(storage_ + ALIGNED_SLOT_SIZE * index);
    }

    /// Compute the slot index for a given object pointer.
    [[nodiscard]] uint32_t slot_index(const T* ptr) const noexcept {
        const auto offset = reinterpret_cast<const char*>(ptr) - storage_;
        return static_cast<uint32_t>(offset / ALIGNED_SLOT_SIZE);
    }
};

} // namespace fastbid

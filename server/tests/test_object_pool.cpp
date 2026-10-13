/// @file test_object_pool.cpp
/// @brief Unit tests for the ObjectPool allocator.

#include <gtest/gtest.h>
#include "memory/object_pool.hpp"
#include "core/types.hpp"

using namespace fastbid;

// Simple test struct
struct TestObj {
    uint64_t a;
    uint32_t b;
    char     data[32];
};

TEST(ObjectPoolTest, AllocateAndDeallocate) {
    ObjectPool<TestObj, 16> pool;

    EXPECT_EQ(pool.allocated(), 0u);
    EXPECT_EQ(pool.available(), 16u);

    TestObj* obj = pool.allocate();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(pool.allocated(), 1u);
    EXPECT_EQ(pool.available(), 15u);
    EXPECT_TRUE(pool.owns(obj));

    pool.deallocate(obj);
    EXPECT_EQ(pool.allocated(), 0u);
    EXPECT_EQ(pool.available(), 16u);
}

TEST(ObjectPoolTest, ExhaustPool) {
    constexpr std::size_t CAP = 8;
    ObjectPool<TestObj, CAP> pool;

    TestObj* ptrs[CAP];
    for (std::size_t i = 0; i < CAP; ++i) {
        ptrs[i] = pool.allocate();
        ASSERT_NE(ptrs[i], nullptr) << "Allocation failed at index " << i;
    }

    EXPECT_EQ(pool.allocated(), CAP);
    EXPECT_EQ(pool.available(), 0u);

    // Pool should be exhausted
    EXPECT_EQ(pool.allocate(), nullptr);

    // Free one and re-allocate
    pool.deallocate(ptrs[0]);
    EXPECT_EQ(pool.available(), 1u);

    TestObj* reused = pool.allocate();
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // Cleanup
    pool.deallocate(reused);
    for (std::size_t i = 1; i < CAP; ++i) {
        pool.deallocate(ptrs[i]);
    }
    EXPECT_EQ(pool.allocated(), 0u);
}

TEST(ObjectPoolTest, AllocatedPointersAreUnique) {
    ObjectPool<TestObj, 64> pool;

    TestObj* a = pool.allocate();
    TestObj* b = pool.allocate();
    TestObj* c = pool.allocate();

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);

    pool.deallocate(a);
    pool.deallocate(b);
    pool.deallocate(c);
}

TEST(ObjectPoolTest, CacheLineAlignment) {
    ObjectPool<TestObj, 4> pool;

    TestObj* a = pool.allocate();
    TestObj* b = pool.allocate();

    // Each allocation should be cache-line aligned
    EXPECT_EQ(reinterpret_cast<uintptr_t>(a) % CACHE_LINE_SIZE, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b) % CACHE_LINE_SIZE, 0u);

    pool.deallocate(a);
    pool.deallocate(b);
}

TEST(ObjectPoolTest, OwnershipCheck) {
    ObjectPool<TestObj, 4> pool;
    TestObj stack_obj{};

    TestObj* pooled = pool.allocate();
    EXPECT_TRUE(pool.owns(pooled));
    EXPECT_FALSE(pool.owns(&stack_obj));

    pool.deallocate(pooled);
}

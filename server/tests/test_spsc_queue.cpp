/// @file test_spsc_queue.cpp
/// @brief Unit tests for the SPSC lock-free ring buffer.

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include "memory/spsc_queue.hpp"
#include "core/types.hpp"

using namespace fastbid;

// Simple trivially-copyable test struct
struct TestMsg {
    uint64_t id;
    uint32_t value;
    char     tag;
};

TEST(SPSCQueueTest, PushAndPop) {
    SPSCQueue<TestMsg, 16> queue;

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 0u);

    TestMsg msg{42, 100, 'A'};
    EXPECT_TRUE(queue.try_push(msg));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 1u);

    TestMsg out{};
    EXPECT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.id, 42u);
    EXPECT_EQ(out.value, 100u);
    EXPECT_EQ(out.tag, 'A');
    EXPECT_TRUE(queue.empty());
}

TEST(SPSCQueueTest, FIFO_Order) {
    SPSCQueue<TestMsg, 16> queue;

    for (uint64_t i = 0; i < 10; ++i) {
        EXPECT_TRUE(queue.try_push(TestMsg{i, 0, 'X'}));
    }

    for (uint64_t i = 0; i < 10; ++i) {
        TestMsg out{};
        EXPECT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out.id, i) << "FIFO order violated at position " << i;
    }
}

TEST(SPSCQueueTest, FullQueue) {
    // Capacity 4, usable slots = 3 (one slot reserved for full/empty distinction)
    SPSCQueue<TestMsg, 4> queue;

    EXPECT_TRUE(queue.try_push(TestMsg{1, 0, 0}));
    EXPECT_TRUE(queue.try_push(TestMsg{2, 0, 0}));
    EXPECT_TRUE(queue.try_push(TestMsg{3, 0, 0}));

    // Queue should be full now (3 usable slots in capacity-4 queue)
    EXPECT_FALSE(queue.try_push(TestMsg{4, 0, 0}));

    // Pop one and push again
    TestMsg out{};
    EXPECT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.id, 1u);

    EXPECT_TRUE(queue.try_push(TestMsg{4, 0, 0}));
}

TEST(SPSCQueueTest, EmptyQueue) {
    SPSCQueue<TestMsg, 8> queue;

    TestMsg out{};
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(SPSCQueueTest, WrapAround) {
    SPSCQueue<TestMsg, 4> queue;  // capacity 4, usable 3

    // Fill and drain multiple times to exercise wrap-around
    for (int round = 0; round < 10; ++round) {
        for (uint64_t i = 0; i < 3; ++i) {
            EXPECT_TRUE(queue.try_push(TestMsg{i + static_cast<uint64_t>(round) * 100, 0, 0}));
        }
        for (uint64_t i = 0; i < 3; ++i) {
            TestMsg out{};
            EXPECT_TRUE(queue.try_pop(out));
            EXPECT_EQ(out.id, i + static_cast<uint64_t>(round) * 100);
        }
        EXPECT_TRUE(queue.empty());
    }
}

TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    constexpr std::size_t NUM_ITEMS = 100000;
    SPSCQueue<TestMsg, 1024> queue;

    std::atomic<bool> done{false};
    std::atomic<uint64_t> consumed_count{0};

    // Producer thread
    std::thread producer([&]() {
        for (uint64_t i = 0; i < NUM_ITEMS; ++i) {
            TestMsg msg{i, static_cast<uint32_t>(i * 2), 'P'};
            while (!queue.try_push(msg)) {
                // Spin-wait if queue is full
            }
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer thread
    std::thread consumer([&]() {
        uint64_t expected = 0;
        while (expected < NUM_ITEMS) {
            TestMsg out{};
            if (queue.try_pop(out)) {
                EXPECT_EQ(out.id, expected)
                    << "Out-of-order at expected=" << expected;
                EXPECT_EQ(out.value, static_cast<uint32_t>(expected * 2));
                ++expected;
            }
        }
        consumed_count.store(expected, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed_count.load(), NUM_ITEMS);
    EXPECT_TRUE(queue.empty());
}

TEST(SPSCQueueTest, Capacity) {
    SPSCQueue<TestMsg, 64> queue;
    // Usable capacity is Capacity - 1
    EXPECT_EQ(queue.capacity(), 63u);
}

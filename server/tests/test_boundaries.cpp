#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <thread>
#include <atomic>
#include "core/matching_engine.hpp"
#include "instrumentation/stats.hpp"

using namespace fastbid;
namespace {
Order make_order(uint64_t id, Side side, uint64_t price, uint32_t qty = 10) {
    Order o{};
    o.order_id = id; o.client_order_id = id; o.instrument_id = 1;
    o.side = side; o.price = price; o.quantity = qty; o.remaining_qty = qty;
    return o;
}
template<std::size_t PoolSize = 32, std::size_t QueueSize = 128>
struct Harness {
    using E = MatchingEngine<201, PoolSize, QueueSize>;
    typename E::InboundQueue in;
    typename E::OutboundQueue out;
    E engine{in, out, {{100, 300, 200, 1}}};
    OrderMessage request(Side side = Side::BUY, uint64_t price = 200, uint32_t qty = 10) {
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.side = side;
        m.order_type = OrderType::LIMIT; m.instrument_id = 1; m.connection_fd = 42;
        m.client_order_id = 900; m.price = price; m.quantity = qty; return m;
    }
    std::vector<EngineEvent> run(OrderMessage m) {
        if (!in.try_push(m)) { ADD_FAILURE() << "Unexpected full ingress"; return {}; }
        engine.process_batch();
        std::vector<EngineEvent> events; EngineEvent e{};
        while (out.try_pop(e)) events.push_back(e);
        return events;
    }
};
}

TEST(PriceLevelEdges, RemoveMiddleHeadTailAndReuse) {
    PriceLevel level{}; level.init(200);
    auto a = make_order(1, Side::BUY, 200), b = make_order(2, Side::BUY, 200), c = make_order(3, Side::BUY, 200);
    level.push_back(&a); level.push_back(&b); level.push_back(&c);
    level.remove(&b);
    EXPECT_EQ(a.next, &c); EXPECT_EQ(c.prev, &a); EXPECT_EQ(level.total_quantity, 20u);
    EXPECT_EQ(b.next, nullptr); EXPECT_EQ(b.prev, nullptr);
    EXPECT_EQ(level.pop_front(), &a); EXPECT_EQ(level.front(), &c);
    level.remove(&c);
    EXPECT_TRUE(level.empty()); EXPECT_EQ(level.tail, nullptr); EXPECT_EQ(level.total_quantity, 0u);
    EXPECT_EQ(level.order_count, 0u); EXPECT_EQ(level.pop_front(), nullptr);
    level.push_back(&b); EXPECT_EQ(level.pop_front(), &b); EXPECT_TRUE(level.empty());
}

TEST(PoolEdges, SingleSlotAndInteriorPointers) {
    ObjectPool<Order, 1> pool;
    auto* a = pool.allocate(); ASSERT_NE(a, nullptr);
    EXPECT_EQ(pool.allocate(), nullptr); EXPECT_EQ(pool.available(), 0u);
    EXPECT_FALSE(pool.owns(nullptr));
    EXPECT_FALSE(pool.owns(reinterpret_cast<Order*>(reinterpret_cast<char*>(a) + 1)));
    pool.deallocate(a); EXPECT_EQ(pool.allocate(), a); pool.deallocate(a);
    EXPECT_EQ(pool.available(), 1u);
}

TEST(QueueEdges, MinimumCapacityRepeatedWrapAndFailedOperationsPreserveValues) {
    SPSCQueue<uint64_t, 2> q;
    uint64_t value = 999;
    EXPECT_FALSE(q.try_pop(value)); EXPECT_EQ(value, 999u);
    for (uint64_t i = 0; i < 10000; ++i) {
        EXPECT_EQ(q.available(), 1u); ASSERT_TRUE(q.try_push(i));
        EXPECT_EQ(q.available(), 0u); EXPECT_FALSE(q.try_push(i + 100));
        ASSERT_TRUE(q.try_pop(value)); ASSERT_EQ(value, i); EXPECT_EQ(q.size_approx(), 0u);
    }
}

TEST(BookEdges, ForcedHashCollisionsUnlinkEveryPosition) {
    OrderBook<201, 1> book({100, 300, 200, 1});
    std::array<Order, 5> orders{};
    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i] = make_order(i + 1, Side::BUY, 100 + i * 32);
        ASSERT_TRUE(book.add_order(&orders[i]));
    }
    for (auto id : {3u, 5u, 1u, 4u, 2u}) {
        EXPECT_EQ(book.find_order(id), &orders[id - 1]);
        EXPECT_EQ(book.cancel_order(id), &orders[id - 1]);
        EXPECT_EQ(book.find_order(id), nullptr); EXPECT_EQ(book.cancel_order(id), nullptr);
    }
    EXPECT_EQ(book.total_orders(), 0u); EXPECT_EQ(book.best_bid(), 0u);
}

TEST(BookEdges, RejectsInvalidAndCrossingInsertionsWithoutMutation) {
    OrderBook<201, 8> book({100, 300, 200, 1});
    EXPECT_FALSE(book.add_order(nullptr));
    auto good = make_order(1, Side::BUY, 200); ASSERT_TRUE(book.add_order(&good));
    for (int i = 0; i < 7; ++i) {
        auto bad = make_order(2, Side::SELL, 201);
        if (i == 0) bad.order_id = 0;
        if (i == 1) bad.remaining_qty = 0;
        if (i == 2) bad.instrument_id = 2;
        if (i == 3) bad.side = static_cast<Side>(0);
        if (i == 4) bad.price = 99;
        if (i == 5) bad.price = 200;
        if (i == 6) bad.order_id = 1;
        EXPECT_FALSE(book.add_order(&bad)); EXPECT_EQ(book.total_orders(), 1u);
    }
    book.cancel_order(1);
}

TEST(BookEdges, SnapshotTopTenSortedAggregatedAndZeroPadded) {
    OrderBook<201, 32> book({100, 300, 200, 1});
    std::array<Order, 25> orders{};
    for (std::size_t i = 0; i < 12; ++i) {
        orders[i] = make_order(i + 1, Side::BUY, 100 + i * 8);
        orders[i + 12] = make_order(i + 13, Side::SELL, 212 + i * 8);
        ASSERT_TRUE(book.add_order(&orders[i])); ASSERT_TRUE(book.add_order(&orders[i + 12]));
    }
    orders[24] = make_order(25, Side::BUY, 188, 25); ASSERT_TRUE(book.add_order(&orders[24]));
    MarketSnapshot snap{}; book.snapshot(snap);
    ASSERT_EQ(snap.bid_count, 10u); ASSERT_EQ(snap.ask_count, 10u);
    EXPECT_EQ(snap.bids[0].quantity, 35u);
    for (std::size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(snap.bids[i].price, 188 - i * 8); EXPECT_EQ(snap.asks[i].price, 212 + i * 8);
    }
    for (auto& o : orders) book.cancel_order(o.order_id);
    book.snapshot(snap); EXPECT_EQ(snap.bid_count, 0u); EXPECT_EQ(snap.ask_count, 0u);
    for (auto& x : snap.bids) { EXPECT_EQ(x.price, 0u); EXPECT_EQ(x.quantity, 0u); }
    for (auto& x : snap.asks) { EXPECT_EQ(x.price, 0u); EXPECT_EQ(x.quantity, 0u); }
}

TEST(BookEdges, SinglePriceConfigurationAndBitmapWordBoundaries) {
    OrderBook<1, 1> one({200, 200, 200, 1});
    auto order = make_order(1, Side::SELL, 200); ASSERT_TRUE(one.add_order(&order));
    EXPECT_EQ(one.best_ask(), 200u); one.cancel_order(1); EXPECT_EQ(one.best_ask(), UINT64_MAX);
    OrderBook<201, 8> book({100, 300, 200, 1});
    std::array<Order, 4> orders{};
    const uint64_t prices[] = {100, 163, 164, 292};
    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i] = make_order(i + 1, Side::BUY, prices[i]); ASSERT_TRUE(book.add_order(&orders[i]));
    }
    for (std::size_t i = orders.size(); i-- > 0;) {
        EXPECT_EQ(book.best_bid(), prices[i]); book.cancel_order(i + 1);
    }
    EXPECT_EQ(book.best_bid(), 0u);
}

TEST(EngineEdges, EmptyAndZeroBudgetLeaveInputUntouched) {
    Harness<> h; EXPECT_EQ(h.engine.process_batch(), 0u);
    ASSERT_TRUE(h.in.try_push(h.request())); EXPECT_EQ(h.engine.process_batch(0), 0u);
    EXPECT_EQ(h.in.size_approx(), 1u); EXPECT_EQ(h.engine.pool().allocated(), 0u);
    EXPECT_EQ(h.engine.process_batch(1), 1u);
}

TEST(EngineEdges, PoolExhaustionRejectsAndCancelRecoversCapacity) {
    Harness<2> h;
    ASSERT_EQ(h.run(h.request()).size(), 1u); ASSERT_EQ(h.run(h.request()).size(), 1u);
    auto events = h.run(h.request()); ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EngineEventType::REJECT); EXPECT_EQ(events[0].price, 2u);
    EXPECT_EQ(events[0].client_order_id, 900u); EXPECT_EQ(h.engine.next_order_id(), 3u);
    auto cancel = h.request(); cancel.type = MessageType::CANCEL; cancel.order_id = 1;
    events = h.run(cancel); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].type, EngineEventType::CANCEL_ACK);
    events = h.run(h.request()); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].order_id, 3u);
    EXPECT_EQ(h.engine.pool().allocated(), 2u);
}

TEST(EngineEdges, CancelPartialFillReportsOriginalOwnerAndClientId) {
    Harness<> h; auto m = h.request(); m.client_order_id = UINT64_MAX - 10;
    h.run(m); auto fill = h.run(h.request(Side::SELL, 199, 4));
    ASSERT_EQ(fill.size(), 3u); EXPECT_EQ(fill[1].price, 200u); EXPECT_EQ(fill[2].remaining_quantity, 6u);
    auto cancel = h.request(); cancel.type = MessageType::CANCEL; cancel.order_id = 1;
    auto events = h.run(cancel); ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].client_order_id, m.client_order_id); EXPECT_EQ(events[0].connection_fd, 42);
    EXPECT_EQ(h.engine.pool().allocated(), 0u);
    events = h.run(cancel); EXPECT_EQ(events[0].price, 3u);
}

TEST(EngineEdges, InvalidModifyCannotChangeOriginal) {
    Harness<> h; h.run(h.request());
    for (int i = 0; i < 6; ++i) {
        auto m = h.request(); m.type = MessageType::MODIFY; m.order_id = 1;
        if (i == 0) m.quantity = 0;
        if (i == 1) m.side = static_cast<Side>(0);
        if (i == 2) m.order_type = static_cast<OrderType>(0);
        if (i == 3) m.instrument_id = 2;
        if (i == 4) m.connection_fd = 43;
        if (i == 5) m.order_id = 99;
        auto events = h.run(m); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].type, EngineEventType::REJECT);
        ASSERT_NE(h.engine.book().find_order(1), nullptr); EXPECT_EQ(h.engine.pool().allocated(), 1u);
        EXPECT_EQ(h.engine.next_order_id(), 2u);
    }
}

TEST(EngineEdges, ReplacementAtFullPoolCanCrossOppositeSide) {
    Harness<2> h; h.run(h.request(Side::BUY, 180)); h.run(h.request(Side::SELL, 220));
    auto m = h.request(Side::BUY, 230); m.type = MessageType::MODIFY; m.order_id = 1;
    auto e = h.run(m); ASSERT_EQ(e.size(), 4u);
    EXPECT_EQ(e[0].type, EngineEventType::CANCEL_ACK); EXPECT_EQ(e[1].order_id, 3u);
    EXPECT_EQ(e[2].price, 220u); EXPECT_EQ(e[2].trade_id, e[3].trade_id);
    EXPECT_EQ(h.engine.pool().allocated(), 0u);
}

TEST(EngineEdges, FullOutputDoesNotConsumeInputOrAssignIds) {
    Harness<16, 4> h;
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.out.try_push(EngineEvent{}));
    ASSERT_TRUE(h.in.try_push(h.request())); EXPECT_EQ(h.engine.process_batch(), 0u);
    EXPECT_EQ(h.in.size_approx(), 1u); EXPECT_EQ(h.engine.next_order_id(), 1u);
    EngineEvent e{}; while (h.out.try_pop(e)) {}
    EXPECT_EQ(h.engine.process_batch(), 1u); EXPECT_EQ(h.engine.pool().allocated(), 1u);
}

class SweepBothSides : public ::testing::TestWithParam<Side> {};
TEST_P(SweepBothSides, RestingPriceFifoAndConservation) {
    Harness<> h; const auto side = GetParam(); const bool buy = side == Side::BUY;
    const auto opposite = buy ? Side::SELL : Side::BUY;
    const uint64_t first = buy ? 180 : 220, second = 200;
    h.run(h.request(opposite, first, 3)); h.run(h.request(opposite, first, 4)); h.run(h.request(opposite, second, 5));
    auto e = h.run(h.request(side, buy ? 300 : 100, 15)); ASSERT_EQ(e.size(), 7u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(e[2 + i * 2].order_id, i + 1); EXPECT_EQ(e[1 + i * 2].aggressor_side, side);
        EXPECT_EQ(e[1 + i * 2].price, i < 2 ? first : second);
        EXPECT_EQ(e[1 + i * 2].trade_id, e[2 + i * 2].trade_id);
    }
    EXPECT_EQ(h.engine.book().get_level(buy ? 300 : 100).total_quantity, 3u);
    EXPECT_EQ(h.engine.pool().allocated(), 1u);
}
INSTANTIATE_TEST_SUITE_P(BuyAndSell, SweepBothSides, ::testing::Values(Side::BUY, Side::SELL));

TEST(StatsEdges, EmptySingletonPercentilesAndDrain) {
    StatsCollector<16> stats; EXPECT_EQ(stats.compute().sample_count, 0u);
    stats.record(UINT64_MAX); auto s = stats.compute();
    EXPECT_EQ(s.min_ns, UINT64_MAX); EXPECT_EQ(s.max_ns, UINT64_MAX); EXPECT_EQ(s.mean_ns, UINT64_MAX);
    for (uint64_t i = 1; i <= 10; ++i) stats.record(i);
    s = stats.compute(); EXPECT_EQ(s.sample_count, 10u); EXPECT_EQ(s.min_ns, 1u); EXPECT_EQ(s.max_ns, 10u);
    EXPECT_EQ(s.p50_ns, 5u); EXPECT_EQ(s.p99_ns, 9u); EXPECT_EQ(s.mean_ns, 5u);
    EXPECT_EQ(stats.compute().sample_count, 0u);
}

TEST(StatsEdges, SaturationCountsDropsAndRecovers) {
    StatsCollector<4> stats;
    for (int i = 0; i < 10; ++i) stats.record(42);
    EXPECT_EQ(stats.dropped(), 7u); EXPECT_EQ(stats.compute().sample_count, 3u);
    stats.record(17); EXPECT_EQ(stats.compute().min_ns, 17u); EXPECT_EQ(stats.dropped(), 7u);
}

TEST(BookEdges, PriceBandNearUint64LimitDoesNotWrap) {
    OrderBook<201, 8> book({UINT64_MAX - 201, UINT64_MAX - 1, UINT64_MAX - 100, 1});
    auto a = make_order(1, Side::SELL, UINT64_MAX - 201), b = make_order(2, Side::SELL, UINT64_MAX - 1);
    ASSERT_TRUE(book.add_order(&a)); ASSERT_TRUE(book.add_order(&b));
    EXPECT_EQ(book.best_ask(), a.price); book.cancel_order(1); EXPECT_EQ(book.best_ask(), b.price);
    book.cancel_order(2); EXPECT_EQ(book.best_ask(), UINT64_MAX);
}

class InvalidBookConfig : public ::testing::TestWithParam<OrderBookConfig> {};
TEST_P(InvalidBookConfig, RejectedBeforeUsingStorage) {
    const auto config = GetParam();
    EXPECT_DEATH((OrderBook<201, 1>{config}), "Invalid order book configuration");
}
INSTANTIATE_TEST_SUITE_P(InvalidBands, InvalidBookConfig, ::testing::Values(
    OrderBookConfig{100, 300, 99, 1}, OrderBookConfig{100, 300, 301, 1},
    OrderBookConfig{100, 300, 200, 0}, OrderBookConfig{300, 100, 200, 1},
    OrderBookConfig{0, 200, 100, 1}, OrderBookConfig{100, UINT64_MAX, 200, 1}));

TEST(EngineConcurrency, ConcurrentQueuesPreserveEveryAcknowledgmentAndFill) {
    using E = MatchingEngine<201, 64, 64>;
    E::InboundQueue in; E::OutboundQueue out; E engine(in, out, {{100, 300, 200, 1}});
    constexpr uint64_t total = 20000;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        uint64_t processed = 0;
        while (processed < total || engine.has_pending()) processed += engine.process_batch(16);
        done.store(true, std::memory_order_release);
    });
    uint64_t sent = 0, acks = 0, fills = 0, last_ack = 0;
    while (!done.load(std::memory_order_acquire) || !out.empty()) {
        if (sent < total) {
            OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.side = sent % 2 ? Side::SELL : Side::BUY;
            m.order_type = OrderType::LIMIT; m.instrument_id = 1; m.price = 200; m.quantity = 1;
            m.client_order_id = sent + 1;
            if (in.try_push(m)) ++sent;
        }
        EngineEvent e{};
        while (out.try_pop(e)) {
            if (e.type == EngineEventType::ORDER_ACK) {
                EXPECT_EQ(e.order_id, ++last_ack); EXPECT_EQ(e.client_order_id, e.order_id); ++acks;
            } else if (e.type == EngineEventType::TRADE) {
                EXPECT_EQ(e.fill_quantity, 1u); EXPECT_EQ(e.remaining_quantity, 0u); ++fills;
            } else ADD_FAILURE() << "Unexpected rejection/event";
        }
    }
    worker.join();
    EXPECT_EQ(sent, total); EXPECT_EQ(acks, total); EXPECT_EQ(fills, total);
    EXPECT_EQ(engine.pool().allocated(), 0u); EXPECT_EQ(engine.book().total_orders(), 0u);
}

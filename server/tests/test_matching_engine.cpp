/// @file test_matching_engine.cpp
/// @brief Unit tests for the OrderBook and MatchingEngine.

#include <gtest/gtest.h>
#include "core/matching_engine.hpp"
#include "core/order_book.hpp"
#include "core/types.hpp"

using namespace fastbid;

// ─── OrderBook Tests ───

class OrderBookTest : public ::testing::Test {
protected:
    static constexpr std::size_t MAX_LEVELS = 3000;

    OrderBookConfig config{
        .min_price      = 9000,   // $0.90 in ticks
        .max_price      = 11000,  // $1.10 in ticks
        .reference_price = 10000, // $1.00 in ticks
        .instrument_id  = 1
    };

    OrderBook<MAX_LEVELS> book{config};

    // Helper: create a test Order on the stack (normally from pool)
    Order make_order(uint64_t id, Side side, uint64_t price, uint32_t qty) {
        Order o{};
        o.order_id      = id;
        o.client_order_id = id;
        o.instrument_id = 1;
        o.side          = side;
        o.price         = price;
        o.quantity      = qty;
        o.remaining_qty = qty;
        o.timestamp     = 0;
        o.connection_fd = -1;
        o.next          = nullptr;
        o.prev          = nullptr;
        return o;
    }
};

TEST_F(OrderBookTest, PriceBandValidation) {
    EXPECT_TRUE(book.is_price_valid(9000));
    EXPECT_TRUE(book.is_price_valid(10000));
    EXPECT_TRUE(book.is_price_valid(11000));
    EXPECT_FALSE(book.is_price_valid(8999));
    EXPECT_FALSE(book.is_price_valid(11001));
    EXPECT_FALSE(book.is_price_valid(0));
}

TEST_F(OrderBookTest, AddAndCancelOrder) {
    Order bid = make_order(1, Side::BUY, 10000, 100);

    EXPECT_TRUE(book.add_order(&bid));
    EXPECT_EQ(book.total_bid_orders(), 1u);
    EXPECT_EQ(book.best_bid(), 10000u);

    Order* removed = book.cancel_order(1);
    EXPECT_EQ(removed, &bid);
    EXPECT_EQ(book.total_bid_orders(), 0u);
    EXPECT_EQ(book.best_bid(), 0u);
}

TEST_F(OrderBookTest, BestBidAskTracking) {
    Order bid1 = make_order(1, Side::BUY, 9900, 50);
    Order bid2 = make_order(2, Side::BUY, 10000, 50);
    Order ask1 = make_order(3, Side::SELL, 10100, 50);
    Order ask2 = make_order(4, Side::SELL, 10200, 50);

    book.add_order(&bid1);
    book.add_order(&bid2);
    book.add_order(&ask1);
    book.add_order(&ask2);

    EXPECT_EQ(book.best_bid(), 10000u);  // highest bid
    EXPECT_EQ(book.best_ask(), 10100u);  // lowest ask

    // Remove the best bid
    book.cancel_order(2);
    EXPECT_EQ(book.best_bid(), 9900u);   // falls back to next

    // Remove the best ask
    book.cancel_order(3);
    EXPECT_EQ(book.best_ask(), 10200u);

    // Cleanup
    book.cancel_order(1);
    book.cancel_order(4);
}

TEST_F(OrderBookTest, PriceBandReject) {
    Order invalid = make_order(99, Side::BUY, 5000, 100);  // way out of band
    EXPECT_FALSE(book.add_order(&invalid));
    EXPECT_EQ(book.total_orders(), 0u);
}

TEST_F(OrderBookTest, PriceLevelFIFO) {
    Order o1 = make_order(1, Side::BUY, 10000, 10);
    Order o2 = make_order(2, Side::BUY, 10000, 20);
    Order o3 = make_order(3, Side::BUY, 10000, 30);

    book.add_order(&o1);
    book.add_order(&o2);
    book.add_order(&o3);

    PriceLevel& level = book.get_level(10000);
    EXPECT_EQ(level.order_count, 3u);
    EXPECT_EQ(level.total_quantity, 60u);

    // Front should be o1 (first in)
    EXPECT_EQ(level.front()->order_id, 1u);

    // Cleanup
    book.cancel_order(1);
    book.cancel_order(2);
    book.cancel_order(3);
}

// ─── MatchingEngine Tests ───

class MatchingEngineTest : public ::testing::Test {
protected:
    // Use small parameters for testing
    static constexpr std::size_t MAX_LEVELS    = 3000;
    static constexpr std::size_t POOL_CAPACITY = 1024;
    static constexpr std::size_t QUEUE_SIZE    = 256;  // Must be power of 2

    using Engine = MatchingEngine<MAX_LEVELS, POOL_CAPACITY, QUEUE_SIZE>;

    Engine::InboundQueue  inbound;
    Engine::OutboundQueue outbound;

    MatchingEngineConfig engine_config{
        .book_config = {
            .min_price       = 9000,
            .max_price       = 11000,
            .reference_price = 10000,
            .instrument_id   = 1
        }
    };

    Engine engine{inbound, outbound, engine_config};

    // Helper: push a new order message into the inbound queue
    void submit_order(uint64_t client_id, Side side, uint64_t price, uint32_t qty, int fd = 1) {
        OrderMessage msg{};
        msg.type             = MessageType::NEW_ORDER;
        msg.side             = side;
        msg.order_type       = OrderType::LIMIT;
        msg.instrument_id    = 1;
        msg.client_order_id  = client_id;
        msg.price            = price;
        msg.quantity         = qty;
        msg.connection_fd    = fd;
        msg.timestamp        = 0;
        ASSERT_TRUE(inbound.try_push(msg));
    }

    void submit_cancel(uint64_t order_id, int fd = 1) {
        OrderMessage msg{};
        msg.type            = MessageType::CANCEL;
        msg.instrument_id   = 1;
        msg.order_id        = order_id;
        msg.connection_fd   = fd;
        ASSERT_TRUE(inbound.try_push(msg));
    }

    // Helper: drain all events from the outbound queue
    std::vector<EngineEvent> drain_events() {
        std::vector<EngineEvent> events;
        EngineEvent evt{};
        while (outbound.try_pop(evt)) {
            events.push_back(evt);
        }
        return events;
    }
};

TEST_F(MatchingEngineTest, NewOrderAddsToBook) {
    submit_order(100, Side::BUY, 10000, 50);
    engine.process_batch();

    auto events = drain_events();
    // Should have 1 ACK event
    ASSERT_GE(events.size(), 1u);
    EXPECT_EQ(events[0].type, EngineEventType::ORDER_ACK);

    EXPECT_EQ(engine.book().best_bid(), 10000u);
    EXPECT_EQ(engine.book().total_bid_orders(), 1u);
}

TEST_F(MatchingEngineTest, PriceBandReject) {
    submit_order(100, Side::BUY, 5000, 50);  // out of band
    engine.process_batch();

    auto events = drain_events();
    ASSERT_GE(events.size(), 1u);
    EXPECT_EQ(events[0].type, EngineEventType::REJECT);
    EXPECT_EQ(engine.book().total_orders(), 0u);
}

TEST_F(MatchingEngineTest, SimpleMatch) {
    // Place a resting sell order
    submit_order(1, Side::SELL, 10000, 100);
    engine.process_batch();
    drain_events();  // consume the ACK

    EXPECT_EQ(engine.book().best_ask(), 10000u);

    // Place an aggressive buy order that crosses the spread
    submit_order(2, Side::BUY, 10000, 50);
    engine.process_batch();

    auto events = drain_events();

    // Expect: ACK for buy, FILL for buy, FILL for sell
    bool got_buy_fill = false;
    bool got_sell_fill = false;
    for (const auto& evt : events) {
        if (evt.type == EngineEventType::TRADE) {
            EXPECT_EQ(evt.aggressor_side, Side::BUY);
            if (evt.order_id == 2) {
                got_buy_fill = true;
                EXPECT_EQ(evt.fill_quantity, 50u);
            } else {
                got_sell_fill = true;
                EXPECT_EQ(evt.fill_quantity, 50u);
                EXPECT_EQ(evt.remaining_quantity, 50u);  // 100 - 50
            }
        }
    }
    EXPECT_TRUE(got_buy_fill);
    EXPECT_TRUE(got_sell_fill);

    // Sell order should still be on the book with 50 remaining
    EXPECT_EQ(engine.book().total_ask_orders(), 1u);
}

TEST_F(MatchingEngineTest, FullFill) {
    // Resting sell: 100 @ 10000
    submit_order(1, Side::SELL, 10000, 100);
    engine.process_batch();
    drain_events();

    // Aggressive buy: 100 @ 10000 — should fully fill both
    submit_order(2, Side::BUY, 10000, 100);
    engine.process_batch();

    auto events = drain_events();

    // Both orders should be fully filled, book should be empty
    EXPECT_EQ(engine.book().total_orders(), 0u);
}

TEST_F(MatchingEngineTest, CancelOrder) {
    submit_order(1, Side::BUY, 10000, 100);
    engine.process_batch();
    drain_events();

    EXPECT_EQ(engine.book().total_bid_orders(), 1u);

    // Cancel the order (order_id = 1, the first assigned ID)
    submit_cancel(1);
    engine.process_batch();

    auto events = drain_events();
    bool got_cancel_ack = false;
    for (const auto& evt : events) {
        if (evt.type == EngineEventType::CANCEL_ACK) {
            got_cancel_ack = true;
        }
    }
    EXPECT_TRUE(got_cancel_ack);
    EXPECT_EQ(engine.book().total_orders(), 0u);
}

TEST_F(MatchingEngineTest, PriceTimePriority) {
    // Place two sell orders at the same price — first one should be matched first
    submit_order(1, Side::SELL, 10000, 50);  // first (higher priority)
    submit_order(2, Side::SELL, 10000, 50);  // second
    engine.process_batch();
    drain_events();

    // Aggressive buy for 50 — should match against order 1 (first seller)
    submit_order(3, Side::BUY, 10000, 50);
    engine.process_batch();

    // Order 1 should be fully filled and removed
    // Order 2 should still be on the book
    EXPECT_EQ(engine.book().total_ask_orders(), 1u);
}

TEST_F(MatchingEngineTest, AggressiveSellFullFillAndPoolReuse) {
    submit_order(1, Side::BUY, 10000, 100); engine.process_batch(); drain_events();
    submit_order(2, Side::SELL, 10000, 100); engine.process_batch();
    auto events = drain_events();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[1].aggressor_side, Side::SELL);
    EXPECT_EQ(events[2].aggressor_side, Side::SELL);
    EXPECT_EQ(engine.book().total_orders(), 0u);
    EXPECT_EQ(engine.pool().allocated(), 0u);
    submit_order(3, Side::SELL, 10000, 10); engine.process_batch(); drain_events();
    submit_order(4, Side::BUY, 10000, 10); engine.process_batch();
    EXPECT_EQ(engine.pool().allocated(), 0u);
}

TEST_F(MatchingEngineTest, AggressiveSellSweepsPriceTimeAndLeavesRemainder) {
    submit_order(1, Side::BUY, 10100, 20);
    submit_order(2, Side::BUY, 10100, 30);
    submit_order(3, Side::BUY, 10000, 40);
    engine.process_batch(); drain_events();
    submit_order(4, Side::SELL, 9900, 100); engine.process_batch();
    const auto events = drain_events(); ASSERT_EQ(events.size(), 7u);
    EXPECT_EQ(events[2].order_id, 1u); EXPECT_EQ(events[4].order_id, 2u); EXPECT_EQ(events[6].order_id, 3u);
    EXPECT_EQ(events[1].price, 10100u); EXPECT_EQ(events[5].price, 10000u);
    EXPECT_EQ(engine.book().get_level(9900).total_quantity, 10u);
    EXPECT_EQ(engine.pool().allocated(), 1u);
}

TEST_F(MatchingEngineTest, RejectsInvalidFieldsAndForeignCancellation) {
    for (int field = 0; field < 5; ++field) {
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.side = Side::BUY;
        m.order_type = OrderType::LIMIT; m.instrument_id = 1; m.price = 10000; m.quantity = 10;
        if (field == 0) m.quantity = 0;
        if (field == 1) m.side = static_cast<Side>(99);
        if (field == 2) m.order_type = static_cast<OrderType>(99);
        if (field == 3) m.instrument_id = 2;
        if (field == 4) m.type = static_cast<MessageType>(99);
        ASSERT_TRUE(inbound.try_push(m)); engine.process_batch();
        const auto events = drain_events(); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].type, EngineEventType::REJECT);
    }
    submit_order(1, Side::BUY, 10000, 10, 1); engine.process_batch(); drain_events();
    submit_cancel(1, 2); engine.process_batch();
    EXPECT_EQ(drain_events()[0].price, 7u);
    EXPECT_EQ(engine.book().total_orders(), 1u);
}

TEST_F(MatchingEngineTest, InvalidModifyPreservesOriginalAndValidModifyLosesPriority) {
    submit_order(1, Side::BUY, 10000, 10); submit_order(2, Side::BUY, 10000, 20);
    engine.process_batch(); drain_events();
    OrderMessage m{}; m.type = MessageType::MODIFY; m.order_id = 1; m.connection_fd = 1;
    m.instrument_id = 1; m.side = Side::BUY; m.order_type = OrderType::LIMIT; m.price = 8000; m.quantity = 10;
    ASSERT_TRUE(inbound.try_push(m)); engine.process_batch(); drain_events();
    EXPECT_EQ(engine.book().get_level(10000).head->order_id, 1u);
    m.price = 10000; ASSERT_TRUE(inbound.try_push(m)); engine.process_batch();
    const auto events = drain_events(); ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, EngineEventType::CANCEL_ACK); EXPECT_EQ(events[1].type, EngineEventType::ORDER_ACK);
    EXPECT_EQ(engine.book().get_level(10000).head->order_id, 2u);
    EXPECT_EQ(engine.book().get_level(10000).tail->order_id, 3u);
}

TEST_F(OrderBookTest, AggregateQuantityExceeds32Bits) {
    Order a = make_order(1, Side::BUY, 10000, UINT32_MAX), b = make_order(2, Side::BUY, 10000, UINT32_MAX);
    ASSERT_TRUE(book.add_order(&a)); ASSERT_TRUE(book.add_order(&b));
    EXPECT_EQ(book.get_level(10000).total_quantity, uint64_t{UINT32_MAX} * 2);
    EXPECT_FALSE(book.add_order(&a));
    book.cancel_order(1); book.cancel_order(2);
}

TEST(MatchingSaturation, ResumesWithoutLossOrPriorityInversion) {
    using E = MatchingEngine<101, 16, 4>;
    E::InboundQueue in; E::OutboundQueue out; E engine(in, out, {{100, 200, 150, 1}});
    auto submit = [&](Side side, uint32_t qty, uint64_t client) {
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.side = side; m.order_type = OrderType::LIMIT;
        m.instrument_id = 1; m.price = 150; m.quantity = qty; m.client_order_id = client;
        EXPECT_TRUE(in.try_push(m));
    };
    EngineEvent e{};
    for (int i = 0; i < 3; ++i) { submit(Side::SELL, 1, i + 1); engine.process_batch(); ASSERT_TRUE(out.try_pop(e)); }
    submit(Side::BUY, 3, 4); submit(Side::SELL, 10, 5);
    EXPECT_EQ(engine.process_batch(), 1u); EXPECT_TRUE(engine.has_pending());
    EXPECT_EQ(in.size_approx(), 1u);
    std::vector<EngineEvent> all;
    for (int i = 0; i < 20; ++i) {
        while (out.try_pop(e)) all.push_back(e);
        engine.process_batch();
    }
    ASSERT_EQ(all.size(), 8u); // aggressor ACK, six fills, later order ACK
    EXPECT_EQ(all.back().client_order_id, 5u);
    EXPECT_EQ(engine.book().total_ask_orders(), 1u); EXPECT_EQ(engine.pool().allocated(), 1u);
}

TEST(OrderBookConfigTest, RejectsInvalidConfigurationInReleaseToo) {
    EXPECT_DEATH((OrderBook<10>{{1, 11, 5, 1}}), "Invalid");
    EXPECT_DEATH((OrderBook<10>{{10, 1, 5, 1}}), "Invalid");
    EXPECT_DEATH((OrderBook<10>{{0, 5, 1, 1}}), "Invalid");
}

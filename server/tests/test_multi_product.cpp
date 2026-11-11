#include <gtest/gtest.h>
#include <array>
#include "core/matching_engine.hpp"
#include "core/product_catalog.hpp"
using namespace fastbid;

class MultiProduct : public ::testing::Test {
protected:
    using E = MatchingEngine<201, 32, 64, 3>;
    std::array<OrderBookConfig, 2> extra{{{100, 300, 200, 2}, {100, 300, 200, 3}}};
    E::InboundQueue in; E::OutboundQueue out;
    E engine{in, out, {{100, 300, 200, 1}, extra}};
    OrderMessage message(uint32_t product, Side side = Side::BUY, uint32_t qty = 10) {
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.instrument_id = product;
        m.side = side; m.order_type = OrderType::LIMIT; m.price = 200; m.quantity = qty;
        m.connection_fd = 7; m.client_order_id = 100 + product; return m;
    }
    std::vector<EngineEvent> run(OrderMessage m) {
        EXPECT_TRUE(in.try_push(m)); engine.process_batch();
        std::vector<EngineEvent> result; EngineEvent e{};
        while (out.try_pop(e)) result.push_back(e); return result;
    }
};

TEST_F(MultiProduct, SamePriceAcrossProductsNeverMatchesAndIdsAreGlobal) {
    auto a = run(message(1)); auto b = run(message(2, Side::SELL));
    ASSERT_EQ(a.size(), 1u); ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0].order_id, 1u); EXPECT_EQ(b[0].order_id, 2u);
    EXPECT_EQ(engine.book(1)->total_bid_orders(), 1u); EXPECT_EQ(engine.book(2)->total_ask_orders(), 1u);
    auto fills = run(message(2, Side::BUY, 4)); ASSERT_EQ(fills.size(), 3u);
    for (const auto& e : fills) EXPECT_EQ(e.instrument_id, 2u);
    EXPECT_EQ(engine.book(1)->get_level(200).total_quantity, 10u);
    EXPECT_EQ(engine.book(2)->get_level(200).total_quantity, 6u);
    auto more = run(message(1, Side::SELL, 10)); ASSERT_EQ(more.size(), 3u);
    EXPECT_GT(more[1].trade_id, fills[1].trade_id);
    EXPECT_EQ(engine.pool().allocated(), 1u);
}

TEST_F(MultiProduct, WrongProductCancelAndModifyLeaveOriginalUntouched) {
    run(message(1));
    for (auto type : {MessageType::CANCEL, MessageType::MODIFY}) {
        auto m = message(2); m.type = type; m.order_id = 1;
        const auto e = run(m); ASSERT_EQ(e.size(), 1u);
        EXPECT_EQ(e[0].type, EngineEventType::REJECT); EXPECT_EQ(e[0].price, 3u);
        ASSERT_NE(engine.book(1)->find_order(1), nullptr);
        EXPECT_EQ(engine.book(2)->total_orders(), 0u);
    }
    auto cancel = message(1); cancel.type = MessageType::CANCEL; cancel.order_id = 1;
    auto e = run(cancel); ASSERT_EQ(e.size(), 1u); EXPECT_EQ(e[0].instrument_id, 1u);
    EXPECT_EQ(e[0].type, EngineEventType::CANCEL_ACK); EXPECT_EQ(engine.pool().allocated(), 0u);
}

TEST_F(MultiProduct, UnknownProductRejectedBeforeAssigningIds) {
    for (auto id : {0u, 4u, UINT32_MAX}) {
        const auto e = run(message(id)); ASSERT_EQ(e.size(), 1u);
        EXPECT_EQ(e[0].price, 4u); EXPECT_EQ(e[0].instrument_id, id);
        EXPECT_EQ(engine.next_order_id(), 1u);
    }
    EXPECT_EQ(engine.book(99), nullptr);
}

TEST_F(MultiProduct, EmptyAndOccupiedSnapshotsCarryCorrectProduct) {
    run(message(3, Side::SELL, 42));
    for (uint32_t i = 1; i <= 3; ++i) {
        MarketSnapshot snap{}; engine.book(i)->snapshot(snap);
        EXPECT_EQ(snap.header.instrument_id, i); EXPECT_EQ(snap.header.version, 2u);
        EXPECT_EQ(snap.ask_count, i == 3 ? 1u : 0u); EXPECT_EQ(snap.bid_count, 0u);
    }
}

TEST(MultiProductConfig, DuplicatesAndExcessProductsFailAtStartup) {
    using E = MatchingEngine<201, 4, 8, 2>;
    E::InboundQueue in; E::OutboundQueue out;
    std::array<OrderBookConfig, 2> extra{{{100, 300, 200, 1}, {100, 300, 200, 2}}};
    EXPECT_DEATH((E{in, out, {{100, 300, 200, 1}, std::span(extra).first(1)}}), "");
    EXPECT_DEATH((E{in, out, {{100, 300, 200, 1}, extra}}), "");
}

TEST(ProductCatalog, CompiledConfigurationContainsDistinctUsableBooks) {
    EXPECT_GE(PRODUCT_COUNT, 3u);
    for (std::size_t i = 0; i < PRODUCT_COUNT; ++i) {
        EXPECT_LE(PRODUCT_BOOKS[i].num_levels(), PRODUCT_LEVELS);
        for (std::size_t j = 0; j < i; ++j) EXPECT_NE(PRODUCT_BOOKS[i].instrument_id, PRODUCT_BOOKS[j].instrument_id);
    }
}

TEST(MultiProductLimits, SharedPoolExhaustionAndRecoveryAcrossProducts) {
    using E = MatchingEngine<201, 2, 16, 2>;
    E::InboundQueue in; E::OutboundQueue out;
    const OrderBookConfig second{100, 300, 200, 2};
    E engine(in, out, {{100, 300, 200, 1}, std::span(&second, 1)});
    auto run = [&](uint32_t product, MessageType type = MessageType::NEW_ORDER) {
        OrderMessage m{}; m.type = type; m.instrument_id = product; m.order_id = 1;
        m.side = Side::BUY; m.order_type = OrderType::LIMIT; m.price = 200; m.quantity = 1;
        EXPECT_TRUE(in.try_push(m)); engine.process_batch();
        EngineEvent e{}; EXPECT_TRUE(out.try_pop(e)); return e;
    };
    run(1); run(1);
    auto e = run(2); EXPECT_EQ(e.type, EngineEventType::REJECT); EXPECT_EQ(e.price, 2u);
    EXPECT_EQ(engine.book(2)->total_orders(), 0u);
    run(1, MessageType::CANCEL);
    e = run(2); EXPECT_EQ(e.type, EngineEventType::ORDER_ACK); EXPECT_EQ(e.instrument_id, 2u);
    EXPECT_EQ(engine.pool().allocated(), 2u);
}

TEST(MultiProductLimits, PendingSweepRetainsItsBookBeforeOtherProductsAreAdmitted) {
    using E = MatchingEngine<201, 8, 4, 2>;
    E::InboundQueue in; E::OutboundQueue out;
    const OrderBookConfig second{100, 300, 200, 2};
    E engine(in, out, {{100, 300, 200, 1}, std::span(&second, 1)});
    auto submit = [&](uint32_t product, Side side, uint32_t qty) {
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.instrument_id = product;
        m.side = side; m.order_type = OrderType::LIMIT; m.price = 200; m.quantity = qty;
        EXPECT_TRUE(in.try_push(m));
    };
    EngineEvent e{};
    for (int i = 0; i < 2; ++i) { submit(1, Side::SELL, 1); engine.process_batch(); ASSERT_TRUE(out.try_pop(e)); }
    submit(1, Side::BUY, 2); submit(2, Side::SELL, 1);
    EXPECT_EQ(engine.process_batch(), 1u); EXPECT_TRUE(engine.has_pending()); EXPECT_EQ(in.size_approx(), 1u);
    for (int i = 0; i < 10; ++i) {
        while (out.try_pop(e)) {}
        engine.process_batch();
    }
    EXPECT_EQ(engine.book(1)->total_orders(), 0u);
    EXPECT_EQ(engine.book(2)->total_ask_orders(), 1u); EXPECT_EQ(engine.pool().allocated(), 1u);
}

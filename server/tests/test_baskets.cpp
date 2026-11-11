#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "core/basket.hpp"
#include "core/matching_engine.hpp"

using namespace fastbid;
TEST(BasketRegistryEdges, ConstantPriceProductsCanFormASingleLevelBasket) {
    using E = MatchingEngine<1, 4, 8, 3>;
    E::InboundQueue in; E::OutboundQueue out;
    const OrderBookConfig other{200, 200, 200, 2};
    E engine(in, out, {{100, 100, 100, 1}, std::span(&other, 1)});
    BasketRequest r{}; r.leg_count = 2; r.name[0] = 'X';
    r.legs[0] = {1, 2}; r.legs[1] = {2, 3};
    const auto result = engine.register_basket(r);
    ASSERT_EQ(result.reason, 0u);
    EXPECT_EQ(result.definition.min_price, 800u);
    EXPECT_EQ(result.definition.max_price, 800u);
    EXPECT_EQ(engine.book(result.definition.instrument_id)->config().num_levels(), 1u);
}
namespace {
BasketRequest request(std::initializer_list<BasketLeg> legs = {{1, 2}, {2, 3}}, const char* name = "Fruit Basket") {
    BasketRequest r{};
    r.request_id = 123;
    r.leg_count = static_cast<uint16_t>(legs.size());
    // An empty initializer_list may have a null begin(); memcpy requires non-null
    // arguments even when its byte count is zero.
    if (legs.size() != 0)
        std::memcpy(r.legs, legs.begin(), legs.size() * sizeof(BasketLeg));
    const auto length = std::strlen(name);
    std::memcpy(r.name, name, length < sizeof(r.name) ? length : sizeof(r.name));
    return r;
}

template<std::size_t Pool = 32, std::size_t Queue = 64>
struct Harness {
    using E = MatchingEngine<201, Pool, Queue, 11>;
    typename E::InboundQueue in;
    typename E::OutboundQueue out;
    std::array<OrderBookConfig, 2> extra{{{100, 300, 200, 2}, {100, 300, 200, 3}}};
    E engine{in, out, {{100, 300, 200, 1}, extra}};
    OrderMessage order(uint32_t id, Side side, uint64_t price, uint32_t qty = 10, int owner = 7) {
        OrderMessage m{};
        m.type = MessageType::NEW_ORDER; m.instrument_id = id; m.side = side;
        m.order_type = OrderType::LIMIT; m.price = price; m.quantity = qty;
        m.connection_fd = owner; m.client_order_id = 999;
        return m;
    }
    std::vector<EngineEvent> run(const OrderMessage& m) {
        EXPECT_TRUE(in.try_push(m)); engine.process_batch();
        EngineEvent e{}; std::vector<EngineEvent> events;
        while (out.try_pop(e)) events.push_back(e);
        return events;
    }
};
}

TEST(BasketRegistry, CreatesCanonicalRecipeAndDerivedPriceGrid) {
    Harness<> h;
    const auto registered = h.engine.register_basket(request({{2, 3}, {1, 2}}));
    ASSERT_EQ(registered.reason, 0u);
    const auto& b = registered.definition;
    EXPECT_GE(b.instrument_id, BASKET_ID_BASE);
    EXPECT_EQ(b.leg_count, 2u); EXPECT_EQ(b.legs[0].instrument_id, 1u);
    EXPECT_EQ(b.legs[0].quantity, 2u); EXPECT_EQ(b.legs[1].instrument_id, 2u);
    EXPECT_EQ(b.legs[1].quantity, 3u);
    EXPECT_EQ(b.min_price, 500u); EXPECT_EQ(b.max_price, 1500u);
    EXPECT_EQ(b.reference_price, 1000u); EXPECT_EQ(b.tick_size, 5u);
    ASSERT_NE(h.engine.book(b.instrument_id), nullptr);
    EXPECT_EQ(h.engine.book(b.instrument_id)->config().num_levels(), 201u);
    EXPECT_EQ(h.engine.instrument_count(), 4u); EXPECT_EQ(h.engine.baskets().size(), 1u);
    EXPECT_EQ(h.engine.next_order_id(), 1u); EXPECT_EQ(h.engine.pool().allocated(), 0u);
}

TEST(BasketRegistry, ReorderedRecipeReusesIdentityAndOriginalName) {
    Harness<> h;
    const auto first = h.engine.register_basket(request({{1, 2}, {2, 3}}, "Original"));
    ASSERT_EQ(first.reason, 0u);
    const auto second = h.engine.register_basket(request({{2, 3}, {1, 2}}, "Different label"));
    ASSERT_EQ(second.reason, 0u);
    EXPECT_EQ(first.definition.instrument_id, second.definition.instrument_id);
    EXPECT_EQ(std::memcmp(first.definition.name, second.definition.name, sizeof(first.definition.name)), 0);
    EXPECT_EQ(h.engine.baskets().size(), 1u); EXPECT_EQ(h.engine.instrument_count(), 4u);
}

TEST(BasketRegistry, QuantityRatiosAndNamesDoNotConflateDifferentRecipes) {
    Harness<> h;
    const auto a = h.engine.register_basket(request({{1, 1}, {2, 2}}, "Same name"));
    const auto b = h.engine.register_basket(request({{1, 2}, {2, 4}}, "Same name"));
    ASSERT_EQ(a.reason, 0u); ASSERT_EQ(b.reason, 0u);
    EXPECT_NE(a.definition.instrument_id, b.definition.instrument_id);
    EXPECT_EQ(h.engine.baskets().size(), 2u);
}

TEST(BasketRegistry, InvalidRecipesNeverConsumeRegistrySlots) {
    Harness<> h;
    std::vector<BasketRequest> bad{
        request({}), request({{1, 1}}), request({{1, 1}, {1, 2}}),
        request({{0, 1}, {1, 1}}), request({{1, 1}, {99, 1}}),
        request({{1, 0}, {2, 1}}), request({{1, MAX_LEG_QUANTITY + 1}, {2, 1}}),
        request({{1, UINT32_MAX}, {2, UINT32_MAX}}), request({{1, 1}, {2, 1}}, "")};
    auto tooMany = request(); tooMany.leg_count = MAX_BASKET_LEGS + 1; bad.push_back(tooMany);
    auto reserved = request(); reserved.reserved = 1; bad.push_back(reserved);
    auto controlName = request(); controlName.name[1] = '\n'; bad.push_back(controlName);
    auto hiddenName = request(); hiddenName.name[20] = 'X'; bad.push_back(hiddenName);
    auto unusedLeg = request(); unusedLeg.legs[2] = {3, 1}; bad.push_back(unusedLeg);
    bad.push_back(request({{1, 1}, {2, 1}}, " Leading"));
    bad.push_back(request({{1, 1}, {2, 1}}, "Trailing "));
    for (const auto& r : bad) {
        EXPECT_EQ(h.engine.register_basket(r).reason, 9u);
        EXPECT_EQ(h.engine.baskets().size(), 0u);
        EXPECT_EQ(h.engine.instrument_count(), 3u);
        EXPECT_EQ(h.engine.next_order_id(), 1u);
    }
    EXPECT_EQ(h.engine.register_basket(request()).reason, 0u);
}

TEST(BasketRegistry, FullWidthPrintableNameIsAcceptedWithoutTerminator) {
    Harness<> h;
    auto r = request(); std::memset(r.name, 'A', sizeof(r.name));
    const auto registration = h.engine.register_basket(r); ASSERT_EQ(registration.reason, 0u);
    EXPECT_EQ(std::memcmp(registration.definition.name, r.name, sizeof(r.name)), 0);
}

TEST(BasketRegistry, NestedBasketIsRejected) {
    Harness<> h;
    const auto first = h.engine.register_basket(request()); ASSERT_EQ(first.reason, 0u);
    const auto nested = h.engine.register_basket(request({{first.definition.instrument_id, 1}, {3, 1}}));
    EXPECT_EQ(nested.reason, 9u); EXPECT_EQ(h.engine.baskets().size(), 1u);
}

TEST(BasketRegistry, CapacityRejectsNewRecipesButAllowsExistingIdentity) {
    Harness<> h;
    for (uint32_t i = 1; i <= MAX_BASKETS; ++i)
        ASSERT_EQ(h.engine.register_basket(request({{1, i}, {2, 1}})).reason, 0u);
    EXPECT_EQ(h.engine.baskets().size(), MAX_BASKETS);
    EXPECT_EQ(h.engine.register_basket(request({{1, MAX_BASKETS + 1}, {2, 1}})).reason, 10u);
    EXPECT_EQ(h.engine.register_basket(request({{2, 1}, {1, 1}})).reason, 0u);
    EXPECT_EQ(h.engine.baskets().size(), MAX_BASKETS);
}

TEST(BasketMatching, WholeBasketFillsPreserveRecipeAndLeaveRemainder) {
    Harness<> h;
    const auto registration = h.engine.register_basket(request()); ASSERT_EQ(registration.reason, 0u);
    const auto id = registration.definition.instrument_id;
    auto ack = h.run(h.order(id, Side::SELL, 1000, 10)); ASSERT_EQ(ack.size(), 1u);
    auto fills = h.run(h.order(id, Side::BUY, 1000, 4)); ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[1].fill_quantity, 4u); EXPECT_EQ(fills[2].fill_quantity, 4u);
    EXPECT_EQ(fills[2].remaining_quantity, 6u); EXPECT_EQ(fills[1].trade_id, fills[2].trade_id);
    EXPECT_EQ(fills[1].instrument_id, id); EXPECT_EQ(fills[2].instrument_id, id);
    EXPECT_EQ(h.engine.book(id)->get_level(1000).total_quantity, 6u);
    EXPECT_EQ(h.engine.baskets()[0].legs[0].quantity, 2u);
    EXPECT_EQ(h.engine.baskets()[0].legs[1].quantity, 3u);
    EXPECT_EQ(h.engine.book(1)->total_orders(), 0u); EXPECT_EQ(h.engine.book(2)->total_orders(), 0u);
}

TEST(BasketMatching, ConstituentsAndDifferentBasketRecipesCannotCross) {
    Harness<> h;
    const auto a = h.engine.register_basket(request({{1, 1}, {2, 1}}));
    const auto b = h.engine.register_basket(request({{1, 1}, {3, 1}}));
    ASSERT_EQ(a.reason, 0u); ASSERT_EQ(b.reason, 0u);
    // Base orders are at their price-band limit, basket orders at their lower limit.
    ASSERT_EQ(h.run(h.order(1, Side::BUY, 300)).size(), 1u);
    ASSERT_EQ(h.run(h.order(a.definition.instrument_id, Side::SELL, 200)).size(), 1u);
    ASSERT_EQ(h.run(h.order(b.definition.instrument_id, Side::BUY, 300)).size(), 1u);
    EXPECT_EQ(h.engine.pool().allocated(), 3u);
    EXPECT_EQ(h.engine.book(a.definition.instrument_id)->total_ask_orders(), 1u);
    EXPECT_EQ(h.engine.book(b.definition.instrument_id)->total_bid_orders(), 1u);
}

TEST(BasketMatching, WrongInstrumentAndForeignOwnerCannotCancelOrModify) {
    Harness<> h;
    const auto r = h.engine.register_basket(request()); ASSERT_EQ(r.reason, 0u);
    const auto id = r.definition.instrument_id;
    auto events = h.run(h.order(id, Side::BUY, 1000)); ASSERT_EQ(events.size(), 1u);
    const auto orderId = events[0].order_id;
    for (auto type : {MessageType::CANCEL, MessageType::MODIFY}) {
        auto wrong = h.order(1, Side::BUY, 200); wrong.type = type; wrong.order_id = orderId;
        events = h.run(wrong); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].price, 3u);
        auto foreign = h.order(id, Side::BUY, 1000, 10, 8); foreign.type = type; foreign.order_id = orderId;
        events = h.run(foreign); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].price, 7u);
        ASSERT_NE(h.engine.book(id)->find_order(orderId), nullptr);
    }
    auto cancel = h.order(id, Side::BUY, 1000); cancel.type = MessageType::CANCEL; cancel.order_id = orderId;
    events = h.run(cancel); ASSERT_EQ(events.size(), 1u); EXPECT_EQ(events[0].type, EngineEventType::CANCEL_ACK);
    EXPECT_EQ(h.engine.pool().allocated(), 0u);
}

TEST(BasketMatching, OffGridPricesRejectWithoutConsumingOrderIds) {
    Harness<> h;
    const auto r = h.engine.register_basket(request()); ASSERT_EQ(r.reason, 0u);
    const auto id = r.definition.instrument_id;
    EXPECT_EQ(h.engine.book(id)->config().tick_size, 5u);
    for (const auto price : {499u, 501u, 1001u, 1501u}) {
        auto events = h.run(h.order(id, Side::BUY, price)); ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, EngineEventType::REJECT); EXPECT_EQ(events[0].price, 1u);
        EXPECT_EQ(h.engine.next_order_id(), 1u);
    }
    ASSERT_EQ(h.run(h.order(id, Side::BUY, 500)).size(), 1u);
    ASSERT_EQ(h.run(h.order(id, Side::SELL, 1500)).size(), 1u);
    MarketSnapshot snapshot{}; h.engine.book(id)->snapshot(snapshot);
    EXPECT_EQ(snapshot.bids[0].price, 500u); EXPECT_EQ(snapshot.asks[0].price, 1500u);
    EXPECT_EQ(snapshot.header.instrument_id, id);
}

TEST(BasketMatching, PoolIsSharedWithBaseProductsAndIdsRemainGlobal) {
    Harness<2> h;
    const auto r = h.engine.register_basket(request()); ASSERT_EQ(r.reason, 0u);
    const auto id = r.definition.instrument_id;
    auto a = h.run(h.order(1, Side::BUY, 200)); auto b = h.run(h.order(id, Side::BUY, 1000));
    ASSERT_EQ(a.size(), 1u); ASSERT_EQ(b.size(), 1u); EXPECT_EQ(a[0].order_id, 1u); EXPECT_EQ(b[0].order_id, 2u);
    auto e = h.run(h.order(2, Side::BUY, 200)); ASSERT_EQ(e.size(), 1u); EXPECT_EQ(e[0].price, 2u);
    auto cancel = h.order(id, Side::BUY, 1000); cancel.type = MessageType::CANCEL; cancel.order_id = 2;
    h.run(cancel); e = h.run(h.order(2, Side::BUY, 200));
    ASSERT_EQ(e.size(), 1u); EXPECT_EQ(e[0].order_id, 3u);
}

TEST(BasketRegistry, PendingSweepRejectsRegistrationUntilSafeBoundary) {
    Harness<32, 4> h;
    for (int i = 0; i < 2; ++i) ASSERT_EQ(h.run(h.order(1, Side::SELL, 200, 1)).size(), 1u);
    ASSERT_TRUE(h.in.try_push(h.order(1, Side::BUY, 200, 2)));
    EXPECT_EQ(h.engine.process_batch(), 1u); ASSERT_TRUE(h.engine.has_pending());
    EXPECT_EQ(h.engine.register_basket(request()).reason, 11u); EXPECT_EQ(h.engine.baskets().size(), 0u);
    EngineEvent e{};
    for (int i = 0; i < 10 && h.engine.has_pending(); ++i) {
        while (h.out.try_pop(e)) {}
        h.engine.process_batch();
    }
    ASSERT_FALSE(h.engine.has_pending()); EXPECT_EQ(h.engine.register_basket(request()).reason, 0u);
}

TEST(BasketPriceGrid, NonUnitStepBestPriceMovementAndSnapshots) {
    OrderBook<201, 8> book({500, 1500, 1000, BASKET_ID_BASE, 5});
    auto order = [](uint64_t id, Side side, uint64_t price) {
        Order o{}; o.order_id = id; o.instrument_id = BASKET_ID_BASE; o.side = side;
        o.price = price; o.quantity = 1; o.remaining_qty = 1; return o;
    };
    std::array<Order, 4> orders{{order(1, Side::BUY, 500), order(2, Side::BUY, 820),
        order(3, Side::SELL, 1180), order(4, Side::SELL, 1500)}};
    for (auto& o : orders) ASSERT_TRUE(book.add_order(&o));
    EXPECT_FALSE(book.is_price_valid(821)); EXPECT_TRUE(book.is_price_valid(820));
    EXPECT_EQ(book.best_bid(), 820u); EXPECT_EQ(book.best_ask(), 1180u);
    book.cancel_order(2); book.cancel_order(3);
    EXPECT_EQ(book.best_bid(), 500u); EXPECT_EQ(book.best_ask(), 1500u);
    MarketSnapshot s{}; book.snapshot(s); EXPECT_EQ(s.bids[0].price, 500u); EXPECT_EQ(s.asks[0].price, 1500u);
}

TEST(BasketPriceGrid, InvalidGridConfigurationsFailInReleaseToo) {
    EXPECT_DEATH((OrderBook<201, 1>{{500, 1500, 1000, BASKET_ID_BASE, 0}}), "Invalid");
    EXPECT_DEATH((OrderBook<201, 1>{{500, 1501, 1000, BASKET_ID_BASE, 5}}), "Invalid");
    EXPECT_DEATH((OrderBook<201, 1>{{500, 1500, 1001, BASKET_ID_BASE, 5}}), "Invalid");
    EXPECT_DEATH((OrderBook<201, 1>{{500, 1500, 1000, BASKET_ID_BASE, 1}}), "Invalid");
}

TEST(BasketRegistryLimits, SixteenLegsAndTotalQuantityBound) {
    using E = MatchingEngine<201, 16, 64, 24>;
    E::InboundQueue in; E::OutboundQueue out;
    std::array<OrderBookConfig, 15> extra{};
    for (std::size_t i = 0; i < extra.size(); ++i) extra[i] = {100, 300, 200, static_cast<uint32_t>(i + 2)};
    auto engine = std::make_unique<E>(in, out, MatchingEngineConfig{{100, 300, 200, 1}, extra});
    auto r = request(); r.leg_count = MAX_BASKET_LEGS;
    for (uint32_t i = 0; i < MAX_BASKET_LEGS; ++i) r.legs[i] = {i + 1, 1000};
    EXPECT_EQ(engine->register_basket(r).reason, 9u); EXPECT_TRUE(engine->baskets().empty());
    for (uint32_t i = 0; i < MAX_BASKET_LEGS; ++i) r.legs[i].quantity = 500;
    const auto registered = engine->register_basket(r); ASSERT_EQ(registered.reason, 0u);
    EXPECT_EQ(registered.definition.leg_count, MAX_BASKET_LEGS);
    EXPECT_LE(engine->book(registered.definition.instrument_id)->config().num_levels(), 201u);
}

TEST(BasketRegistryLimits, WeightedPriceOverflowRejectsWithoutCreatingBook) {
    using E = MatchingEngine<201, 4, 16, 10>;
    E::InboundQueue in; E::OutboundQueue out;
    const OrderBookConfig second{100, 300, 200, 2};
    E engine(in, out, {{UINT64_MAX - 200, UINT64_MAX - 100, UINT64_MAX - 150, 1}, std::span(&second, 1)});
    const auto r = engine.register_basket(request({{1, 2}, {2, 1}}));
    EXPECT_EQ(r.reason, 9u); EXPECT_TRUE(engine.baskets().empty()); EXPECT_EQ(engine.instrument_count(), 2u);
}

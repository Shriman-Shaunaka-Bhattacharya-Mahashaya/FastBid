#include <gtest/gtest.h>
#include <random>
#include <vector>
#include <algorithm>
#include <thread>
#include "core/matching_engine.hpp"
#include "instrumentation/stats.hpp"
using namespace fastbid;
TEST(ReferenceModel, RandomizedPriceTimeMatchesAndCancels) {
    using E = MatchingEngine<101, 512, 2048>;
    auto in = std::make_unique<E::InboundQueue>(); auto out = std::make_unique<E::OutboundQueue>();
    auto engine = std::make_unique<E>(*in, *out, MatchingEngineConfig{{100,200,150,1}});
    struct Resting { uint64_t id, price; uint32_t qty; Side side; };
    std::vector<Resting> book;
    std::mt19937 random(12345);
    uint64_t next = 1;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        OrderMessage m{}; m.instrument_id = 1; m.connection_fd = 1;
        std::vector<std::pair<uint64_t, uint32_t>> expected;
        if (!book.empty() && random()%4 == 0) {
            auto index = random()%book.size(); m.type = MessageType::CANCEL; m.order_id = book[index].id;
            book.erase(book.begin()+static_cast<std::ptrdiff_t>(index));
        } else {
            m.type = MessageType::NEW_ORDER; m.order_type = OrderType::LIMIT;
            m.side = random()%2 ? Side::BUY : Side::SELL; m.quantity = static_cast<uint32_t>(1+random()%100); m.price = 100+random()%101;
            uint32_t qty = m.quantity;
            while (qty) {
                auto best = book.end();
                for (auto it = book.begin(); it != book.end(); ++it) {
                    if (it->side == m.side || (m.side == Side::BUY ? it->price > m.price : it->price < m.price)) continue;
                    if (best == book.end() || (m.side == Side::BUY ? it->price < best->price : it->price > best->price)) best = it;
                }
                if (best == book.end()) break;
                const auto fill = std::min(qty, best->qty); expected.emplace_back(best->id, fill);
                best->qty -= fill; qty -= fill;
                if (!best->qty) book.erase(best);
            }
            if (qty) book.push_back({next,m.price,qty,m.side});
            ++next;
        }
        ASSERT_TRUE(in->try_push(m)); engine->process_batch();
        EngineEvent e{}; ASSERT_TRUE(out->try_pop(e));
        ASSERT_EQ(e.type, m.type == MessageType::CANCEL ? EngineEventType::CANCEL_ACK : EngineEventType::ORDER_ACK);
        for (const auto& [id, qty] : expected) {
            ASSERT_TRUE(out->try_pop(e)); EXPECT_EQ(e.type, EngineEventType::TRADE); EXPECT_EQ(e.fill_quantity,qty);
            ASSERT_TRUE(out->try_pop(e)); EXPECT_EQ(e.order_id,id); EXPECT_EQ(e.fill_quantity,qty);
        }
        ASSERT_FALSE(out->try_pop(e));
        ASSERT_EQ(engine->book().total_orders(),book.size()); ASSERT_EQ(engine->pool().allocated(),book.size());
        uint64_t bid=0,ask=UINT64_MAX;
        for (const auto& rest : book) {
            auto actual = engine->book().find_order(rest.id); ASSERT_NE(actual,nullptr); ASSERT_EQ(actual->remaining_qty,rest.qty);
            if (rest.side == Side::BUY) bid=std::max(bid,rest.price); else ask=std::min(ask,rest.price);
        }
        ASSERT_EQ(engine->book().best_bid(),bid); ASSERT_EQ(engine->book().best_ask(),ask);
    }
}
TEST(Stats, ConcurrentTransferAccountsForEverySample) {
    StatsCollector<256> stats;
    std::atomic<bool> done{false};
    std::thread producer([&] { for (int i=0;i<100000;++i) stats.record(100); done.store(true,std::memory_order_release); });
    uint64_t received=0;
    do { auto s=stats.compute(); received+=s.sample_count; if(s.sample_count) { EXPECT_EQ(s.p50_ns,100u); EXPECT_EQ(s.p99_ns,100u); } } while(!done.load(std::memory_order_acquire));
    producer.join(); received+=stats.compute().sample_count;
    EXPECT_EQ(received+stats.dropped(),100000u);
}

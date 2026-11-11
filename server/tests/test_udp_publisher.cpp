#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include "network/udp_publisher.hpp"
using namespace fastbid;

TEST(UDPPublisherTest, ExplicitPacketsSequenceAndSingleTrade) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); ASSERT_GE(fd, 0);
    struct Owner { int fd; ~Owner() { close(fd); } } owner{fd};
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr); ASSERT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    UDPPublisher pub({"127.0.0.1", ntohs(addr.sin_port)}); pub.init();
    EngineEvent event{}; event.type = EngineEventType::ORDER_ACK; pub.publish_event(event);
    event.instrument_id = 2; event.type = EngineEventType::TRADE; event.trade_id = 77; event.price = 100001;
    event.fill_quantity = 9; event.aggressor_side = Side::SELL; pub.publish_event(event); pub.publish_event(event);
    pollfd ready{fd, POLLIN, 0}; ASSERT_EQ(poll(&ready, 1, 1000), 1);
    char bytes[1024]{}; ASSERT_EQ(recv(fd, bytes, sizeof(bytes), 0), 48);
    MarketTrade trade{}; std::memcpy(&trade, bytes, sizeof(trade));
    EXPECT_EQ(trade.header.magic, 0x31444246u); EXPECT_EQ(trade.header.version, 2u);
    EXPECT_EQ(trade.header.kind, 1u); EXPECT_EQ(trade.header.sequence, 1u); EXPECT_EQ(trade.trade_id, 77u);
    EXPECT_EQ(trade.header.instrument_id, 2u); EXPECT_EQ(trade.price, 100001u); EXPECT_EQ(trade.quantity, 9u); EXPECT_EQ(trade.side, Side::SELL);
    EXPECT_EQ(trade.reserved[0], 0u); EXPECT_EQ(poll(&ready, 1, 20), 0);
    MarketSnapshot snap{}; snap.bid_count = 1; snap.bids[0] = {100000, uint64_t{UINT32_MAX} + 1};
    snap.p50_ns = 123; snap.p99_ns = 456; snap.samples = 7; pub.publish_snapshot(snap);
    ASSERT_EQ(poll(&ready, 1, 1000), 1); ASSERT_EQ(recv(fd, bytes, sizeof(bytes), 0), 392);
    MarketSnapshot actual{}; std::memcpy(&actual, bytes, sizeof(actual));
    EXPECT_EQ(actual.header.kind, 2u); EXPECT_EQ(actual.header.sequence, 2u);
    EXPECT_EQ(actual.bids[0].quantity, uint64_t{UINT32_MAX} + 1); EXPECT_EQ(actual.p99_ns, 456u);
    EXPECT_EQ(actual.asks[9].price, 0u); EXPECT_EQ(pub.dropped(), 0u);
}

TEST(UDPPublisherTest, FailedSendIsCounted) {
    UDPPublisher pub({"127.0.0.1", 5000});
    pub.publish_snapshot(MarketSnapshot{}); EXPECT_EQ(pub.dropped(), 1u);
}

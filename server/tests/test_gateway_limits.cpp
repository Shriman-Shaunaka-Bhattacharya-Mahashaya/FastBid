#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include "network/tcp_gateway.hpp"

using namespace fastbid;
namespace {
uint16_t available_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) std::abort();
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len)) std::abort();
    close(fd); return ntohs(addr.sin_port);
}
class GatewayLimits : public ::testing::Test {
protected:
    std::unique_ptr<TCPGateway::InboundQueue> in = std::make_unique<TCPGateway::InboundQueue>();
    std::unique_ptr<TCPGateway::OutboundQueue> out = std::make_unique<TCPGateway::OutboundQueue>();
    const uint16_t port = available_port();
    TCPGateway gateway{*in, *out, {"127.0.0.1", port, 2}};
    std::vector<int> clients;
    void SetUp() override { gateway.init(); }
    void TearDown() override { for (int fd : clients) close(fd); }
    int connect_client() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        clients.push_back(fd); int small = 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) return -1;
        for (int i = 0; i < 5; ++i) gateway.poll();
        return fd;
    }
};
}

TEST_F(GatewayLimits, AdmissionStopsAtConfiguredConnectionBound) {
    ASSERT_GE(connect_client(), 0); ASSERT_GE(connect_client(), 0);
    const int excess = connect_client(); ASSERT_GE(excess, 0);
    char byte{};
    EXPECT_EQ(recv(excess, &byte, 1, MSG_DONTWAIT), 0);
}

TEST_F(GatewayLimits, BufferedRequestSurvivesFullInboundQueueAndAdmissionPause) {
    const int fd = connect_client(); ASSERT_GE(fd, 0);
    OrderMessage placeholder{};
    for (std::size_t i = 0; i < in->capacity(); ++i) ASSERT_TRUE(in->try_push(placeholder));
    char packet[38]{}; ProtocolHeader header{38, MessageType::NEW_ORDER, 0};
    NewOrderPayload payload{99, 1, Side::BUY, OrderType::LIMIT, 100000, 7, UINT64_MAX};
    std::memcpy(packet, &header, sizeof(header)); std::memcpy(packet + 4, &payload, sizeof(payload));
    ASSERT_EQ(send(fd, packet, sizeof(packet), MSG_NOSIGNAL), sizeof(packet));
    for (int i = 0; i < 5; ++i) gateway.poll();
    EXPECT_EQ(in->size_approx(), in->capacity());
    OrderMessage message{}; while (in->try_pop(message)) {}
    gateway.poll(false); EXPECT_TRUE(in->empty());
    for (int i = 0; i < 100 && in->empty(); ++i) { gateway.poll(); usleep(1000); }
    ASSERT_TRUE(in->try_pop(message)); EXPECT_EQ(message.client_order_id, 99u);
    EXPECT_EQ(message.price, 100000u); EXPECT_EQ(message.quantity, 7u); EXPECT_EQ(message.connection_fd, 1);
    EXPECT_NE(message.timestamp, UINT64_MAX); EXPECT_FALSE(in->try_pop(message));
}

TEST_F(GatewayLimits, SlowReportConsumerIsDisconnectedAtOutputBound) {
    const int fd = connect_client(); ASSERT_GE(fd, 0);
    EngineEvent e{}; e.type = EngineEventType::ORDER_ACK; e.connection_fd = 1;
    bool observed_buffer = false, closed = false;
    for (int i = 0; i < 250000; ++i) {
        e.order_id = static_cast<uint64_t>(i + 1); gateway.send_event(e);
        if (gateway.output_pending()) observed_buffer = true;
        else if (observed_buffer) { closed = true; break; }
    }
    EXPECT_TRUE(observed_buffer); EXPECT_TRUE(closed);
    char buffer[8192]; bool eof = false;
    for (int i = 0; i < 10000; ++i) {
        const auto n = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno == ECONNRESET)) { eof = true; break; }
        if (n < 0) usleep(1000);
    }
    EXPECT_TRUE(eof);
}

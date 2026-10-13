#include "network/udp_publisher.hpp"
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
namespace fastbid {
UDPPublisher::~UDPPublisher() { if (sock_fd_ >= 0) close(sock_fd_); }
void UDPPublisher::init() {
    sock_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    destination_.sin_family = AF_INET; destination_.sin_port = htons(config_.mcast_port);
    if (sock_fd_ < 0 || inet_pton(AF_INET, config_.mcast_group.c_str(), &destination_.sin_addr) != 1) {
        std::fputs("Invalid UDP publisher configuration\n", stderr); std::exit(1);
    }
    unsigned char ttl = 1;
    if (setsockopt(sock_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl))) { std::perror("multicast TTL"); std::exit(1); }
}
void UDPPublisher::send_packet(const void* p, std::size_t size) noexcept {
    if (sendto(sock_fd_, p, size, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_)) != static_cast<ssize_t>(size)) ++dropped_;
}
void UDPPublisher::publish_event(const EngineEvent& e) noexcept {
    if (e.type != EngineEventType::TRADE || e.trade_id == last_trade_) return;
    last_trade_ = e.trade_id;
    MarketTrade t{}; t.header.kind = 1; t.header.sequence = ++sequence_;
    t.trade_id = e.trade_id; t.price = e.price; t.quantity = e.fill_quantity; t.side = e.aggressor_side;
    send_packet(&t, sizeof(t));
}
void UDPPublisher::publish_snapshot(MarketSnapshot snapshot) noexcept {
    snapshot.header.sequence = ++sequence_; send_packet(&snapshot, sizeof(snapshot));
}
}

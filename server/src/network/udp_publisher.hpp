#pragma once
#include <string>
#include <netinet/in.h>
#include "network/market_data.hpp"
namespace fastbid {
struct UDPPublisherConfig { std::string mcast_group = "239.255.0.1"; uint16_t mcast_port = 5000; };
class UDPPublisher {
public:
    explicit UDPPublisher(const UDPPublisherConfig& cfg) : config_(cfg) {}
    ~UDPPublisher();
    UDPPublisher(const UDPPublisher&) = delete;
    UDPPublisher& operator=(const UDPPublisher&) = delete;
    void init();
    void publish_event(const EngineEvent&) noexcept;
    void publish_snapshot(MarketSnapshot) noexcept;
    uint64_t dropped() const noexcept { return dropped_; }
private:
    void send_packet(const void*, std::size_t) noexcept;
    UDPPublisherConfig config_; int sock_fd_ = -1; sockaddr_in destination_{};
    uint64_t sequence_ = 0, last_trade_ = 0, dropped_ = 0;
};
}

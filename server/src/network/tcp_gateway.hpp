#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "core/types.hpp"
#include "memory/spsc_queue.hpp"
namespace fastbid {
struct TCPGatewayConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    int max_connections = 1024;
};
// Linux, single IO thread. Allocation is allowed on connection establishment,
// never in the matching thread. Per-client buffers and total clients are bounded.
class TCPGateway {
public:
    using InboundQueue = SPSCQueue<OrderMessage, 65536>;
    using OutboundQueue = SPSCQueue<EngineEvent, 65536>;
    TCPGateway(InboundQueue&, OutboundQueue&, const TCPGatewayConfig&);
    ~TCPGateway();
    TCPGateway(const TCPGateway&) = delete;
    TCPGateway& operator=(const TCPGateway&) = delete;
    void init();
    int poll(bool accept_orders = true) noexcept;
    void send_event(const EngineEvent&) noexcept;
    bool output_pending() const noexcept;
private:
    struct ClientContext {
        int session = 0;
        std::array<char, 8192> rx{};
        std::size_t rx_size = 0;
        std::array<char, 65536> tx{};
        std::size_t tx_begin = 0, tx_end = 0;
    };
    void accept_connection() noexcept;
    bool process_buffer(int, ClientContext&) noexcept;
    bool flush(int, ClientContext&) noexcept;
    void disconnect(int) noexcept;
    InboundQueue& inbound_;
    TCPGatewayConfig config_;
    int listen_fd_ = -1, epoll_fd_ = -1;
    uint64_t next_session_ = 1;
    std::unordered_map<int, ClientContext> clients_;
    std::unordered_map<int, int> sessions_;
};
}

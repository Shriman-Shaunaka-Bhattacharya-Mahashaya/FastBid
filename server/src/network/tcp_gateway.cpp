#include "network/tcp_gateway.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
namespace fastbid {
namespace { void fatal(const char* what) { std::perror(what); std::exit(EXIT_FAILURE); } }
TCPGateway::TCPGateway(InboundQueue& in, OutboundQueue&, const TCPGatewayConfig& cfg) : inbound_(in), config_(cfg) {}
TCPGateway::~TCPGateway() {
    for (auto& [fd, ctx] : clients_) close(fd);
    if (listen_fd_ >= 0) close(listen_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}
void TCPGateway::init() {
    if (config_.max_connections <= 0) { std::fputs("Invalid connection limit\n", stderr); std::exit(1); }
    clients_.reserve(static_cast<std::size_t>(config_.max_connections));
    sessions_.reserve(static_cast<std::size_t>(config_.max_connections));
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) fatal("socket");
    int one = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) fatal("reuseaddr");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) { std::fputs("Invalid bind IPv4\n", stderr); std::exit(1); }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) fatal("bind");
    if (listen(listen_fd_, config_.max_connections)) fatal("listen");
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC); if (epoll_fd_ < 0) fatal("epoll_create1");
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev)) fatal("epoll_ctl");
    std::printf("[TCP] Listening on %s:%u\n", config_.host.c_str(), config_.port);
}
void TCPGateway::accept_connection() noexcept {
    for (int count = 0; count < 64; ++count) {
        int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        if (clients_.size() >= static_cast<std::size_t>(config_.max_connections) || next_session_ > INT_MAX) { close(fd); continue; }
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) { close(fd); continue; }
        epoll_event ev{}; ev.events = EPOLLIN | EPOLLRDHUP; ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev)) { close(fd); continue; }
        auto& ctx = clients_[fd]; ctx.session = static_cast<int>(next_session_++);
        sessions_[ctx.session] = fd;
    }
}
void TCPGateway::disconnect(int fd) noexcept {
    auto it = clients_.find(fd); if (it == clients_.end()) return;
    sessions_.erase(it->second.session);
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr); close(fd); clients_.erase(it);
}
bool TCPGateway::process_buffer(int, ClientContext& ctx) noexcept {
    std::size_t consumed = 0;
    for (int count = 0; count < 64 && ctx.rx_size - consumed >= sizeof(ProtocolHeader); ++count) {
        ProtocolHeader hdr{}; std::memcpy(&hdr, ctx.rx.data() + consumed, sizeof(hdr));
        const auto expected = hdr.message_type == MessageType::NEW_ORDER ? 38u : hdr.message_type == MessageType::CANCEL ? 16u : 0u;
        if (!expected || hdr.message_length != expected || hdr.reserved) return false;
        if (ctx.rx_size - consumed < expected || !inbound_.available()) break;
        OrderMessage msg{}; msg.connection_fd = ctx.session; msg.type = hdr.message_type;
        msg.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (hdr.message_type == MessageType::NEW_ORDER) {
            NewOrderPayload p{}; std::memcpy(&p, ctx.rx.data() + consumed + 4, sizeof(p));
            msg.client_order_id = p.client_order_id; msg.instrument_id = p.instrument_id;
            msg.side = p.side; msg.order_type = p.order_type; msg.price = p.price; msg.quantity = p.quantity;
        } else {
            CancelOrderPayload p{}; std::memcpy(&p, ctx.rx.data() + consumed + 4, sizeof(p));
            msg.order_id = p.order_id; msg.instrument_id = p.instrument_id;
        }
        if (!inbound_.try_push(msg)) break;
        consumed += expected;
    }
    ctx.rx_size -= consumed;
    if (consumed) std::memmove(ctx.rx.data(), ctx.rx.data() + consumed, ctx.rx_size);
    return true;
}
bool TCPGateway::flush(int fd, ClientContext& ctx) noexcept {
    if (ctx.tx_begin == ctx.tx_end) return true;
    const auto n = send(fd, ctx.tx.data() + ctx.tx_begin, ctx.tx_end - ctx.tx_begin, MSG_NOSIGNAL);
    if (n > 0) {
        ctx.tx_begin += static_cast<std::size_t>(n);
        if (ctx.tx_begin == ctx.tx_end) ctx.tx_begin = ctx.tx_end = 0;
        return true;
    }
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
}
int TCPGateway::poll(bool accept_orders) noexcept {
    // Retry buffered ingress after queue pressure clears even without new socket readiness.
    for (auto it = clients_.begin(); it != clients_.end();) {
        const int fd = it->first; auto& ctx = it->second; ++it;
        if (!flush(fd, ctx) || (accept_orders && !process_buffer(fd, ctx))) disconnect(fd);
    }
    epoll_event events[64]; const int n = epoll_wait(epoll_fd_, events, 64, 0);
    for (int i = 0; i < n; ++i) {
        const int fd = events[i].data.fd;
        if (fd == listen_fd_) { if (accept_orders) accept_connection(); continue; }
        auto it = clients_.find(fd); if (it == clients_.end()) continue;
        if (events[i].events & (EPOLLERR | EPOLLHUP)) { disconnect(fd); continue; }
        if (!accept_orders) continue;
        auto& ctx = it->second;
        // Stop reading when bounded RX buffer fills; TCP flow control applies.
        if (ctx.rx_size == ctx.rx.size()) continue;
        const auto bytes = recv(fd, ctx.rx.data() + ctx.rx_size, ctx.rx.size() - ctx.rx_size, 0);
        if (bytes > 0) {
            ctx.rx_size += static_cast<std::size_t>(bytes);
            if (!process_buffer(fd, ctx)) disconnect(fd);
        } else if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) disconnect(fd);
    }
    return n > 0 ? n : 0;
}
void TCPGateway::send_event(const EngineEvent& e) noexcept {
    auto session = sessions_.find(e.connection_fd); if (session == sessions_.end()) return;
    const int fd = session->second; auto& ctx = clients_.find(fd)->second;
    char buf[64]{}; ResponseHeader hdr{};
    if (e.type == EngineEventType::ORDER_ACK || e.type == EngineEventType::CANCEL_ACK) {
        hdr = {20, e.type == EngineEventType::ORDER_ACK ? ResponseType::ACK : ResponseType::CANCEL_ACK, 0};
        AckPayload p{e.order_id, e.client_order_id}; std::memcpy(buf + 4, &p, sizeof(p));
    } else if (e.type == EngineEventType::TRADE) {
        hdr = {36, ResponseType::FILL, 0}; FillPayload p{e.order_id, e.trade_id, e.price, e.fill_quantity, e.remaining_quantity}; std::memcpy(buf + 4, &p, sizeof(p));
    } else if (e.type == EngineEventType::REJECT) {
        hdr = {28, ResponseType::REJECT, 0}; RejectPayload p{e.order_id, e.client_order_id, e.price}; std::memcpy(buf + 4, &p, sizeof(p));
    } else return;
    std::memcpy(buf, &hdr, sizeof(hdr));
    if (ctx.tx.size() - ctx.tx_end < hdr.message_length && ctx.tx_begin) {
        std::memmove(ctx.tx.data(), ctx.tx.data() + ctx.tx_begin, ctx.tx_end - ctx.tx_begin);
        ctx.tx_end -= ctx.tx_begin; ctx.tx_begin = 0;
    }
    if (ctx.tx.size() - ctx.tx_end < hdr.message_length) {
        std::fprintf(stderr, "[TCP] Disconnecting slow session %d: report buffer exhausted\n", ctx.session);
        disconnect(fd); return;
    }
    std::memcpy(ctx.tx.data() + ctx.tx_end, buf, hdr.message_length); ctx.tx_end += hdr.message_length;
    if (!flush(fd, ctx)) disconnect(fd);
}
bool TCPGateway::output_pending() const noexcept {
    for (const auto& [fd, ctx] : clients_) if (ctx.tx_end != ctx.tx_begin) return true;
    return false;
}
}

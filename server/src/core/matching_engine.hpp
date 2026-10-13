#pragma once
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include "core/order_book.hpp"
#include "memory/object_pool.hpp"
#include "memory/spsc_queue.hpp"

namespace fastbid {
struct MatchingEngineConfig { OrderBookConfig book_config; };

// Single engine-thread owner. Matching can pause between fills when outbound
// capacity runs out. The pending aggressor retains priority over later requests.
// A processing call is bounded by batch_size requests AND batch_size fills.
template <std::size_t MaxLevels = 100001, std::size_t PoolCapacity = 1048576, std::size_t QueueSize = 65536>
class MatchingEngine {
    static_assert(QueueSize >= 4);
public:
    using InboundQueue = SPSCQueue<OrderMessage, QueueSize>;
    using OutboundQueue = SPSCQueue<EngineEvent, QueueSize>;
    using Book = OrderBook<MaxLevels, PoolCapacity>;
    using Pool = ObjectPool<Order, PoolCapacity>;
    MatchingEngine(InboundQueue& in, OutboundQueue& out, const MatchingEngineConfig& cfg) noexcept
        : inbound_(in), outbound_(out), book_(cfg.book_config) {}
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    std::size_t process_batch(std::size_t batch_size = 256) noexcept {
        std::size_t processed = 0, fills = 0;
        const bool resuming = pending_ != nullptr;
        while (true) {
            if (pending_) {
                const bool buy = pending_->side == Side::BUY;
                const auto price = buy ? book_.best_ask() : book_.best_bid();
                const bool crosses = buy ? pending_->price >= price : pending_->price <= price;
                if (!pending_->remaining_qty || !crosses) {
                    if (pending_->remaining_qty) {
                        if (!book_.add_order(pending_)) std::abort(); // internal invariant
                    } else pool_.deallocate(pending_);
                    pending_ = nullptr;
                    if (resuming) break;
                } else {
                    if (outbound_.available() < 2 || fills == batch_size) break;
                    Order* resting = book_.get_level(price).front();
                    const auto qty = pending_->remaining_qty < resting->remaining_qty ? pending_->remaining_qty : resting->remaining_qty;
                    assert(qty);
                    pending_->remaining_qty -= qty; resting->remaining_qty -= qty;
                    book_.get_level(price).total_quantity -= qty;
                    const auto trade = next_trade_id_++;
                    emit_fill(pending_, trade, price, qty, pending_->side);
                    emit_fill(resting, trade, price, qty, pending_->side);
                    if (!resting->remaining_qty) { book_.remove_order(resting); pool_.deallocate(resting); }
                    ++fills;
                    continue;
                }
            }
            if (processed == batch_size || outbound_.available() < 2) break;
            OrderMessage msg{};
            if (!inbound_.try_pop(msg)) break;
            ++processed;
            if (msg.instrument_id != book_.config().instrument_id) { reject(msg, 4); continue; }
            if (msg.type == MessageType::NEW_ORDER) start_order(msg);
            else if (msg.type == MessageType::CANCEL || msg.type == MessageType::MODIFY) {
                Order* old = book_.find_order(msg.order_id);
                if (!old) { reject(msg, 3); continue; }
                if (old->connection_fd != msg.connection_fd) { reject(msg, 7); continue; }
                // Validate replacement before mutating the old order.
                if (msg.type == MessageType::MODIFY && !validate(msg)) continue;
                book_.remove_order(old);
                auto evt = event_for(old, EngineEventType::CANCEL_ACK); push(evt);
                pool_.deallocate(old);
                if (msg.type == MessageType::MODIFY) start_order(msg);
            } else reject(msg, 5);
        }
        return processed;
    }
    [[nodiscard]] bool has_pending() const noexcept { return pending_ != nullptr; }
    [[nodiscard]] const Book& book() const noexcept { return book_; }
    [[nodiscard]] const Pool& pool() const noexcept { return pool_; }
    [[nodiscard]] uint64_t next_order_id() const noexcept { return next_order_id_; }
    [[nodiscard]] uint64_t next_trade_id() const noexcept { return next_trade_id_; }
private:
    bool validate(const OrderMessage& msg) noexcept {
        if (!book_.is_price_valid(msg.price)) { reject(msg, 1); return false; }
        if ((msg.side != Side::BUY && msg.side != Side::SELL) || msg.order_type != OrderType::LIMIT || !msg.quantity) {
            reject(msg, 5); return false;
        }
        if (next_order_id_ == UINT64_MAX || next_trade_id_ == UINT64_MAX) { reject(msg, 8); return false; }
        return true;
    }
    void start_order(const OrderMessage& msg) noexcept {
        if (!validate(msg)) return;
        pending_ = pool_.allocate();
        if (!pending_) { reject(msg, 2); return; }
        pending_->init_from_message(msg, next_order_id_++);
        push(event_for(pending_, EngineEventType::ORDER_ACK));
    }
    static EngineEvent event_for(const Order* o, EngineEventType type) noexcept {
        EngineEvent e{}; e.type = type; e.order_id = o->order_id; e.client_order_id = o->client_order_id;
        e.instrument_id = o->instrument_id; e.connection_fd = o->connection_fd; e.timestamp = o->timestamp;
        return e;
    }
    void emit_fill(const Order* o, uint64_t trade, uint64_t price, uint32_t qty, Side side) noexcept {
        auto e = event_for(o, EngineEventType::TRADE); e.trade_id = trade; e.price = price;
        e.fill_quantity = qty; e.remaining_quantity = o->remaining_qty; e.aggressor_side = side; push(e);
    }
    void reject(const OrderMessage& msg, uint64_t reason) noexcept {
        EngineEvent e{}; e.type = EngineEventType::REJECT; e.order_id = msg.order_id;
        e.client_order_id = msg.client_order_id; e.price = reason; e.instrument_id = msg.instrument_id;
        e.connection_fd = msg.connection_fd; e.timestamp = msg.timestamp; push(e);
    }
    void push(const EngineEvent& e) noexcept {
        // All mutations reserve their worst-case event count first. Never drop reports.
        if (!outbound_.try_push(e)) std::abort();
    }
    InboundQueue& inbound_; OutboundQueue& outbound_;
    Book book_; Pool pool_;
    Order* pending_ = nullptr;
    uint64_t next_order_id_ = 1, next_trade_id_ = 1;
};
}

#pragma once
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <array>
#include <optional>
#include <span>
#include "core/order_book.hpp"
#include "core/basket.hpp"
#include "memory/object_pool.hpp"
#include "memory/spsc_queue.hpp"

namespace fastbid {
struct MatchingEngineConfig {
    OrderBookConfig book_config;
    std::span<const OrderBookConfig> additional_books{};
};

// Single engine-thread owner. Matching can pause between fills when outbound
// capacity runs out. The pending aggressor retains priority over later requests.
// A processing call is bounded by batch_size requests AND batch_size fills.
template <std::size_t MaxLevels = 100001, std::size_t PoolCapacity = 1048576, std::size_t QueueSize = 65536, std::size_t MaxProducts = 1>
class MatchingEngine {
    static_assert(QueueSize >= 4);
    static_assert(MaxProducts > 0);
public:
    using InboundQueue = SPSCQueue<OrderMessage, QueueSize>;
    using OutboundQueue = SPSCQueue<EngineEvent, QueueSize>;
    using Book = OrderBook<MaxLevels, PoolCapacity>;
    using Pool = ObjectPool<Order, PoolCapacity>;
    MatchingEngine(InboundQueue& in, OutboundQueue& out, const MatchingEngineConfig& cfg) noexcept
        : inbound_(in), outbound_(out) {
        if (cfg.additional_books.size() >= MaxProducts) std::abort();
        books_[0].emplace(cfg.book_config);
        if (cfg.book_config.instrument_id >= BASKET_ID_BASE) std::abort();
        product_count_ = 1;
        for (const auto& config : cfg.additional_books) {
            if (book(config.instrument_id) || config.instrument_id >= BASKET_ID_BASE) std::abort();
            books_[product_count_++].emplace(config);
        }
        book_ = &*books_[0];
        base_product_count_ = product_count_;
    }
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    std::size_t process_batch(std::size_t batch_size = 256) noexcept {
        std::size_t processed = 0, fills = 0;
        const bool resuming = pending_ != nullptr;
        while (true) {
            if (pending_) {
                const bool buy = pending_->side == Side::BUY;
                const auto price = buy ? book_->best_ask() : book_->best_bid();
                const bool crosses = buy ? pending_->price >= price : pending_->price <= price;
                if (!pending_->remaining_qty || !crosses) {
                    if (pending_->remaining_qty) {
                        if (!book_->add_order(pending_)) std::abort(); // internal invariant
                    } else pool_.deallocate(pending_);
                    pending_ = nullptr;
                    if (resuming) break;
                } else {
                    if (outbound_.available() < 2 || fills == batch_size) break;
                    Order* resting = book_->get_level(price).front();
                    const auto qty = pending_->remaining_qty < resting->remaining_qty ? pending_->remaining_qty : resting->remaining_qty;
                    assert(qty);
                    pending_->remaining_qty -= qty; resting->remaining_qty -= qty;
                    book_->get_level(price).total_quantity -= qty;
                    const auto trade = next_trade_id_++;
                    emit_fill(pending_, trade, price, qty, pending_->side);
                    emit_fill(resting, trade, price, qty, pending_->side);
                    if (!resting->remaining_qty) { book_->remove_order(resting); pool_.deallocate(resting); }
                    ++fills;
                    continue;
                }
            }
            if (processed == batch_size || outbound_.available() < 2) break;
            OrderMessage msg{};
            if (!inbound_.try_pop(msg)) break;
            ++processed;
            Book* selected = nullptr;
            for (std::size_t i = 0; i < product_count_; ++i)
                if (books_[i]->config().instrument_id == msg.instrument_id) { selected = &*books_[i]; break; }
            if (!selected) { reject(msg, 4); continue; }
            book_ = selected;
            if (msg.type == MessageType::NEW_ORDER) start_order(msg);
            else if (msg.type == MessageType::CANCEL || msg.type == MessageType::MODIFY) {
                Order* old = book_->find_order(msg.order_id);
                if (!old) { reject(msg, 3); continue; }
                if (old->connection_fd != msg.connection_fd) { reject(msg, 7); continue; }
                // Validate replacement before mutating the old order.
                if (msg.type == MessageType::MODIFY && !validate(msg)) continue;
                book_->remove_order(old);
                auto evt = event_for(old, EngineEventType::CANCEL_ACK); push(evt);
                pool_.deallocate(old);
                if (msg.type == MessageType::MODIFY) start_order(msg);
            } else reject(msg, 5);
        }
        return processed;
    }
    [[nodiscard]] bool has_pending() const noexcept { return pending_ != nullptr; }
    [[nodiscard]] const Book& book() const noexcept { return *books_[0]; }
    [[nodiscard]] const Book* book(uint32_t instrument_id) const noexcept {
        for (std::size_t i = 0; i < product_count_; ++i)
            if (books_[i]->config().instrument_id == instrument_id) return &*books_[i];
        return nullptr;
    }
    [[nodiscard]] const Pool& pool() const noexcept { return pool_; }
    [[nodiscard]] uint64_t next_order_id() const noexcept { return next_order_id_; }
    [[nodiscard]] uint64_t next_trade_id() const noexcept { return next_trade_id_; }
    [[nodiscard]] std::size_t instrument_count() const noexcept { return product_count_; }
    [[nodiscard]] const Book& book_at(std::size_t i) const noexcept { assert(i < product_count_); return *books_[i]; }
    [[nodiscard]] std::span<const BasketDefinition> baskets() const noexcept { return {baskets_.data(), basket_count_}; }

    // Control plane only, engine owner thread, between completed requests.
    BasketRegistration register_basket(const BasketRequest& request) noexcept {
        BasketRegistration result{};
        auto invalid = [&] { result.reason = 9; return result; };
        if (pending_) { result.reason = 11; return result; }
        if (request.reserved || request.leg_count < 2 || request.leg_count > MAX_BASKET_LEGS) return invalid();
        std::size_t name_length = 0;
        while (name_length < sizeof(request.name) && request.name[name_length]) {
            const auto ch = static_cast<unsigned char>(request.name[name_length]);
            if (ch < 32 || ch > 126) return invalid();
            ++name_length;
        }
        if (!name_length || request.name[0] == ' ' || request.name[name_length - 1] == ' ') return invalid();
        for (std::size_t i = name_length; i < sizeof(request.name); ++i) if (request.name[i]) return invalid();
        std::array<BasketLeg, MAX_BASKET_LEGS> legs{};
        uint64_t min = 0, max = 0, reference = 0;
        uint32_t total = 0;
        for (std::size_t i = 0; i < request.leg_count; ++i) {
            const auto leg = request.legs[i];
            if (!leg.quantity || leg.quantity > MAX_LEG_QUANTITY || leg.quantity > MAX_BASKET_TOTAL_QUANTITY - total) return invalid();
            total += leg.quantity;
            const OrderBookConfig* config = nullptr;
            for (std::size_t j = 0; j < base_product_count_; ++j)
                if (books_[j]->config().instrument_id == leg.instrument_id) config = &books_[j]->config();
            if (!config || config->max_price > (UINT64_MAX - max) / leg.quantity ||
                config->min_price > (UINT64_MAX - min) / leg.quantity ||
                config->reference_price > (UINT64_MAX - reference) / leg.quantity) return invalid();
            min += config->min_price * leg.quantity; max += config->max_price * leg.quantity;
            reference += config->reference_price * leg.quantity;
            std::size_t pos = i;
            while (pos && legs[pos - 1].instrument_id > leg.instrument_id) { legs[pos] = legs[pos - 1]; --pos; }
            legs[pos] = leg;
        }
        for (std::size_t i = 1; i < request.leg_count; ++i)
            if (legs[i - 1].instrument_id == legs[i].instrument_id) return invalid();
        for (std::size_t i = request.leg_count; i < MAX_BASKET_LEGS; ++i)
            if (request.legs[i].instrument_id || request.legs[i].quantity) return invalid();
        for (std::size_t i = 0; i < basket_count_; ++i) {
            const auto& existing = baskets_[i];
            if (existing.leg_count != request.leg_count) continue;
            bool same = true;
            for (std::size_t j = 0; j < request.leg_count; ++j)
                if (existing.legs[j].instrument_id != legs[j].instrument_id || existing.legs[j].quantity != legs[j].quantity) same = false;
            if (same) { result.definition = existing; return result; }
        }
        if (basket_count_ == MAX_BASKETS || product_count_ == MaxProducts) { result.reason = 10; return result; }
        if constexpr (MaxLevels < 2) { if (max != min) return invalid(); }
        const uint64_t range = max - min;
        constexpr std::size_t intervals = MaxLevels > 1 ? MaxLevels - 1 : 1;
        const uint64_t tick = range ? range / intervals + (range % intervals != 0) : 1;
        if (!min || max == UINT64_MAX || tick > UINT32_MAX) return invalid();
        BasketDefinition definition{};
        definition.instrument_id = BASKET_ID_BASE + static_cast<uint32_t>(basket_count_);
        definition.tick_size = static_cast<uint32_t>(tick);
        definition.min_price = min; definition.max_price = min + range / tick * tick;
        definition.reference_price = min + (reference - min) / tick * tick;
        definition.leg_count = request.leg_count;
        std::memcpy(definition.name, request.name, name_length);
        for (std::size_t i = 0; i < request.leg_count; ++i) definition.legs[i] = legs[i];
        books_[product_count_].emplace(OrderBookConfig{definition.min_price, definition.max_price,
            definition.reference_price, definition.instrument_id, definition.tick_size});
        ++product_count_; baskets_[basket_count_++] = definition;
        result.definition = definition; return result;
    }
private:
    bool validate(const OrderMessage& msg) noexcept {
        if (!book_->is_price_valid(msg.price)) { reject(msg, 1); return false; }
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
    std::array<std::optional<Book>, MaxProducts> books_;
    std::size_t product_count_ = 0;
    std::size_t base_product_count_ = 0, basket_count_ = 0;
    std::array<BasketDefinition, MAX_BASKETS> baskets_{};
    Book* book_ = nullptr;
    Pool pool_;
    Order* pending_ = nullptr;
    uint64_t next_order_id_ = 1, next_trade_id_ = 1;
};
}

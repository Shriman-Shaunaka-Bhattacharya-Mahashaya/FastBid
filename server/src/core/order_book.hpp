#pragma once
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "core/price_level.hpp"
#include "network/market_data.hpp"

namespace fastbid {
struct OrderBookConfig {
    uint64_t min_price, max_price, reference_price;
    uint32_t instrument_id;
    uint32_t tick_size = 1;
    [[nodiscard]] uint64_t num_levels() const noexcept { return tick_size ? (max_price - min_price) / tick_size + 1 : UINT64_MAX; }
};

// One level array is sufficient because a matched book never rests crossed sides.
// Price occupancy bitmap skips empty ticks; order IDs use intrusive hash chains.
// No allocations after construction. Expected O(1) lookup; worst case O(active orders).
template <std::size_t MaxLevels, std::size_t IndexBuckets = 65536>
class OrderBook {
    static_assert(MaxLevels > 0 && IndexBuckets > 0);
public:
    explicit OrderBook(const OrderBookConfig& config) noexcept : config_(config) {
        // Runtime checks must remain active in release builds. Reserve price sentinels.
        if (!config.tick_size || config.min_price == 0 || config.max_price == UINT64_MAX ||
            config.min_price > config.max_price || config.num_levels() > MaxLevels ||
            config.instrument_id == 0 || config.reference_price < config.min_price ||
            config.reference_price > config.max_price || (config.max_price - config.min_price) % config.tick_size ||
            (config.reference_price - config.min_price) % config.tick_size) {
            std::fputs("Invalid order book configuration\n", stderr);
            std::abort();
        }
        for (std::size_t i = 0; i < config.num_levels(); ++i) levels_[i].init(config.min_price + i * config.tick_size);
    }
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    [[nodiscard]] bool is_price_valid(uint64_t p) const noexcept {
        return p >= config_.min_price && p <= config_.max_price && (p - config_.min_price) % config_.tick_size == 0;
    }
    bool add_order(Order* o) noexcept {
        if (!o || !is_price_valid(o->price) || !o->order_id || !o->remaining_qty ||
            o->instrument_id != config_.instrument_id ||
            (o->side != Side::BUY && o->side != Side::SELL) || find_order(o->order_id)) return false;
        if ((o->side == Side::BUY && o->price >= ask_) || (o->side == Side::SELL && o->price <= bid_)) return false;
        auto& level = get_level(o->price);
        if (!level.empty() && level.front()->side != o->side) return false;
        level.push_back(o);
        auto& bucket = index_[hash(o->order_id)];
        o->index_next = bucket; bucket = o;
        auto i = (o->price - config_.min_price) / config_.tick_size;
        occupancy_[i / 64] |= uint64_t{1} << (i % 64);
        if (o->side == Side::BUY) { ++bids_; if (o->price > bid_) bid_ = o->price; }
        else { ++asks_; if (o->price < ask_) ask_ = o->price; }
        return true;
    }
    [[nodiscard]] Order* find_order(uint64_t id) const noexcept {
        for (Order* o = index_[hash(id)]; o; o = o->index_next) if (o->order_id == id) return o;
        return nullptr;
    }
    Order* cancel_order(uint64_t id) noexcept {
        Order* o = find_order(id); if (o) remove_order(o); return o;
    }
    void remove_order(Order* o) noexcept {
        assert(o && find_order(o->order_id) == o);
        Order** link = &index_[hash(o->order_id)];
        while (*link != o) link = &(*link)->index_next;
        *link = o->index_next; o->index_next = nullptr;
        auto& level = get_level(o->price);
        level.remove(o);
        const auto idx = (o->price - config_.min_price) / config_.tick_size;
        if (level.empty()) occupancy_[idx / 64] &= ~(uint64_t{1} << (idx % 64));
        if (o->side == Side::BUY) {
            --bids_;
            if (!bids_) bid_ = 0;
            else if (o->price == bid_ && level.empty()) {
                auto word = idx / 64;
                auto bits = occupancy_[word] & (UINT64_MAX >> (63 - idx % 64));
                while (!bits && word) bits = occupancy_[--word];
                assert(bits); bid_ = config_.min_price + (word * 64 + 63 - static_cast<uint64_t>(std::countl_zero(bits))) * config_.tick_size;
            }
        } else {
            --asks_;
            if (!asks_) ask_ = UINT64_MAX;
            else if (o->price == ask_ && level.empty()) {
                auto word = idx / 64;
                auto bits = occupancy_[word] & (UINT64_MAX << (idx % 64));
                while (!bits && word + 1 < WORDS) bits = occupancy_[++word];
                assert(bits); ask_ = config_.min_price + (word * 64 + static_cast<uint64_t>(std::countr_zero(bits))) * config_.tick_size;
            }
        }
    }
    // Called only by the engine owner at a completed request boundary.
    void snapshot(MarketSnapshot& out) const noexcept {
        out = MarketSnapshot{};
        out.header.instrument_id = config_.instrument_id;
        if (asks_) {
            const auto start = (ask_ - config_.min_price) / config_.tick_size;
            for (std::size_t w = start / 64; w < WORDS && out.ask_count < 10; ++w) {
                auto bits = occupancy_[w];
                if (w == start / 64) bits &= UINT64_MAX << (start % 64);
                while (bits && out.ask_count < 10) {
                    const auto& level = levels_[w * 64 + static_cast<std::size_t>(std::countr_zero(bits))];
                    out.asks[out.ask_count++] = {level.price, level.total_quantity};
                    bits &= bits - 1;
                }
            }
        }
        if (bids_) {
            const auto start = (bid_ - config_.min_price) / config_.tick_size;
            for (std::size_t w = start / 64 + 1; w-- > 0 && out.bid_count < 10;) {
                auto bits = occupancy_[w];
                if (w == start / 64) bits &= UINT64_MAX >> (63 - start % 64);
                while (bits && out.bid_count < 10) {
                    auto bit = 63 - std::countl_zero(bits);
                    const auto& level = levels_[w * 64 + static_cast<std::size_t>(bit)];
                    out.bids[out.bid_count++] = {level.price, level.total_quantity};
                    bits &= ~(uint64_t{1} << bit);
                }
            }
        }
    }
    [[nodiscard]] uint64_t best_bid() const noexcept { return bid_; }
    [[nodiscard]] uint64_t best_ask() const noexcept { return ask_; }
    [[nodiscard]] PriceLevel& get_level(uint64_t p) noexcept { assert(is_price_valid(p)); return levels_[(p - config_.min_price) / config_.tick_size]; }
    [[nodiscard]] const PriceLevel& get_level(uint64_t p) const noexcept { assert(is_price_valid(p)); return levels_[(p - config_.min_price) / config_.tick_size]; }
    [[nodiscard]] uint32_t total_bid_orders() const noexcept { return bids_; }
    [[nodiscard]] uint32_t total_ask_orders() const noexcept { return asks_; }
    [[nodiscard]] uint32_t total_orders() const noexcept { return bids_ + asks_; }
    [[nodiscard]] const OrderBookConfig& config() const noexcept { return config_; }
private:
    static constexpr std::size_t WORDS = (MaxLevels + 63) / 64;
    static std::size_t hash(uint64_t id) noexcept {
        id ^= id >> 30; id *= 0xbf58476d1ce4e5b9ULL;
        id ^= id >> 27; id *= 0x94d049bb133111ebULL;
        return (id ^ (id >> 31)) % IndexBuckets;
    }
    OrderBookConfig config_;
    PriceLevel levels_[MaxLevels]{};
    uint64_t occupancy_[WORDS]{};
    Order* index_[IndexBuckets]{};
    uint64_t bid_ = 0, ask_ = UINT64_MAX;
    uint32_t bids_ = 0, asks_ = 0;
};
}

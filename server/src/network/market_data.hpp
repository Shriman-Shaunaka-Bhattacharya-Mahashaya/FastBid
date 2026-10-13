#pragma once
#include <cstdint>
#include "core/types.hpp"
namespace fastbid {
// Explicit versioned little-endian market-data packets. Never transmit internal
// structs, connection identities or compiler padding. Full top-10 snapshots heal loss.
#pragma pack(push, 1)
struct MarketHeader { uint32_t magic = 0x31444246; uint16_t version = 1; uint16_t kind = 2; uint64_t sequence = 0; };
struct DepthLevel { uint64_t price = 0, quantity = 0; };
struct MarketSnapshot {
    MarketHeader header{};
    uint32_t bid_count = 0, ask_count = 0;
    DepthLevel bids[10]{}, asks[10]{};
    uint64_t p50_ns = 0, p99_ns = 0, orders_per_sec = 0, samples = 0, dropped_samples = 0;
};
struct MarketTrade {
    MarketHeader header{};
    uint64_t trade_id = 0, price = 0;
    uint32_t quantity = 0;
    Side side = Side::BUY;
    uint8_t reserved[3]{};
};
#pragma pack(pop)
static_assert(sizeof(MarketSnapshot) == 384);
static_assert(sizeof(MarketTrade) == 40);
}

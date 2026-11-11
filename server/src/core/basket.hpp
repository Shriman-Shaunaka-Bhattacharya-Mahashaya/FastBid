#pragma once
#include <cstddef>
#include <cstdint>
#include "core/types.hpp"
#include "memory/spsc_queue.hpp"
namespace fastbid {
inline constexpr std::size_t MAX_BASKETS = 8;
inline constexpr std::size_t MAX_BASKET_LEGS = 16;
inline constexpr uint32_t MAX_LEG_QUANTITY = 1000;
inline constexpr uint32_t MAX_BASKET_TOTAL_QUANTITY = 10000;
inline constexpr uint32_t BASKET_ID_BASE = 0x80000000u;
struct BasketLeg { uint32_t instrument_id; uint32_t quantity; };
#pragma pack(push, 1)
struct BasketRequest {
    uint64_t request_id;
    uint16_t leg_count, reserved;
    char name[48];
    BasketLeg legs[MAX_BASKET_LEGS];
};
#pragma pack(pop)
// Explicit tail prevents implicit padding in the byte layout. Internally aligned.
struct BasketDefinition {
    uint32_t instrument_id, tick_size;
    uint64_t min_price, max_price, reference_price;
    char name[48];
    uint16_t leg_count, reserved;
    BasketLeg legs[MAX_BASKET_LEGS];
    uint32_t reserved_end;
};
struct BasketRegistration { uint64_t reason = 0; BasketDefinition definition{}; };
struct ControlRequest { MessageType type; int session; BasketRequest request; };
struct ControlResponse {
    ResponseType type;
    int session;
    uint64_t request_id, reason;
    BasketDefinition definition;
};
using ControlInboundQueue = SPSCQueue<ControlRequest, 32>;
using ControlOutboundQueue = SPSCQueue<ControlResponse, 32>;
static_assert(sizeof(BasketRequest) == 188);
static_assert(sizeof(BasketDefinition) == 216);
static_assert(offsetof(BasketDefinition, legs) == 84);
}

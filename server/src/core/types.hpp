#pragma once

/// @file types.hpp
/// @brief Core type definitions, enums, and constants for the FastBid engine.
///
/// All fundamental types used across the matching engine are defined here.
/// This file has ZERO dependencies beyond the standard library.

#include <cstdint>
#include <cstddef>
#include <bit>
#include <cstring>

namespace fastbid {

static_assert(std::endian::native == std::endian::little, "Wire protocol requires little-endian hosts");

// ─── Constants ───

/// Cache line size in bytes (x86 and ARM Cortex-A series)
inline constexpr std::size_t CACHE_LINE_SIZE = 64;

/// Fixed-point multiplier for price representation.
/// Price in ticks = price_dollars * PRICE_MULTIPLIER.
/// e.g., $100.5025 → 1005025
inline constexpr uint64_t PRICE_MULTIPLIER = 10000;

/// Invalid/sentinel values
inline constexpr uint64_t INVALID_ORDER_ID = 0;
inline constexpr uint64_t INVALID_PRICE    = 0;
inline constexpr uint32_t INVALID_INSTRUMENT = 0;

// ─── Enums ───

/// Order side: Buy or Sell
enum class Side : uint8_t {
    BUY  = 1,
    SELL = 2
};

/// Order type (currently only LIMIT is supported)
enum class OrderType : uint8_t {
    LIMIT = 1
};

/// Inbound message types (TCP client → gateway)
enum class MessageType : uint8_t {
    NEW_ORDER = 1,
    CANCEL    = 2,
    MODIFY    = 3,
    REGISTER_BASKET = 4,
    SYNC_BASKETS = 5
};

/// Outbound message types (gateway → TCP client)
enum class ResponseType : uint8_t {
    ACK        = 10,
    FILL       = 11,
    CANCEL_ACK = 12,
    REJECT     = 13,
    BASKET_REPORT = 14,
    BASKET_SYNC_DONE = 15
};

/// Engine event types (engine → outbound queue)
enum class EngineEventType : uint8_t {
    ORDER_ACK    = 1,
    TRADE        = 2,
    CANCEL_ACK   = 3,
    REJECT       = 4,
    BOOK_UPDATE  = 5
};

// ─── Message Structs (carried through SPSC queues) ───

/// @brief Lightweight order message carried through the inbound SPSC queue.
///
/// This is NOT the Order object itself. The engine thread allocates an Order
/// from the object pool upon dequeue and populates it from this message.
/// This keeps pool access single-threaded (zero contention).
struct alignas(CACHE_LINE_SIZE) OrderMessage {
    MessageType  type;
    Side         side;
    OrderType    order_type;
    uint8_t      _pad0;            // explicit padding
    uint32_t     instrument_id;
    uint64_t     client_order_id;
    uint64_t     order_id;         // exchange-assigned (for CANCEL/MODIFY)
    uint64_t     price;            // fixed-point
    uint32_t     quantity;
    int          connection_fd;    // for routing execution reports back
    uint64_t     timestamp;        // nanosecond timestamp from client
};

/// @brief Outbound engine event carried through the outbound SPSC queue.
///
/// Produced by the engine thread after matching. Consumed by the IO thread
/// for UDP multicast publishing and WebSocket broadcasting.
struct alignas(CACHE_LINE_SIZE) EngineEvent {
    EngineEventType type;
    Side            aggressor_side;
    uint8_t         _pad0[2];       // explicit padding
    uint32_t        instrument_id;
    uint64_t        order_id;
    uint64_t        trade_id;
    uint64_t        price;
    uint32_t        fill_quantity;
    uint32_t        remaining_quantity;
    int             connection_fd;  // for routing execution reports
    uint64_t        client_order_id;
    uint64_t        timestamp;
};

// ─── Protocol Structs (wire format, packed) ───

#pragma pack(push, 1)

/// Binary protocol header (4 bytes, fixed)
struct ProtocolHeader {
    uint16_t    message_length;  // including header
    MessageType message_type;
    uint8_t     reserved;
};

/// NewOrder payload (34 bytes, fixed)
struct NewOrderPayload {
    uint64_t  client_order_id;
    uint32_t  instrument_id;
    Side      side;
    OrderType order_type;
    uint64_t  price;
    uint32_t  quantity;
    uint64_t  timestamp;
};

/// CancelOrder payload (12 bytes, fixed)
struct CancelOrderPayload {
    uint64_t  order_id;
    uint32_t  instrument_id;
};

/// Response header (4 bytes, fixed)
struct ResponseHeader {
    uint16_t     message_length;
    ResponseType response_type;
    uint8_t      reserved;
};

/// Fill response payload (32 bytes, fixed)
struct FillPayload {
    uint64_t order_id;
    uint64_t trade_id;
    uint64_t fill_price;
    uint32_t fill_quantity;
    uint32_t remaining_quantity;
};

/// Ack response payload (16 bytes, fixed)
struct AckPayload {
    uint64_t order_id;
    uint64_t client_order_id;
};

/// Reject response payload (24 bytes, fixed)
struct RejectPayload {
    uint64_t order_id;
    uint64_t client_order_id;
    uint64_t reason_code;       // enumerated reject reason
};

#pragma pack(pop)

// Compile-time size verification for wire protocol structs
static_assert(sizeof(ProtocolHeader) == 4,     "ProtocolHeader must be exactly 4 bytes");
static_assert(sizeof(NewOrderPayload) == 34,   "NewOrderPayload must be exactly 34 bytes");
static_assert(sizeof(CancelOrderPayload) == 12, "CancelOrderPayload must be exactly 12 bytes");
static_assert(sizeof(FillPayload) == 32,        "FillPayload must be exactly 32 bytes");

} // namespace fastbid

#pragma once

/// @file order.hpp
/// @brief Order struct with intrusive doubly-linked list pointers.
///
/// Each Order lives in the ObjectPool and is linked into a PriceLevel's
/// FIFO queue via intrusive prev/next pointers. This avoids separate
/// node allocations for the linked list.
///
/// The struct is cache-line aligned (64 bytes) to prevent false sharing
/// when multiple orders are accessed sequentially.

#include <cstdint>
#include "core/types.hpp"

namespace fastbid {

/// @brief A single limit order in the order book.
///
/// Allocated from the ObjectPool by the engine thread.
/// Linked into a PriceLevel's doubly-linked list via intrusive pointers.
struct alignas(CACHE_LINE_SIZE) Order {
    // ─── Identity ───
    uint64_t order_id;           ///< Exchange-assigned unique order ID
    uint64_t client_order_id;    ///< Client-assigned order ID (for execution reports)
    uint32_t instrument_id;      ///< Symbol/instrument identifier

    // ─── Order Parameters ───
    Side     side;               ///< BUY or SELL
    uint8_t  _pad0[3];           ///< Explicit padding for alignment
    uint64_t price;              ///< Price in fixed-point ticks (price × PRICE_MULTIPLIER)
    uint32_t quantity;           ///< Original order quantity
    uint32_t remaining_qty;      ///< Remaining unfilled quantity

    // ─── Timing ───
    uint64_t timestamp;          ///< Nanosecond timestamp (arrival time)

    // ─── Intrusive List Pointers (within a PriceLevel) ───
    Order*   next;               ///< Next order in the price level's FIFO queue
    Order*   index_next;         ///< Intrusive order-ID hash chain
    Order*   prev;               ///< Previous order in the price level's FIFO queue

    // ─── Connection Routing ───
    int      connection_fd;      ///< File descriptor of the originating TCP connection
    uint8_t  _pad1[4];           ///< Padding to round struct size

    /// @brief Initialize an Order from an OrderMessage.
    /// Sets up all fields and clears the intrusive list pointers.
    void init_from_message(const OrderMessage& msg, uint64_t assigned_order_id) noexcept {
        order_id        = assigned_order_id;
        client_order_id = msg.client_order_id;
        instrument_id   = msg.instrument_id;
        side            = msg.side;
        price           = msg.price;
        quantity        = msg.quantity;
        remaining_qty   = msg.quantity;
        timestamp       = msg.timestamp;
        connection_fd   = msg.connection_fd;
        index_next      = nullptr;
        next            = nullptr;
        prev            = nullptr;
    }
};

// Verify the struct fits well within cache line boundaries
static_assert(sizeof(Order) <= 2 * CACHE_LINE_SIZE,
              "Order struct should fit within 2 cache lines");
static_assert(alignof(Order) == CACHE_LINE_SIZE,
              "Order struct must be cache-line aligned");

} // namespace fastbid

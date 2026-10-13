#pragma once

/// @file price_level.hpp
/// @brief PriceLevel struct — a FIFO queue of orders at a single price.
///
/// Each PriceLevel maintains a doubly-linked list of Order objects
/// (via intrusive pointers). Orders are added at the tail (newest)
/// and matched from the head (oldest) — implementing price-time priority.
///
/// All operations are O(1).

#include <cassert>
#include <cstdint>
#include "core/order.hpp"

namespace fastbid {

/// @brief A single price level in the order book.
///
/// Manages a FIFO queue of Order objects at a specific price.
/// Orders are linked via their intrusive prev/next pointers.
struct PriceLevel {
    uint64_t price;              ///< Price in fixed-point ticks
    uint64_t total_quantity;     ///< Aggregate quantity across all orders at this level
    uint32_t order_count;        ///< Number of active orders at this level
    Order*   head;               ///< Front of queue (oldest order, highest time priority)
    Order*   tail;               ///< Back of queue (newest order)

    /// @brief Initialize/reset the price level.
    void init(uint64_t lvl_price) noexcept {
        price          = lvl_price;
        total_quantity = 0;
        order_count    = 0;
        head           = nullptr;
        tail           = nullptr;
    }

    /// @brief Check if this price level has any orders.
    [[nodiscard]] bool empty() const noexcept {
        return head == nullptr;
    }

    /// @brief Add an order to the back of the queue (newest, lowest time priority).
    /// O(1).
    void push_back(Order* order) noexcept {
        assert(order != nullptr);
        assert(order->price == price && "Order price must match level price");

        order->next = nullptr;
        order->prev = tail;

        if (tail != nullptr) {
            tail->next = order;
        } else {
            // Queue was empty; this order is also the head.
            head = order;
        }
        tail = order;

        total_quantity += order->remaining_qty;
        ++order_count;
    }

    /// @brief Remove a specific order from anywhere in the queue.
    /// O(1) because we have prev/next pointers.
    void remove(Order* order) noexcept {
        assert(order != nullptr);

        if (order->prev != nullptr) {
            order->prev->next = order->next;
        } else {
            // This order was the head.
            head = order->next;
        }

        if (order->next != nullptr) {
            order->next->prev = order->prev;
        } else {
            // This order was the tail.
            tail = order->prev;
        }

        total_quantity -= order->remaining_qty;
        --order_count;

        // Clear the order's list pointers (defensive).
        order->next = nullptr;
        order->prev = nullptr;
    }

    /// @brief Peek at the front order without removing it.
    [[nodiscard]] Order* front() const noexcept {
        return head;
    }

    /// @brief Remove and return the front order (oldest, highest time priority).
    /// O(1).
    [[nodiscard]] Order* pop_front() noexcept {
        if (head == nullptr) {
            return nullptr;
        }

        Order* order = head;
        head = order->next;

        if (head != nullptr) {
            head->prev = nullptr;
        } else {
            tail = nullptr;  // Queue is now empty
        }

        total_quantity -= order->remaining_qty;
        --order_count;

        order->next = nullptr;
        order->prev = nullptr;

        return order;
    }
};

} // namespace fastbid

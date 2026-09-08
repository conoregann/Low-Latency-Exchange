#include "low_latency_exchange/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace low_latency_exchange {

SubmitResult OrderBook::submit(const NewOrder& order) {
    if (validate(order).has_value()) {
        return {.executions = {},
                .rejection = OrderRejectReason::invalid_order,
                .remaining_quantity = std::nullopt};
    }
    if (order.type != OrderType::limit) {
        return {.executions = {},
                .rejection = OrderRejectReason::market_orders_not_supported,
                .remaining_quantity = std::nullopt};
    }
    if (seen_order_ids_.contains(order.order_id)) {
        return {.executions = {},
                .rejection = OrderRejectReason::duplicate_order_id,
                .remaining_quantity = std::nullopt};
    }

    seen_order_ids_.insert(order.order_id);
    const Price limit_price = *order.limit_price;
    std::uint64_t remaining_units = order.quantity.units();
    SubmitResult result;

    if (order.side == Side::buy) {
        while (remaining_units > 0 && !asks_.empty() && asks_.begin()->first <= limit_price) {
            auto level = asks_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                result.executions.push_back(
                    {resting_order.order_id, order.order_id, execution_price, executed_quantity});
                remaining_units -= executed_units;

                const std::uint64_t resting_remaining =
                    resting_order.remaining_quantity.units() - executed_units;
                if (resting_remaining == 0) {
                    orders.pop_front();
                } else {
                    resting_order.remaining_quantity = *Quantity::from_units(resting_remaining);
                }
            }
            if (orders.empty()) {
                asks_.erase(level);
            }
        }
    } else {
        while (remaining_units > 0 && !bids_.empty() && bids_.begin()->first >= limit_price) {
            auto level = bids_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                result.executions.push_back(
                    {resting_order.order_id, order.order_id, execution_price, executed_quantity});
                remaining_units -= executed_units;

                const std::uint64_t resting_remaining =
                    resting_order.remaining_quantity.units() - executed_units;
                if (resting_remaining == 0) {
                    orders.pop_front();
                } else {
                    resting_order.remaining_quantity = *Quantity::from_units(resting_remaining);
                }
            }
            if (orders.empty()) {
                bids_.erase(level);
            }
        }
    }

    if (remaining_units > 0) {
        const Quantity remaining_quantity = *Quantity::from_units(remaining_units);
        RestingOrder resting_order{order.order_id, order.sequence, remaining_quantity};
        if (order.side == Side::buy) {
            bids_[limit_price].push_back(resting_order);
        } else {
            asks_[limit_price].push_back(resting_order);
        }
        result.remaining_quantity = remaining_quantity;
    }

    return result;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::size_t OrderBook::resting_order_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [price, orders] : bids_) {
        static_cast<void>(price);
        count += orders.size();
    }
    for (const auto& [price, orders] : asks_) {
        static_cast<void>(price);
        count += orders.size();
    }
    return count;
}

}  // namespace low_latency_exchange

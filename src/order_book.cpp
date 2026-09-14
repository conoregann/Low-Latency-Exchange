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
    if (seen_order_ids_.contains(order.order_id)) {
        return {.executions = {},
                .rejection = OrderRejectReason::duplicate_order_id,
                .remaining_quantity = std::nullopt};
    }

    seen_order_ids_.insert(order.order_id);
    std::uint64_t remaining_units = order.quantity.units();
    SubmitResult result;

    if (order.side == Side::buy) {
        while (remaining_units > 0 && !asks_.empty() &&
               (order.type == OrderType::market || asks_.begin()->first <= *order.limit_price)) {
            auto level = asks_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second.orders;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                result.executions.push_back(
                    {resting_order.order_id, order.order_id, execution_price, executed_quantity});
                remaining_units -= executed_units;
                level->second.total_units -= executed_units;

                const std::uint64_t resting_remaining =
                    resting_order.remaining_quantity.units() - executed_units;
                if (resting_remaining == 0) {
                    resting_orders_.erase(resting_order.order_id);
                    orders.pop_front();
                    --resting_order_count_;
                } else {
                    resting_order.remaining_quantity = *Quantity::from_units(resting_remaining);
                }
            }
            if (orders.empty()) {
                asks_.erase(level);
            }
        }
    } else {
        while (remaining_units > 0 && !bids_.empty() &&
               (order.type == OrderType::market || bids_.begin()->first >= *order.limit_price)) {
            auto level = bids_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second.orders;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                result.executions.push_back(
                    {resting_order.order_id, order.order_id, execution_price, executed_quantity});
                remaining_units -= executed_units;
                level->second.total_units -= executed_units;

                const std::uint64_t resting_remaining =
                    resting_order.remaining_quantity.units() - executed_units;
                if (resting_remaining == 0) {
                    resting_orders_.erase(resting_order.order_id);
                    orders.pop_front();
                    --resting_order_count_;
                } else {
                    resting_order.remaining_quantity = *Quantity::from_units(resting_remaining);
                }
            }
            if (orders.empty()) {
                bids_.erase(level);
            }
        }
    }

    if (order.type == OrderType::limit && remaining_units > 0) {
        const Quantity remaining_quantity = *Quantity::from_units(remaining_units);
        RestingOrder resting_order{order.order_id, order.sequence, remaining_quantity};
        const Price price = *order.limit_price;
        if (order.side == Side::buy) {
            auto& level = bids_[price];
            level.total_units += remaining_units;
            level.orders.push_back(resting_order);
            auto it = std::prev(level.orders.end());
            resting_orders_.emplace(order.order_id, OrderLocation{Side::buy, price, it});
        } else {
            auto& level = asks_[price];
            level.total_units += remaining_units;
            level.orders.push_back(resting_order);
            auto it = std::prev(level.orders.end());
            resting_orders_.emplace(order.order_id, OrderLocation{Side::sell, price, it});
        }
        ++resting_order_count_;
        result.remaining_quantity = remaining_quantity;
    }

    return result;
}

CancelResult OrderBook::cancel(const CancelOrder& command) {
    auto it = resting_orders_.find(command.order_id);
    if (it == resting_orders_.end()) {
        return {.order_id = command.order_id,
                .cancelled_quantity = std::nullopt,
                .rejection = CancelRejectReason::order_not_found};
    }

    const OrderLocation loc = it->second;
    const Quantity cancelled_quantity = loc.it->remaining_quantity;
    const std::uint64_t cancelled_units = cancelled_quantity.units();

    if (loc.side == Side::buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            level_it->second.total_units -= cancelled_units;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            level_it->second.total_units -= cancelled_units;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    resting_orders_.erase(it);
    --resting_order_count_;

    return {.order_id = command.order_id,
            .cancelled_quantity = cancelled_quantity,
            .rejection = std::nullopt};
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
    return resting_order_count_;
}

}  // namespace low_latency_exchange

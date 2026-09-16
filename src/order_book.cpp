#include "low_latency_exchange/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace low_latency_exchange {

void OrderBook::match_against_book(OrderId incoming_order_id,
                                    Side side,
                                    OrderType type,
                                    std::optional<Price> limit_price,
                                    std::uint64_t& remaining_units,
                                    std::vector<Execution>& executions) {
    if (side == Side::buy) {
        while (remaining_units > 0 && !asks_.empty() &&
               (type == OrderType::market || asks_.begin()->first <= *limit_price)) {
            auto level = asks_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second.orders;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                executions.push_back(
                    {resting_order.order_id, incoming_order_id, execution_price, executed_quantity});
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
               (type == OrderType::market || bids_.begin()->first >= *limit_price)) {
            auto level = bids_.begin();
            const Price execution_price = level->first;
            auto& orders = level->second.orders;

            while (remaining_units > 0 && !orders.empty()) {
                auto& resting_order = orders.front();
                const std::uint64_t executed_units =
                    std::min(remaining_units, resting_order.remaining_quantity.units());
                const Quantity executed_quantity = *Quantity::from_units(executed_units);
                executions.push_back(
                    {resting_order.order_id, incoming_order_id, execution_price, executed_quantity});
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
}

void OrderBook::insert_resting_order(OrderId order_id,
                                      SequenceNumber sequence,
                                      Side side,
                                      Price price,
                                      Quantity quantity) {
    const std::uint64_t units = quantity.units();
    RestingOrder resting_order{order_id, sequence, quantity};
    if (side == Side::buy) {
        auto& level = bids_[price];
        level.total_units += units;
        level.orders.push_back(resting_order);
        auto it = std::prev(level.orders.end());
        resting_orders_.emplace(order_id, OrderLocation{Side::buy, price, it});
    } else {
        auto& level = asks_[price];
        level.total_units += units;
        level.orders.push_back(resting_order);
        auto it = std::prev(level.orders.end());
        resting_orders_.emplace(order_id, OrderLocation{Side::sell, price, it});
    }
    ++resting_order_count_;
}

Quantity OrderBook::remove_resting_order(const OrderLocation& loc, OrderId order_id) {
    const Quantity cancelled_quantity = loc.it->remaining_quantity;
    const std::uint64_t units = cancelled_quantity.units();

    if (loc.side == Side::buy) {
        auto level_it = bids_.find(loc.price);
        if (level_it != bids_.end()) {
            level_it->second.total_units -= units;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(loc.price);
        if (level_it != asks_.end()) {
            level_it->second.total_units -= units;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    resting_orders_.erase(order_id);
    --resting_order_count_;
    return cancelled_quantity;
}

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

    match_against_book(order.order_id, order.side, order.type, order.limit_price, remaining_units, result.executions);

    if (order.type == OrderType::limit && remaining_units > 0) {
        const Quantity remaining_quantity = *Quantity::from_units(remaining_units);
        insert_resting_order(order.order_id, order.sequence, order.side, *order.limit_price, remaining_quantity);
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
    const Quantity cancelled_quantity = remove_resting_order(loc, command.order_id);

    return {.order_id = command.order_id,
            .cancelled_quantity = cancelled_quantity,
            .rejection = std::nullopt};
}

ReplaceResult OrderBook::replace(const ReplaceOrder& command) {
    auto it = resting_orders_.find(command.order_id);
    if (it == resting_orders_.end()) {
        return {.order_id = command.order_id,
                .executions = {},
                .remaining_quantity = std::nullopt,
                .rejection = ReplaceRejectReason::order_not_found};
    }

    const OrderLocation loc = it->second;
    const Side side = loc.side;
    const Price old_price = loc.price;
    const Quantity current_quantity = loc.it->remaining_quantity;
    const std::uint64_t current_units = current_quantity.units();
    const std::uint64_t new_units = command.new_quantity.units();

    // Priority preservation rule:
    // If price is unchanged and quantity is reduced, retain priority in place.
    if (command.new_price == old_price && new_units <= current_units) {
        const std::uint64_t unit_diff = current_units - new_units;
        if (side == Side::buy) {
            bids_[old_price].total_units -= unit_diff;
        } else {
            asks_[old_price].total_units -= unit_diff;
        }
        loc.it->remaining_quantity = command.new_quantity;
        return {.order_id = command.order_id,
                .executions = {},
                .remaining_quantity = command.new_quantity,
                .rejection = std::nullopt};
    }

    // Otherwise (price changed or quantity increased):
    // Time priority is lost. Remove from current position.
    remove_resting_order(loc, command.order_id);

    // Match replacement against contra book
    std::vector<Execution> executions;
    std::uint64_t remaining_units = new_units;
    match_against_book(command.order_id, side, OrderType::limit, command.new_price, remaining_units, executions);

    std::optional<Quantity> remaining_quantity;
    if (remaining_units > 0) {
        remaining_quantity = *Quantity::from_units(remaining_units);
        insert_resting_order(command.order_id, command.sequence, side, command.new_price, *remaining_quantity);
    }

    return {.order_id = command.order_id,
            .executions = std::move(executions),
            .remaining_quantity = remaining_quantity,
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

std::optional<LevelQuote> OrderBook::top_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *bids_.begin();
    return LevelQuote{price, *Quantity::from_units(level.total_units), level.orders.size()};
}

std::optional<LevelQuote> OrderBook::top_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    const auto& [price, level] = *asks_.begin();
    return LevelQuote{price, *Quantity::from_units(level.total_units), level.orders.size()};
}

std::vector<LevelQuote> OrderBook::bid_depth(std::size_t max_depth) const {
    std::vector<LevelQuote> levels;
    levels.reserve(std::min(max_depth, bids_.size()));
    for (const auto& [price, level] : bids_) {
        if (levels.size() == max_depth) {
            break;
        }
        levels.push_back({price, *Quantity::from_units(level.total_units), level.orders.size()});
    }
    return levels;
}

std::vector<LevelQuote> OrderBook::ask_depth(std::size_t max_depth) const {
    std::vector<LevelQuote> levels;
    levels.reserve(std::min(max_depth, asks_.size()));
    for (const auto& [price, level] : asks_) {
        if (levels.size() == max_depth) {
            break;
        }
        levels.push_back({price, *Quantity::from_units(level.total_units), level.orders.size()});
    }
    return levels;
}

BookDepth OrderBook::depth(std::size_t max_depth) const {
    return BookDepth{
        .bids = bid_depth(max_depth),
        .asks = ask_depth(max_depth),
    };
}

std::size_t OrderBook::resting_order_count() const noexcept {
    return resting_order_count_;
}

bool OrderBook::validate_invariants() const noexcept {
    if (!bids_.empty() && !asks_.empty()) {
        if (bids_.begin()->first >= asks_.begin()->first) {
            return false;
        }
    }

    std::size_t total_orders = 0;

    for (const auto& [price, level] : bids_) {
        if (level.orders.empty() || level.total_units == 0) {
            return false;
        }
        std::uint64_t counted_units = 0;
        for (const auto& order : level.orders) {
            counted_units += order.remaining_quantity.units();
            const auto it = resting_orders_.find(order.order_id);
            if (it == resting_orders_.end()) {
                return false;
            }
            if (it->second.side != Side::buy || it->second.price != price) {
                return false;
            }
            if (it->second.it->order_id != order.order_id) {
                return false;
            }
        }
        if (counted_units != level.total_units) {
            return false;
        }
        total_orders += level.orders.size();
    }

    for (const auto& [price, level] : asks_) {
        if (level.orders.empty() || level.total_units == 0) {
            return false;
        }
        std::uint64_t counted_units = 0;
        for (const auto& order : level.orders) {
            counted_units += order.remaining_quantity.units();
            const auto it = resting_orders_.find(order.order_id);
            if (it == resting_orders_.end()) {
                return false;
            }
            if (it->second.side != Side::sell || it->second.price != price) {
                return false;
            }
            if (it->second.it->order_id != order.order_id) {
                return false;
            }
        }
        if (counted_units != level.total_units) {
            return false;
        }
        total_orders += level.orders.size();
    }

    if (total_orders != resting_order_count_ || total_orders != resting_orders_.size()) {
        return false;
    }

    return true;
}

bool OrderBook::contains(OrderId order_id) const noexcept {
    return resting_orders_.contains(order_id);
}

}  // namespace low_latency_exchange

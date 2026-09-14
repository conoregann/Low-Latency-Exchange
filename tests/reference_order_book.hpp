#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/types.hpp"

namespace low_latency_exchange::test {

/// @brief An intentionally simple, naive, independent reference order book used
/// as an oracle in differential testing. Prioritizes algorithmic obviousness
/// over execution speed.
class ReferenceOrderBook final {
  public:
    SubmitResult submit(const NewOrder& order) {
        if (validate(order).has_value()) {
            return {.executions = {},
                    .rejection = OrderRejectReason::invalid_order,
                    .remaining_quantity = std::nullopt};
        }

        const bool duplicate = std::ranges::any_of(
            seen_order_ids_, [&](const OrderId& id) { return id == order.order_id; });
        if (duplicate) {
            return {.executions = {},
                    .rejection = OrderRejectReason::duplicate_order_id,
                    .remaining_quantity = std::nullopt};
        }

        seen_order_ids_.push_back(order.order_id);
        std::uint64_t remaining_units = order.quantity.units();
        std::vector<Execution> executions;

        match(order.order_id, order.side, order.type, order.limit_price, remaining_units, executions);

        std::optional<Quantity> remaining_quantity;
        if (order.type == OrderType::limit && remaining_units > 0) {
            remaining_quantity = *Quantity::from_units(remaining_units);
            resting_orders_.push_back(RestingRefOrder{
                .order_id = order.order_id,
                .sequence = order.sequence,
                .side = order.side,
                .price = *order.limit_price,
                .remaining_quantity = *remaining_quantity,
            });
        }

        return {.executions = std::move(executions),
                .rejection = std::nullopt,
                .remaining_quantity = remaining_quantity};
    }

    CancelResult cancel(const CancelOrder& command) {
        auto it = std::ranges::find_if(resting_orders_, [&](const RestingRefOrder& o) {
            return o.order_id == command.order_id;
        });

        if (it == resting_orders_.end()) {
            return {.order_id = command.order_id,
                    .cancelled_quantity = std::nullopt,
                    .rejection = CancelRejectReason::order_not_found};
        }

        const Quantity cancelled_quantity = it->remaining_quantity;
        resting_orders_.erase(it);

        return {.order_id = command.order_id,
                .cancelled_quantity = cancelled_quantity,
                .rejection = std::nullopt};
    }

    ReplaceResult replace(const ReplaceOrder& command) {
        auto it = std::ranges::find_if(resting_orders_, [&](const RestingRefOrder& o) {
            return o.order_id == command.order_id;
        });

        if (it == resting_orders_.end()) {
            return {.order_id = command.order_id,
                    .executions = {},
                    .remaining_quantity = std::nullopt,
                    .rejection = ReplaceRejectReason::order_not_found};
        }

        const Side side = it->side;
        const Price old_price = it->price;
        const Quantity current_quantity = it->remaining_quantity;

        // In-place reduction preserves time priority
        if (command.new_price == old_price && command.new_quantity.units() <= current_quantity.units()) {
            it->remaining_quantity = command.new_quantity;
            return {.order_id = command.order_id,
                    .executions = {},
                    .remaining_quantity = command.new_quantity,
                    .rejection = std::nullopt};
        }

        // Otherwise time priority is forfeited
        resting_orders_.erase(it);

        std::uint64_t remaining_units = command.new_quantity.units();
        std::vector<Execution> executions;
        match(command.order_id, side, OrderType::limit, command.new_price, remaining_units, executions);

        std::optional<Quantity> remaining_quantity;
        if (remaining_units > 0) {
            remaining_quantity = *Quantity::from_units(remaining_units);
            resting_orders_.push_back(RestingRefOrder{
                .order_id = command.order_id,
                .sequence = command.sequence,
                .side = side,
                .price = command.new_price,
                .remaining_quantity = *remaining_quantity,
            });
        }

        return {.order_id = command.order_id,
                .executions = std::move(executions),
                .remaining_quantity = remaining_quantity,
                .rejection = std::nullopt};
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        std::optional<Price> best;
        for (const auto& o : resting_orders_) {
            if (o.side == Side::buy) {
                if (!best.has_value() || o.price > *best) {
                    best = o.price;
                }
            }
        }
        return best;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        std::optional<Price> best;
        for (const auto& o : resting_orders_) {
            if (o.side == Side::sell) {
                if (!best.has_value() || o.price < *best) {
                    best = o.price;
                }
            }
        }
        return best;
    }

    [[nodiscard]] std::optional<LevelQuote> top_bid() const noexcept {
        const auto bb = best_bid();
        if (!bb.has_value()) {
            return std::nullopt;
        }
        return aggregate_level(Side::buy, *bb);
    }

    [[nodiscard]] std::optional<LevelQuote> top_ask() const noexcept {
        const auto ba = best_ask();
        if (!ba.has_value()) {
            return std::nullopt;
        }
        return aggregate_level(Side::sell, *ba);
    }

    [[nodiscard]] std::vector<LevelQuote> bid_depth(std::size_t max_depth = 5) const {
        std::map<Price, std::pair<std::uint64_t, std::size_t>, std::greater<Price>> levels;
        for (const auto& o : resting_orders_) {
            if (o.side == Side::buy) {
                auto& entry = levels[o.price];
                entry.first += o.remaining_quantity.units();
                entry.second += 1;
            }
        }
        std::vector<LevelQuote> quotes;
        quotes.reserve(std::min(max_depth, levels.size()));
        for (const auto& [price, agg] : levels) {
            if (quotes.size() == max_depth) {
                break;
            }
            quotes.push_back(LevelQuote{
                .price = price,
                .quantity = *Quantity::from_units(agg.first),
                .order_count = agg.second,
            });
        }
        return quotes;
    }

    [[nodiscard]] std::vector<LevelQuote> ask_depth(std::size_t max_depth = 5) const {
        std::map<Price, std::pair<std::uint64_t, std::size_t>, std::less<Price>> levels;
        for (const auto& o : resting_orders_) {
            if (o.side == Side::sell) {
                auto& entry = levels[o.price];
                entry.first += o.remaining_quantity.units();
                entry.second += 1;
            }
        }
        std::vector<LevelQuote> quotes;
        quotes.reserve(std::min(max_depth, levels.size()));
        for (const auto& [price, agg] : levels) {
            if (quotes.size() == max_depth) {
                break;
            }
            quotes.push_back(LevelQuote{
                .price = price,
                .quantity = *Quantity::from_units(agg.first),
                .order_count = agg.second,
            });
        }
        return quotes;
    }

    [[nodiscard]] BookDepth depth(std::size_t max_depth = 5) const {
        return BookDepth{
            .bids = bid_depth(max_depth),
            .asks = ask_depth(max_depth),
        };
    }

    [[nodiscard]] std::size_t resting_order_count() const noexcept {
        return resting_orders_.size();
    }

  private:
    struct RestingRefOrder {
        OrderId order_id;
        SequenceNumber sequence;
        Side side;
        Price price;
        Quantity remaining_quantity;
    };

    void match(OrderId incoming_order_id,
               Side side,
               OrderType type,
               std::optional<Price> limit_price,
               std::uint64_t& remaining_units,
               std::vector<Execution>& executions) {
        while (remaining_units > 0) {
            std::optional<std::size_t> best_idx;
            for (std::size_t i = 0; i < resting_orders_.size(); ++i) {
                const auto& candidate = resting_orders_[i];
                if (candidate.side == side) {
                    continue;
                }

                if (side == Side::buy) {
                    if (type == OrderType::limit && candidate.price > *limit_price) {
                        continue;
                    }
                } else {
                    if (type == OrderType::limit && candidate.price < *limit_price) {
                        continue;
                    }
                }

                if (!best_idx.has_value()) {
                    best_idx = i;
                } else {
                    const auto& current_best = resting_orders_[*best_idx];
                    if (side == Side::buy) {
                        if (candidate.price < current_best.price ||
                            (candidate.price == current_best.price && candidate.sequence < current_best.sequence)) {
                            best_idx = i;
                        }
                    } else {
                        if (candidate.price > current_best.price ||
                            (candidate.price == current_best.price && candidate.sequence < current_best.sequence)) {
                            best_idx = i;
                        }
                    }
                }
            }

            if (!best_idx.has_value()) {
                break;
            }

            auto& contra = resting_orders_[*best_idx];
            const std::uint64_t executed_units = std::min(remaining_units, contra.remaining_quantity.units());
            const Quantity executed_quantity = *Quantity::from_units(executed_units);

            executions.push_back(Execution{
                .resting_order_id = contra.order_id,
                .incoming_order_id = incoming_order_id,
                .price = contra.price,
                .quantity = executed_quantity,
            });

            remaining_units -= executed_units;
            const std::uint64_t contra_remaining = contra.remaining_quantity.units() - executed_units;
            if (contra_remaining == 0) {
                resting_orders_.erase(resting_orders_.begin() + static_cast<std::ptrdiff_t>(*best_idx));
            } else {
                contra.remaining_quantity = *Quantity::from_units(contra_remaining);
            }
        }
    }

    [[nodiscard]] LevelQuote aggregate_level(Side side, Price price) const noexcept {
        std::uint64_t total_units = 0;
        std::size_t count = 0;
        for (const auto& o : resting_orders_) {
            if (o.side == side && o.price == price) {
                total_units += o.remaining_quantity.units();
                ++count;
            }
        }
        return LevelQuote{
            .price = price,
            .quantity = *Quantity::from_units(total_units),
            .order_count = count,
        };
    }

    std::vector<RestingRefOrder> resting_orders_;
    std::vector<OrderId> seen_order_ids_;
};

}  // namespace low_latency_exchange::test

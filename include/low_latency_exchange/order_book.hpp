#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "low_latency_exchange/order_command.hpp"

namespace low_latency_exchange {

enum class OrderRejectReason {
    invalid_order,
    market_orders_not_supported,
    duplicate_order_id,
};

struct Execution {
    OrderId resting_order_id;
    OrderId incoming_order_id;
    Price price;
    Quantity quantity;
};

struct SubmitResult {
    std::vector<Execution> executions;
    std::optional<OrderRejectReason> rejection;
    std::optional<Quantity> remaining_quantity;

    [[nodiscard]] bool accepted() const noexcept {
        return !rejection.has_value();
    }
};

class OrderBook final {
  public:
    [[nodiscard]] SubmitResult submit(const NewOrder& order);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::size_t resting_order_count() const noexcept;

  private:
    struct RestingOrder {
        OrderId order_id;
        SequenceNumber sequence;
        Quantity remaining_quantity;
    };

    using BidLevels = std::map<Price, std::deque<RestingOrder>, std::greater<Price>>;
    using AskLevels = std::map<Price, std::deque<RestingOrder>, std::less<Price>>;

    BidLevels bids_;
    AskLevels asks_;
    std::set<OrderId> seen_order_ids_;
};

}  // namespace low_latency_exchange

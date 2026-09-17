#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
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

enum class CancelRejectReason {
    order_not_found,
};

struct CancelResult {
    OrderId order_id;
    std::optional<Quantity> cancelled_quantity;
    std::optional<CancelRejectReason> rejection;

    [[nodiscard]] bool accepted() const noexcept {
        return !rejection.has_value();
    }
};

enum class ReplaceRejectReason {
    order_not_found,
};

struct ReplaceResult {
    OrderId order_id;
    std::vector<Execution> executions;
    std::optional<Quantity> remaining_quantity;
    std::optional<ReplaceRejectReason> rejection;

    [[nodiscard]] bool accepted() const noexcept {
        return !rejection.has_value();
    }
};

struct LevelQuote {
    Price price;
    Quantity quantity;
    std::size_t order_count;

    constexpr auto operator<=>(const LevelQuote&) const noexcept = default;
};

struct BookDepth {
    std::vector<LevelQuote> bids;
    std::vector<LevelQuote> asks;
};

class OrderBook final {
  public:
    [[nodiscard]] SubmitResult submit(const NewOrder& order);
    [[nodiscard]] CancelResult cancel(const CancelOrder& command);
    [[nodiscard]] ReplaceResult replace(const ReplaceOrder& command);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<LevelQuote> top_bid() const noexcept;
    [[nodiscard]] std::optional<LevelQuote> top_ask() const noexcept;
    [[nodiscard]] BookDepth depth(std::size_t max_depth = 5) const;
    [[nodiscard]] std::vector<LevelQuote> bid_depth(std::size_t max_depth = 5) const;
    [[nodiscard]] std::vector<LevelQuote> ask_depth(std::size_t max_depth = 5) const;
    void copy_depth(std::span<std::optional<LevelQuote>> bids,
                    std::span<std::optional<LevelQuote>> asks) const noexcept;
    [[nodiscard]] std::size_t resting_order_count() const noexcept;
    [[nodiscard]] bool contains(OrderId order_id) const noexcept;
    [[nodiscard]] bool validate_invariants() const noexcept;
    [[nodiscard]] std::uint64_t state_digest() const noexcept;

  private:
    struct RestingOrder {
        OrderId order_id;
        SequenceNumber sequence;
        Quantity remaining_quantity;
    };

    struct PriceLevel {
        std::uint64_t total_units = 0;
        std::list<RestingOrder> orders;
    };

    struct OrderLocation {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator it;
    };

    void match_against_book(OrderId incoming_order_id,
                           Side side,
                           OrderType type,
                           std::optional<Price> limit_price,
                           std::uint64_t& remaining_units,
                           std::vector<Execution>& executions);

    void insert_resting_order(OrderId order_id,
                             SequenceNumber sequence,
                             Side side,
                             Price price,
                             Quantity quantity);

    Quantity remove_resting_order(const OrderLocation& loc, OrderId order_id);

    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, OrderLocation> resting_orders_;
    std::unordered_set<OrderId> seen_order_ids_;
    std::size_t resting_order_count_ = 0;
};

}  // namespace low_latency_exchange

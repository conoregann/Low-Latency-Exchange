#pragma once

#include <optional>

#include "low_latency_exchange/types.hpp"

namespace low_latency_exchange {

struct NewOrder {
    SequenceNumber sequence;
    OrderId order_id;
    Side side;
    OrderType type;
    Quantity quantity;
    std::optional<Price> limit_price;
};

enum class NewOrderValidationError {
    limit_order_missing_price,
    market_order_has_price,
};

[[nodiscard]] constexpr std::optional<NewOrderValidationError> validate(const NewOrder& order) noexcept {
    if (order.type == OrderType::limit && !order.limit_price.has_value()) {
        return NewOrderValidationError::limit_order_missing_price;
    }
    if (order.type == OrderType::market && order.limit_price.has_value()) {
        return NewOrderValidationError::market_order_has_price;
    }
    return std::nullopt;
}

struct CancelOrder {
    SequenceNumber sequence;
    OrderId order_id;
};

struct ReplaceOrder {
    SequenceNumber sequence;
    OrderId order_id;
    Quantity new_quantity;
    Price new_price;
};

}  // namespace low_latency_exchange

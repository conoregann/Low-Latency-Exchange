#include "low_latency_exchange/order_command.hpp"
#include "test_util.hpp"

using low_latency_exchange::NewOrder;
using low_latency_exchange::NewOrderValidationError;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderType;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

namespace {

NewOrder make_order(OrderType type, std::optional<Price> price) {
    return NewOrder{
        .sequence = *SequenceNumber::from_value(1),
        .order_id = *OrderId::from_value(10),
        .side = Side::buy,
        .type = type,
        .quantity = *Quantity::from_units(100),
        .limit_price = price,
    };
}

}  // namespace

int main() {
    bool passed = true;
    const auto price = Price::from_ticks(10'000);

    passed &= test_util::check(
        !low_latency_exchange::validate(make_order(OrderType::limit, price)).has_value(),
        "limit order with price is valid");
    passed &= test_util::check(
        low_latency_exchange::validate(make_order(OrderType::limit, std::nullopt)) ==
            NewOrderValidationError::limit_order_missing_price,
        "limit order without price is rejected");
    passed &= test_util::check(
        !low_latency_exchange::validate(make_order(OrderType::market, std::nullopt)).has_value(),
        "market order without price is valid");
    passed &= test_util::check(
        low_latency_exchange::validate(make_order(OrderType::market, price)) ==
            NewOrderValidationError::market_order_has_price,
        "market order carrying a price is rejected");

    return passed ? 0 : 1;
}

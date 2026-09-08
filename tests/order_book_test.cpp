#include <cstdint>
#include <optional>

#include "low_latency_exchange/order_book.hpp"
#include "test_util.hpp"

using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderBook;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderRejectReason;
using low_latency_exchange::OrderType;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

namespace {

NewOrder limit_order(std::uint64_t sequence,
                     std::uint64_t order_id,
                     Side side,
                     std::int64_t price_ticks,
                     std::uint64_t quantity) {
    return NewOrder{
        .sequence = *SequenceNumber::from_value(sequence),
        .order_id = *OrderId::from_value(order_id),
        .side = side,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(quantity),
        .limit_price = *Price::from_ticks(price_ticks),
    };
}

}  // namespace

int main() {
    bool passed = true;

    {
        OrderBook book;
        const auto first_buy = book.submit(limit_order(1, 1, Side::buy, 100, 10));
        const auto second_buy = book.submit(limit_order(2, 2, Side::buy, 100, 20));
        const auto sell = book.submit(limit_order(3, 3, Side::sell, 100, 15));

        passed &= test_util::check(first_buy.accepted(), "resting buy is accepted");
        passed &= test_util::check(second_buy.accepted(), "second resting buy is accepted");
        passed &= test_util::check(sell.executions.size() == 2, "sell matches two FIFO orders");
        passed &= test_util::check(
            sell.executions[0].resting_order_id == *OrderId::from_value(1),
            "first order at a price level matches first");
        passed &= test_util::check(
            sell.executions[0].quantity == *Quantity::from_units(10),
            "first execution consumes the first order");
        passed &= test_util::check(
            sell.executions[1].resting_order_id == *OrderId::from_value(2),
            "second order matches after the first is exhausted");
        passed &= test_util::check(book.best_bid() == Price::from_ticks(100), "residual bid remains");
        passed &= test_util::check(book.resting_order_count() == 1, "only residual order remains");
    }

    {
        OrderBook book;
        const auto buy = book.submit(limit_order(1, 10, Side::buy, 99, 10));
        const auto sell = book.submit(limit_order(2, 11, Side::sell, 100, 10));

        passed &= test_util::check(buy.executions.empty(), "non-crossing buy does not execute");
        passed &= test_util::check(sell.executions.empty(), "non-crossing sell does not execute");
        passed &= test_util::check(book.best_bid() == Price::from_ticks(99), "best bid is retained");
        passed &= test_util::check(book.best_ask() == Price::from_ticks(100), "best ask is retained");
    }

    {
        OrderBook book;
        const auto accepted = book.submit(limit_order(1, 20, Side::buy, 100, 10));
        const auto duplicate = book.submit(limit_order(2, 20, Side::sell, 100, 10));

        passed &= test_util::check(accepted.accepted(), "initial order id is accepted");
        passed &= test_util::check(
            duplicate.rejection == OrderRejectReason::duplicate_order_id,
            "duplicate order id is rejected");
    }

    return passed ? 0 : 1;
}

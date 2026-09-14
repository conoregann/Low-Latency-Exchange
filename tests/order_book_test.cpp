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

NewOrder market_order(std::uint64_t sequence,
                      std::uint64_t order_id,
                      Side side,
                      std::uint64_t quantity) {
    return NewOrder{
        .sequence = *SequenceNumber::from_value(sequence),
        .order_id = *OrderId::from_value(order_id),
        .side = side,
        .type = OrderType::market,
        .quantity = *Quantity::from_units(quantity),
        .limit_price = std::nullopt,
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

    {
        OrderBook book;
        const auto s1 = book.submit(limit_order(1, 1, Side::sell, 100, 10));
        const auto s2 = book.submit(limit_order(2, 2, Side::sell, 102, 15));
        passed &= test_util::check(s1.accepted() && s2.accepted(), "resting sells are accepted");

        const auto market_buy = book.submit(market_order(3, 3, Side::buy, 20));
        passed &= test_util::check(market_buy.accepted(), "market buy is accepted");
        passed &= test_util::check(market_buy.executions.size() == 2, "market buy fills across multiple levels");
        passed &= test_util::check(
            market_buy.executions[0].price == *Price::from_ticks(100) &&
                market_buy.executions[0].quantity == *Quantity::from_units(10),
            "first execution consumes best ask");
        passed &= test_util::check(
            market_buy.executions[1].price == *Price::from_ticks(102) &&
                market_buy.executions[1].quantity == *Quantity::from_units(10),
            "second execution partially fills next level");
        passed &= test_util::check(!market_buy.remaining_quantity.has_value(), "market buy does not rest remainder");
        passed &= test_util::check(book.best_ask() == Price::from_ticks(102), "residual ask remains at 102");
        passed &= test_util::check(book.resting_order_count() == 1, "one resting order remains in book");
    }

    {
        OrderBook book;
        const auto s1 = book.submit(limit_order(1, 1, Side::sell, 100, 10));
        passed &= test_util::check(s1.accepted(), "resting sell is accepted");

        // Market buy for 25 units when only 10 are available
        const auto partial_market_buy = book.submit(market_order(2, 2, Side::buy, 25));
        passed &= test_util::check(partial_market_buy.accepted(), "insufficient contra liquidity market order accepted");
        passed &= test_util::check(partial_market_buy.executions.size() == 1, "fills available liquidity");
        passed &= test_util::check(partial_market_buy.executions[0].quantity == *Quantity::from_units(10),
                                   "consumes entire book");
        passed &= test_util::check(!partial_market_buy.remaining_quantity.has_value(),
                                   "unfilled market order remainder is not rested");
        passed &= test_util::check(!book.best_ask().has_value(), "ask book is exhausted");
        passed &= test_util::check(!book.best_bid().has_value(), "no bid is posted from market order");
        passed &= test_util::check(book.resting_order_count() == 0, "book is empty");
    }

    {
        OrderBook book;
        // Market order against empty book
        const auto empty_market = book.submit(market_order(1, 1, Side::sell, 50));
        passed &= test_util::check(empty_market.accepted(), "market order on empty book is accepted");
        passed &= test_util::check(empty_market.executions.empty(), "no executions on empty book");
        passed &= test_util::check(!empty_market.remaining_quantity.has_value(), "no remainder rested");
        passed &= test_util::check(book.resting_order_count() == 0, "book remains empty");
    }

    {
        OrderBook book;
        const auto b1 = book.submit(limit_order(1, 1, Side::buy, 100, 10));
        const auto b2 = book.submit(limit_order(2, 2, Side::buy, 100, 20));
        const auto b3 = book.submit(limit_order(3, 3, Side::buy, 99, 30));
        passed &= test_util::check(book.resting_order_count() == 3, "three orders resting");

        // Cancel the middle order at price 100
        const auto cancel_res = book.cancel({
            .sequence = *SequenceNumber::from_value(4),
            .order_id = *OrderId::from_value(2),
        });
        passed &= test_util::check(cancel_res.accepted(), "cancel order 2 is accepted");
        passed &= test_util::check(cancel_res.cancelled_quantity == *Quantity::from_units(20),
                                   "cancelled quantity matches resting units");
        passed &= test_util::check(book.resting_order_count() == 2, "resting count reduced to 2");
        passed &= test_util::check(book.best_bid() == Price::from_ticks(100), "best bid remains 100");

        // Sell against remaining order 1 at 100
        const auto sell = book.submit(limit_order(5, 5, Side::sell, 100, 15));
        passed &= test_util::check(sell.executions.size() == 1, "matches only order 1");
        passed &= test_util::check(sell.executions[0].resting_order_id == *OrderId::from_value(1),
                                   "order 1 executed");
        passed &= test_util::check(sell.executions[0].quantity == *Quantity::from_units(10),
                                   "order 1 full quantity executed");
        passed &= test_util::check(book.best_bid() == Price::from_ticks(99), "best bid moves to 99");
    }

    {
        OrderBook book;
        const auto s1 = book.submit(limit_order(1, 10, Side::sell, 105, 50));
        passed &= test_util::check(s1.accepted(), "resting sell accepted");

        // Partial fill of order 10
        const auto b1 = book.submit(limit_order(2, 11, Side::buy, 105, 20));
        passed &= test_util::check(b1.executions.size() == 1, "partial fill occurs");

        // Cancel partially filled order
        const auto cancel_res = book.cancel({
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(10),
        });
        passed &= test_util::check(cancel_res.accepted(), "cancel partially filled order accepted");
        passed &= test_util::check(cancel_res.cancelled_quantity == *Quantity::from_units(30),
                                   "cancelled quantity is residual quantity");
        passed &= test_util::check(book.resting_order_count() == 0, "no orders resting");
        passed &= test_util::check(!book.best_ask().has_value(), "ask book is now empty");

        // Cancel already cancelled / nonexistent order
        const auto cancel_again = book.cancel({
            .sequence = *SequenceNumber::from_value(4),
            .order_id = *OrderId::from_value(10),
        });
        passed &= test_util::check(!cancel_again.accepted(), "cancel unknown order rejected");
        passed &= test_util::check(cancel_again.rejection == low_latency_exchange::CancelRejectReason::order_not_found,
                                   "reject reason is order_not_found");
    }

    return passed ? 0 : 1;
}

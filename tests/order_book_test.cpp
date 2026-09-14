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

    {
        // Replace: quantity reduction retains time priority
        OrderBook book;
        const auto b1 = book.submit(limit_order(1, 1, Side::buy, 100, 20));
        const auto b2 = book.submit(limit_order(2, 2, Side::buy, 100, 10));
        passed &= test_util::check(b1.accepted() && b2.accepted(), "both buys accepted");

        const auto replace_res = book.replace({
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(1),
            .new_quantity = *Quantity::from_units(10),
            .new_price = *Price::from_ticks(100),
        });
        passed &= test_util::check(replace_res.accepted(), "replace quantity decrease accepted");
        passed &= test_util::check(replace_res.remaining_quantity == *Quantity::from_units(10),
                                   "new remaining quantity is 10");

        // Incoming sell for 15 units should consume all 10 of order 1, and 5 of order 2
        const auto sell = book.submit(limit_order(4, 3, Side::sell, 100, 15));
        passed &= test_util::check(sell.executions.size() == 2, "matches two orders");
        passed &= test_util::check(sell.executions[0].resting_order_id == *OrderId::from_value(1),
                                   "order 1 retained priority ahead of order 2");
        passed &= test_util::check(sell.executions[0].quantity == *Quantity::from_units(10),
                                   "order 1 filled for reduced quantity");
        passed &= test_util::check(sell.executions[1].resting_order_id == *OrderId::from_value(2),
                                   "order 2 partially filled");
    }

    {
        // Replace: quantity increase loses time priority
        OrderBook book;
        const auto b1 = book.submit(limit_order(1, 1, Side::buy, 100, 10));
        const auto b2 = book.submit(limit_order(2, 2, Side::buy, 100, 10));
        passed &= test_util::check(b1.accepted() && b2.accepted(), "both buys accepted");

        const auto replace_res = book.replace({
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(1),
            .new_quantity = *Quantity::from_units(30),
            .new_price = *Price::from_ticks(100),
        });
        passed &= test_util::check(replace_res.accepted(), "replace quantity increase accepted");

        // Incoming sell for 15 units should now match order 2 first (since order 1 lost priority)
        const auto sell = book.submit(limit_order(4, 3, Side::sell, 100, 15));
        passed &= test_util::check(sell.executions.size() == 2, "matches two orders");
        passed &= test_util::check(sell.executions[0].resting_order_id == *OrderId::from_value(2),
                                   "order 2 matched first because order 1 lost priority");
        passed &= test_util::check(sell.executions[0].quantity == *Quantity::from_units(10),
                                   "order 2 full fill");
        passed &= test_util::check(sell.executions[1].resting_order_id == *OrderId::from_value(1),
                                   "order 1 matched second");
        passed &= test_util::check(sell.executions[1].quantity == *Quantity::from_units(5),
                                   "order 1 partial fill");
    }

    {
        // Replace: price change crossing spread triggers execution
        OrderBook book;
        const auto s1 = book.submit(limit_order(1, 10, Side::sell, 102, 10));
        const auto b1 = book.submit(limit_order(2, 20, Side::buy, 100, 15));
        passed &= test_util::check(s1.accepted() && b1.accepted(), "both orders accepted");

        // Replace buy order 20 with price 102 (crosses ask at 102)
        const auto replace_res = book.replace({
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(20),
            .new_quantity = *Quantity::from_units(15),
            .new_price = *Price::from_ticks(102),
        });
        passed &= test_util::check(replace_res.accepted(), "crossing replace accepted");
        passed &= test_util::check(replace_res.executions.size() == 1, "crossing replace executes immediately");
        passed &= test_util::check(replace_res.executions[0].price == *Price::from_ticks(102),
                                   "execution price is resting ask 102");
        passed &= test_util::check(replace_res.executions[0].quantity == *Quantity::from_units(10),
                                   "executed against resting sell");
        passed &= test_util::check(replace_res.remaining_quantity == *Quantity::from_units(5),
                                   "remainder rests at 102");
        passed &= test_util::check(book.best_bid() == Price::from_ticks(102), "best bid is now 102");
        passed &= test_util::check(!book.best_ask().has_value(), "ask book is exhausted");
        passed &= test_util::check(book.resting_order_count() == 1, "only residual buy rests");
    }

    {
        // Replace: unknown order
        OrderBook book;
        const auto replace_res = book.replace({
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(999),
            .new_quantity = *Quantity::from_units(10),
            .new_price = *Price::from_ticks(100),
        });
        passed &= test_util::check(!replace_res.accepted(), "replace unknown order rejected");
        passed &= test_util::check(
            replace_res.rejection == low_latency_exchange::ReplaceRejectReason::order_not_found,
            "rejection reason is order_not_found");
    }

    {
        // Level-2 Depth & Top of Book Quotes
        OrderBook book;
        passed &= test_util::check(!book.top_bid().has_value(), "top bid is empty initially");
        passed &= test_util::check(!book.top_ask().has_value(), "top ask is empty initially");
        passed &= test_util::check(book.depth(5).bids.empty() && book.depth(5).asks.empty(),
                                   "depth is empty initially");

        // Add 3 bid levels: 100 (2 orders, total qty 30), 99 (1 order, qty 15), 98 (1 order, qty 25)
        const auto b1 = book.submit(limit_order(1, 1, Side::buy, 100, 10));
        const auto b2 = book.submit(limit_order(2, 2, Side::buy, 100, 20));
        const auto b3 = book.submit(limit_order(3, 3, Side::buy, 99, 15));
        const auto b4 = book.submit(limit_order(4, 4, Side::buy, 98, 25));
        passed &= test_util::check(b1.accepted() && b2.accepted() && b3.accepted() && b4.accepted(),
                                   "all bids accepted");

        // Add 2 ask levels: 102 (1 order, qty 40), 103 (2 orders, total qty 50)
        const auto a1 = book.submit(limit_order(5, 5, Side::sell, 102, 40));
        const auto a2 = book.submit(limit_order(6, 6, Side::sell, 103, 30));
        const auto a3 = book.submit(limit_order(7, 7, Side::sell, 103, 20));
        passed &= test_util::check(a1.accepted() && a2.accepted() && a3.accepted(),
                                   "all asks accepted");

        // Test top_bid and top_ask
        const auto tb = book.top_bid();
        passed &= test_util::check(tb.has_value(), "top bid exists");
        passed &= test_util::check(tb->price == *Price::from_ticks(100) &&
                                       tb->quantity == *Quantity::from_units(30) &&
                                       tb->order_count == 2,
                                   "top bid aggregates price, quantity, and order count");

        const auto ta = book.top_ask();
        passed &= test_util::check(ta.has_value(), "top ask exists");
        passed &= test_util::check(ta->price == *Price::from_ticks(102) &&
                                       ta->quantity == *Quantity::from_units(40) &&
                                       ta->order_count == 1,
                                   "top ask aggregates correctly");

        // Test depth with limit
        const auto depth_2 = book.depth(2);
        passed &= test_util::check(depth_2.bids.size() == 2, "bids depth limited to 2");
        passed &= test_util::check(depth_2.bids[0].price == *Price::from_ticks(100) &&
                                       depth_2.bids[0].quantity == *Quantity::from_units(30),
                                   "bid level 1 correct");
        passed &= test_util::check(depth_2.bids[1].price == *Price::from_ticks(99) &&
                                       depth_2.bids[1].quantity == *Quantity::from_units(15),
                                   "bid level 2 correct");

        passed &= test_util::check(depth_2.asks.size() == 2, "asks depth has 2 levels");
        passed &= test_util::check(depth_2.asks[0].price == *Price::from_ticks(102) &&
                                       depth_2.asks[0].quantity == *Quantity::from_units(40),
                                   "ask level 1 correct");
        passed &= test_util::check(depth_2.asks[1].price == *Price::from_ticks(103) &&
                                       depth_2.asks[1].quantity == *Quantity::from_units(50) &&
                                       depth_2.asks[1].order_count == 2,
                                   "ask level 2 aggregates multiple orders");

        // Cancel order 1 at price 100
        const auto cancel_res = book.cancel({
            .sequence = *SequenceNumber::from_value(8),
            .order_id = *OrderId::from_value(1),
        });
        passed &= test_util::check(cancel_res.accepted(), "cancel order 1 accepted");

        const auto updated_tb = book.top_bid();
        passed &= test_util::check(updated_tb->price == *Price::from_ticks(100) &&
                                       updated_tb->quantity == *Quantity::from_units(20) &&
                                       updated_tb->order_count == 1,
                                   "top bid reflects cancellation immediately");
    }

    return passed ? 0 : 1;
}

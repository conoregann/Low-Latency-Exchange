#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/types.hpp"
#include "test_util.hpp"

using low_latency_exchange::CancelOrder;
using low_latency_exchange::CancelRejectReason;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderBook;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderRejectReason;
using low_latency_exchange::OrderType;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::ReplaceRejectReason;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

namespace {

bool test_boundary_values() {
    bool passed = true;
    OrderBook book;

    const auto min_price = *Price::from_ticks(1);
    const auto max_price = *Price::from_ticks(std::numeric_limits<std::int64_t>::max());
    const auto min_qty = *Quantity::from_units(1);
    const auto max_qty = *Quantity::from_units(1'000'000'000'000ULL);

    const auto min_oid = *OrderId::from_value(1);
    const auto max_oid = *OrderId::from_value(std::numeric_limits<std::uint64_t>::max());

    const auto seq1 = *SequenceNumber::from_value(1);
    const auto seq2 = *SequenceNumber::from_value(std::numeric_limits<std::uint64_t>::max());

    // Submit min price bid
    const auto r1 = book.submit(NewOrder{
        .sequence = seq1,
        .order_id = min_oid,
        .side = Side::buy,
        .type = OrderType::limit,
        .quantity = min_qty,
        .limit_price = min_price,
    });
    passed &= test_util::check(r1.accepted(), "min price/qty limit buy accepted");
    passed &= test_util::check(book.best_bid() == min_price, "best bid is min price");

    // Submit max price ask
    const auto r2 = book.submit(NewOrder{
        .sequence = seq2,
        .order_id = max_oid,
        .side = Side::sell,
        .type = OrderType::limit,
        .quantity = max_qty,
        .limit_price = max_price,
    });
    passed &= test_util::check(r2.accepted(), "max price/qty limit sell accepted");
    passed &= test_util::check(book.best_ask() == max_price, "best ask is max price");
    passed &= test_util::check(book.validate_invariants(), "invariants hold with boundary values");

    return passed;
}

bool test_hostile_invalid_commands() {
    bool passed = true;
    OrderBook book;

    const auto seq = *SequenceNumber::from_value(1);
    const auto oid = *OrderId::from_value(10);
    const auto price = *Price::from_ticks(100);
    const auto qty = *Quantity::from_units(10);

    // Limit order without price (should be rejected)
    const NewOrder invalid_limit{
        .sequence = seq,
        .order_id = oid,
        .side = Side::buy,
        .type = OrderType::limit,
        .quantity = qty,
        .limit_price = std::nullopt,
    };
    const auto r1 = book.submit(invalid_limit);
    passed &= test_util::check(!r1.accepted() && r1.rejection == OrderRejectReason::invalid_order,
                               "limit order without price is rejected as invalid");
    passed &= test_util::check(book.resting_order_count() == 0, "no order placed");

    // Market order with price (should be rejected)
    const NewOrder invalid_market{
        .sequence = seq,
        .order_id = oid,
        .side = Side::buy,
        .type = OrderType::market,
        .quantity = qty,
        .limit_price = price,
    };
    const auto r2 = book.submit(invalid_market);
    passed &= test_util::check(!r2.accepted() && r2.rejection == OrderRejectReason::invalid_order,
                               "market order with price is rejected as invalid");

    // Valid order accepted
    const NewOrder valid{
        .sequence = seq,
        .order_id = oid,
        .side = Side::buy,
        .type = OrderType::limit,
        .quantity = qty,
        .limit_price = price,
    };
    const auto r3 = book.submit(valid);
    passed &= test_util::check(r3.accepted(), "valid order accepted");

    // Re-submission of exact same order ID
    const auto r4 = book.submit(valid);
    passed &= test_util::check(!r4.accepted() && r4.rejection == OrderRejectReason::duplicate_order_id,
                               "duplicate order id rejected");

    // Double cancel of same order
    const auto c1 = book.cancel(CancelOrder{.sequence = *SequenceNumber::from_value(2), .order_id = oid});
    passed &= test_util::check(c1.accepted(), "first cancel accepted");

    const auto c2 = book.cancel(CancelOrder{.sequence = *SequenceNumber::from_value(3), .order_id = oid});
    passed &= test_util::check(!c2.accepted() && c2.rejection == CancelRejectReason::order_not_found,
                               "second cancel rejected with order_not_found");

    // Replace of already cancelled order
    const auto rep = book.replace(ReplaceOrder{
        .sequence = *SequenceNumber::from_value(4),
        .order_id = oid,
        .new_quantity = qty,
        .new_price = price,
    });
    passed &= test_util::check(!rep.accepted() && rep.rejection == ReplaceRejectReason::order_not_found,
                               "replacing cancelled order rejected with order_not_found");

    passed &= test_util::check(book.validate_invariants(), "invariants hold after hostile inputs");
    return passed;
}

bool test_high_depth_queue_churn() {
    bool passed = true;
    OrderBook book;

    const auto price = *Price::from_ticks(100);
    const auto qty = *Quantity::from_units(5);
    constexpr std::size_t N = 1'000;

    // Insert 1000 orders at the exact same price level
    for (std::size_t i = 1; i <= N; ++i) {
        const auto r = book.submit(NewOrder{
            .sequence = *SequenceNumber::from_value(i),
            .order_id = *OrderId::from_value(i),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = qty,
            .limit_price = price,
        });
        passed &= test_util::check(r.accepted(), "mass order accepted");
    }

    passed &= test_util::check(book.resting_order_count() == N, "1000 orders resting");
    const auto top = book.top_bid();
    passed &= test_util::check(top.has_value() && top->order_count == N && top->quantity.units() == N * 5,
                               "depth accurately aggregates 1000 orders at single level");

    // Cancel every third order (testing arbitrary list node erasure)
    for (std::size_t i = 3; i <= N; i += 3) {
        const auto c = book.cancel(CancelOrder{
            .sequence = *SequenceNumber::from_value(N + i),
            .order_id = *OrderId::from_value(i),
        });
        passed &= test_util::check(c.accepted(), "middle order cancel accepted");
    }

    passed &= test_util::check(book.validate_invariants(), "invariants hold after 333 middle cancels");

    // Execute an incoming sell that partially fills the remaining queue
    const auto sell = book.submit(NewOrder{
        .sequence = *SequenceNumber::from_value(2 * N),
        .order_id = *OrderId::from_value(999'999),
        .side = Side::sell,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(500),
        .limit_price = price,
    });
    passed &= test_util::check(sell.accepted() && sell.executions.size() == 100,
                               "sell matches exactly 100 remaining FIFO orders");
    passed &= test_util::check(book.validate_invariants(), "invariants hold after mass matching");

    return passed;
}

bool test_multi_level_sweep() {
    bool passed = true;
    OrderBook book;

    // Place 50 ask levels with 10 units each
    for (std::int64_t p = 101; p <= 150; ++p) {
        const auto r = book.submit(NewOrder{
            .sequence = *SequenceNumber::from_value(static_cast<std::uint64_t>(p)),
            .order_id = *OrderId::from_value(static_cast<std::uint64_t>(p)),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = *Price::from_ticks(p),
        });
        passed &= test_util::check(r.accepted(), "ask level accepted");
    }

    passed &= test_util::check(book.resting_order_count() == 50, "50 ask levels created");

    // Buy order for 355 units @ 140 (should execute through levels 101..135 fully [350 units] + 5 units @ 136)
    const auto sweep_buy = book.submit(NewOrder{
        .sequence = *SequenceNumber::from_value(1'000),
        .order_id = *OrderId::from_value(1'000),
        .side = Side::buy,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(355),
        .limit_price = *Price::from_ticks(140),
    });

    passed &= test_util::check(sweep_buy.accepted(), "sweep buy accepted");
    passed &= test_util::check(sweep_buy.executions.size() == 36, "executed across 36 price levels");
    passed &= test_util::check(!sweep_buy.remaining_quantity.has_value(), "sweep buy completely filled");
    passed &= test_util::check(book.best_ask() == Price::from_ticks(136), "best ask is now 136");
    const auto top = book.top_ask();
    passed &= test_util::check(top->quantity == *Quantity::from_units(5), "remaining quantity @ 136 is 5 units");
    passed &= test_util::check(book.validate_invariants(), "invariants hold after multi-level sweep");

    return passed;
}

}  // namespace

int main() {
    std::cout << "[Fuzz / Hostile Input Tests] Running suite...\n";

    bool passed = true;
    passed &= test_util::check(test_boundary_values(), "boundary value testing");
    passed &= test_util::check(test_hostile_invalid_commands(), "hostile invalid command testing");
    passed &= test_util::check(test_high_depth_queue_churn(), "high-depth queue churn testing");
    passed &= test_util::check(test_multi_level_sweep(), "multi-level aggressive sweep testing");

    if (passed) {
        std::cout << "[Fuzz / Hostile Input Tests] ALL PASSED.\n";
        return 0;
    }
    std::cerr << "[Fuzz / Hostile Input Tests] FAILED.\n";
    return 1;
}

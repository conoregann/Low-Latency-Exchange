#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/types.hpp"
#include "reference_order_book.hpp"
#include "test_util.hpp"

using low_latency_exchange::CancelOrder;
using low_latency_exchange::CancelResult;
using low_latency_exchange::Execution;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderBook;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderRejectReason;
using low_latency_exchange::OrderType;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::ReplaceResult;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;
using low_latency_exchange::SubmitResult;
using low_latency_exchange::test::ReferenceOrderBook;

namespace {

bool compare_executions(const std::vector<Execution>& actual,
                        const std::vector<Execution>& expected,
                        std::uint32_t step) {
    if (actual.size() != expected.size()) {
        std::cerr << "Step " << step << ": Execution count mismatch! Actual: "
                  << actual.size() << ", Expected: " << expected.size() << '\n';
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].resting_order_id != expected[i].resting_order_id ||
            actual[i].incoming_order_id != expected[i].incoming_order_id ||
            actual[i].price != expected[i].price ||
            actual[i].quantity != expected[i].quantity) {
            std::cerr << "Step " << step << ": Execution[" << i << "] mismatch!\n"
                      << "  Actual: resting=" << actual[i].resting_order_id.value()
                      << " incoming=" << actual[i].incoming_order_id.value()
                      << " price=" << actual[i].price.ticks()
                      << " qty=" << actual[i].quantity.units() << '\n'
                      << "  Expected: resting=" << expected[i].resting_order_id.value()
                      << " incoming=" << expected[i].incoming_order_id.value()
                      << " price=" << expected[i].price.ticks()
                      << " qty=" << expected[i].quantity.units() << '\n';
            return false;
        }
    }
    return true;
}

bool compare_submit_results(const SubmitResult& actual,
                            const SubmitResult& expected,
                            std::uint32_t step) {
    if (actual.accepted() != expected.accepted()) {
        std::cerr << "Step " << step << ": Submit acceptance mismatch! Actual: "
                  << actual.accepted() << ", Expected: " << expected.accepted() << '\n';
        return false;
    }
    if (actual.rejection != expected.rejection) {
        std::cerr << "Step " << step << ": Submit rejection mismatch!\n";
        return false;
    }
    if (actual.remaining_quantity != expected.remaining_quantity) {
        std::cerr << "Step " << step << ": Submit remaining_quantity mismatch!\n";
        return false;
    }
    return compare_executions(actual.executions, expected.executions, step);
}

bool compare_cancel_results(const CancelResult& actual,
                            const CancelResult& expected,
                            std::uint32_t step) {
    if (actual.accepted() != expected.accepted()) {
        std::cerr << "Step " << step << ": Cancel acceptance mismatch!\n";
        return false;
    }
    if (actual.rejection != expected.rejection) {
        std::cerr << "Step " << step << ": Cancel rejection mismatch!\n";
        return false;
    }
    if (actual.cancelled_quantity != expected.cancelled_quantity) {
        std::cerr << "Step " << step << ": Cancel cancelled_quantity mismatch!\n";
        return false;
    }
    return true;
}

bool compare_replace_results(const ReplaceResult& actual,
                             const ReplaceResult& expected,
                             std::uint32_t step) {
    if (actual.accepted() != expected.accepted()) {
        std::cerr << "Step " << step << ": Replace acceptance mismatch!\n";
        return false;
    }
    if (actual.rejection != expected.rejection) {
        std::cerr << "Step " << step << ": Replace rejection mismatch!\n";
        return false;
    }
    if (actual.remaining_quantity != expected.remaining_quantity) {
        std::cerr << "Step " << step << ": Replace remaining_quantity mismatch!\n";
        return false;
    }
    return compare_executions(actual.executions, expected.executions, step);
}

bool compare_book_states(const OrderBook& actual,
                         const ReferenceOrderBook& expected,
                         std::uint32_t step) {
    if (actual.best_bid() != expected.best_bid()) {
        std::cerr << "Step " << step << ": best_bid mismatch!\n";
        return false;
    }
    if (actual.best_ask() != expected.best_ask()) {
        std::cerr << "Step " << step << ": best_ask mismatch!\n";
        return false;
    }
    if (actual.top_bid() != expected.top_bid()) {
        std::cerr << "Step " << step << ": top_bid mismatch!\n";
        return false;
    }
    if (actual.top_ask() != expected.top_ask()) {
        std::cerr << "Step " << step << ": top_ask mismatch!\n";
        return false;
    }
    if (actual.resting_order_count() != expected.resting_order_count()) {
        std::cerr << "Step " << step << ": resting_order_count mismatch! Actual: "
                  << actual.resting_order_count() << ", Expected: " << expected.resting_order_count() << '\n';
        return false;
    }

    const auto act_depth = actual.depth(5);
    const auto exp_depth = expected.depth(5);
    if (act_depth.bids != exp_depth.bids) {
        std::cerr << "Step " << step << ": depth.bids mismatch!\n";
        return false;
    }
    if (act_depth.asks != exp_depth.asks) {
        std::cerr << "Step " << step << ": depth.asks mismatch!\n";
        return false;
    }

    return true;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [--seed=<uint64>] [--iterations=<uint32>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::uint64_t seed = 0xD1FF'E471ULL;
    std::uint32_t iterations = 15'000;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg.starts_with("--seed=")) {
            seed = std::stoull(std::string(arg.substr(7)));
        } else if (arg.starts_with("--iterations=")) {
            iterations = static_cast<std::uint32_t>(std::stoul(std::string(arg.substr(13))));
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::cout << "[Differential Test] Comparing OrderBook against ReferenceOrderBook with seed "
              << seed << " (" << iterations << " iterations)...\n";
    std::mt19937_64 rng(seed);

    OrderBook book;
    ReferenceOrderBook ref_book;

    std::vector<OrderId> active_order_ids;
    std::uint64_t next_order_id = 1;
    std::uint64_t sequence = 1;

    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<std::int64_t> price_dist(95, 105);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, 30);

    for (std::uint32_t step = 1; step <= iterations; ++step) {
        const int action = action_dist(rng);

        if (action < 50) {
            // Submit limit order
            const Side side = side_dist(rng) == 0 ? Side::buy : Side::sell;
            const auto price = *Price::from_ticks(price_dist(rng));
            const auto qty = *Quantity::from_units(qty_dist(rng));
            const auto oid = *OrderId::from_value(next_order_id++);
            const auto seq = *SequenceNumber::from_value(sequence++);

            const NewOrder order{
                .sequence = seq,
                .order_id = oid,
                .side = side,
                .type = OrderType::limit,
                .quantity = qty,
                .limit_price = price,
            };

            const auto act_res = book.submit(order);
            const auto exp_res = ref_book.submit(order);

            if (!compare_submit_results(act_res, exp_res, step)) {
                return 1;
            }

            if (act_res.remaining_quantity.has_value()) {
                active_order_ids.push_back(oid);
            }
            // Remove filled resting orders
            for (const auto& ex : act_res.executions) {
                std::erase(active_order_ids, ex.resting_order_id);
            }
        } else if (action < 65) {
            // Submit market order
            const Side side = side_dist(rng) == 0 ? Side::buy : Side::sell;
            const auto qty = *Quantity::from_units(qty_dist(rng));
            const auto oid = *OrderId::from_value(next_order_id++);
            const auto seq = *SequenceNumber::from_value(sequence++);

            const NewOrder order{
                .sequence = seq,
                .order_id = oid,
                .side = side,
                .type = OrderType::market,
                .quantity = qty,
                .limit_price = std::nullopt,
            };

            const auto act_res = book.submit(order);
            const auto exp_res = ref_book.submit(order);

            if (!compare_submit_results(act_res, exp_res, step)) {
                return 1;
            }

            for (const auto& ex : act_res.executions) {
                std::erase(active_order_ids, ex.resting_order_id);
            }
        } else if (action < 82) {
            // Cancel order
            const auto seq = *SequenceNumber::from_value(sequence++);
            OrderId cancel_id = *OrderId::from_value(999'999);
            if (!active_order_ids.empty() && action_dist(rng) < 80) {
                std::uniform_int_distribution<std::size_t> idx_dist(0, active_order_ids.size() - 1);
                cancel_id = active_order_ids[idx_dist(rng)];
            }

            const CancelOrder cmd{.sequence = seq, .order_id = cancel_id};
            const auto act_res = book.cancel(cmd);
            const auto exp_res = ref_book.cancel(cmd);

            if (!compare_cancel_results(act_res, exp_res, step)) {
                return 1;
            }

            if (act_res.accepted()) {
                std::erase(active_order_ids, cancel_id);
            }
        } else {
            // Replace order
            const auto seq = *SequenceNumber::from_value(sequence++);
            OrderId replace_id = *OrderId::from_value(999'999);
            if (!active_order_ids.empty() && action_dist(rng) < 80) {
                std::uniform_int_distribution<std::size_t> idx_dist(0, active_order_ids.size() - 1);
                replace_id = active_order_ids[idx_dist(rng)];
            }

            const auto new_price = *Price::from_ticks(price_dist(rng));
            const auto new_qty = *Quantity::from_units(qty_dist(rng));

            const ReplaceOrder cmd{
                .sequence = seq,
                .order_id = replace_id,
                .new_quantity = new_qty,
                .new_price = new_price,
            };

            const auto act_res = book.replace(cmd);
            const auto exp_res = ref_book.replace(cmd);

            if (!compare_replace_results(act_res, exp_res, step)) {
                return 1;
            }

            if (act_res.accepted()) {
                std::erase(active_order_ids, replace_id);
                for (const auto& ex : act_res.executions) {
                    std::erase(active_order_ids, ex.resting_order_id);
                }
                if (act_res.remaining_quantity.has_value()) {
                    active_order_ids.push_back(replace_id);
                }
            }
        }

        // Compare book states
        if (!compare_book_states(book, ref_book, step)) {
            return 1;
        }
    }

    std::cout << "[Differential Test] PASSED: 100% parity verified between OrderBook and ReferenceOrderBook over "
              << iterations << " commands.\n";
    return 0;
}

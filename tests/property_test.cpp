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
#include "test_util.hpp"

using low_latency_exchange::CancelOrder;
using low_latency_exchange::Execution;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderBook;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderRejectReason;
using low_latency_exchange::OrderType;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

namespace {

struct ActiveOrderRecord {
    OrderId order_id;
    Side side;
    Price price;
    Quantity remaining_quantity;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [--seed=<uint64>] [--iterations=<uint32>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::uint64_t seed = 0x5EEDULL;
    std::uint32_t iterations = 20'000;

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

    std::cout << "[Property Test] Running " << iterations << " iterations with deterministic seed: " << seed << '\n';
    std::mt19937_64 rng(seed);

    OrderBook book;

    // Track active orders known to be resting
    std::vector<ActiveOrderRecord> active_orders;

    // Conservation tracking
    std::uint64_t buy_submitted_units = 0;
    std::uint64_t sell_submitted_units = 0;
    std::uint64_t buy_executed_units = 0;
    std::uint64_t sell_executed_units = 0;
    std::uint64_t buy_cancelled_units = 0;
    std::uint64_t sell_cancelled_units = 0;

    std::uint64_t next_order_id = 1;
    std::uint64_t sequence = 1;

    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<std::int64_t> price_dist(90, 110);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, 50);

    for (std::uint32_t step = 1; step <= iterations; ++step) {
        const int action = action_dist(rng);

        if (action < 50) {
            // 50% chance: Submit limit order
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

            const auto result = book.submit(order);
            if (!result.accepted()) {
                std::cerr << "Step " << step << ": valid limit order rejected\n";
                return 1;
            }

            if (side == Side::buy) {
                buy_submitted_units += qty.units();
            } else {
                sell_submitted_units += qty.units();
            }

            for (const auto& ex : result.executions) {
                buy_executed_units += ex.quantity.units();
                sell_executed_units += ex.quantity.units();

                // Update active orders for resting order
                auto it = std::ranges::find_if(active_orders, [&](const ActiveOrderRecord& r) {
                    return r.order_id == ex.resting_order_id;
                });
                if (it != active_orders.end()) {
                    if (it->remaining_quantity.units() == ex.quantity.units()) {
                        active_orders.erase(it);
                    } else {
                        it->remaining_quantity = *Quantity::from_units(
                            it->remaining_quantity.units() - ex.quantity.units());
                    }
                }
            }

            if (result.remaining_quantity.has_value()) {
                active_orders.push_back({
                    .order_id = oid,
                    .side = side,
                    .price = price,
                    .remaining_quantity = *result.remaining_quantity,
                });
            }
        } else if (action < 65) {
            // 15% chance: Submit market order
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

            const auto result = book.submit(order);
            if (!result.accepted()) {
                std::cerr << "Step " << step << ": valid market order rejected\n";
                return 1;
            }

            for (const auto& ex : result.executions) {
                if (side == Side::buy) {
                    buy_submitted_units += ex.quantity.units();
                } else {
                    sell_submitted_units += ex.quantity.units();
                }
                buy_executed_units += ex.quantity.units();
                sell_executed_units += ex.quantity.units();

                auto it = std::ranges::find_if(active_orders, [&](const ActiveOrderRecord& r) {
                    return r.order_id == ex.resting_order_id;
                });
                if (it != active_orders.end()) {
                    if (it->remaining_quantity.units() == ex.quantity.units()) {
                        active_orders.erase(it);
                    } else {
                        it->remaining_quantity = *Quantity::from_units(
                            it->remaining_quantity.units() - ex.quantity.units());
                    }
                }
            }
        } else if (action < 82) {
            // 17% chance: Cancel order
            const auto seq = *SequenceNumber::from_value(sequence++);
            OrderId cancel_id = *OrderId::from_value(999'999);
            const bool try_real = !active_orders.empty() && (action_dist(rng) < 80);

            if (try_real) {
                std::uniform_int_distribution<std::size_t> idx_dist(0, active_orders.size() - 1);
                cancel_id = active_orders[idx_dist(rng)].order_id;
            }

            const CancelOrder command{.sequence = seq, .order_id = cancel_id};
            const auto result = book.cancel(command);

            if (result.accepted()) {
                auto it = std::ranges::find_if(active_orders, [&](const ActiveOrderRecord& r) {
                    return r.order_id == cancel_id;
                });
                if (it == active_orders.end()) {
                    std::cerr << "Step " << step << ": cancel accepted for unknown order in shadow tracker\n";
                    return 1;
                }
                if (it->side == Side::buy) {
                    buy_cancelled_units += result.cancelled_quantity->units();
                } else {
                    sell_cancelled_units += result.cancelled_quantity->units();
                }
                active_orders.erase(it);
            }
        } else {
            // 18% chance: Replace order
            const auto seq = *SequenceNumber::from_value(sequence++);
            OrderId replace_id = *OrderId::from_value(999'999);
            const bool try_real = !active_orders.empty() && (action_dist(rng) < 80);

            if (try_real) {
                std::uniform_int_distribution<std::size_t> idx_dist(0, active_orders.size() - 1);
                const std::size_t target_idx = idx_dist(rng);
                const auto target_record = active_orders[target_idx];
                replace_id = target_record.order_id;

                const auto new_price = *Price::from_ticks(price_dist(rng));
                const auto new_qty = *Quantity::from_units(qty_dist(rng));

                const ReplaceOrder command{
                    .sequence = seq,
                    .order_id = replace_id,
                    .new_quantity = new_qty,
                    .new_price = new_price,
                };

                const auto result = book.replace(command);
                if (result.accepted()) {
                    const std::uint64_t old_units = target_record.remaining_quantity.units();
                    const std::uint64_t new_units = new_qty.units();

                    if (target_record.side == Side::buy) {
                        buy_submitted_units = (buy_submitted_units + new_units) - old_units;
                    } else {
                        sell_submitted_units = (sell_submitted_units + new_units) - old_units;
                    }

                    // Remove original from active orders
                    auto it = std::ranges::find_if(active_orders, [&](const ActiveOrderRecord& r) {
                        return r.order_id == replace_id;
                    });
                    if (it != active_orders.end()) {
                        active_orders.erase(it);
                    }

                    // Process executions
                    for (const auto& ex : result.executions) {
                        buy_executed_units += ex.quantity.units();
                        sell_executed_units += ex.quantity.units();

                        auto contra_it = std::ranges::find_if(active_orders, [&](const ActiveOrderRecord& r) {
                            return r.order_id == ex.resting_order_id;
                        });
                        if (contra_it != active_orders.end()) {
                            if (contra_it->remaining_quantity.units() == ex.quantity.units()) {
                                active_orders.erase(contra_it);
                            } else {
                                contra_it->remaining_quantity = *Quantity::from_units(
                                    contra_it->remaining_quantity.units() - ex.quantity.units());
                            }
                        }
                    }

                    if (result.remaining_quantity.has_value()) {
                        active_orders.push_back({
                            .order_id = replace_id,
                            .side = target_record.side,
                            .price = new_price,
                            .remaining_quantity = *result.remaining_quantity,
                        });
                    }
                }
            } else {
                const ReplaceOrder command{
                    .sequence = seq,
                    .order_id = replace_id,
                    .new_quantity = *Quantity::from_units(10),
                    .new_price = *Price::from_ticks(100),
                };
                const auto result = book.replace(command);
                if (result.accepted()) {
                    std::cerr << "Step " << step << ": replace accepted for non-existent order\n";
                    return 1;
                }
            }
        }

        // ==========================================
        // INVARIANT CHECKS AT EVERY STEP
        // ==========================================

        // Invariant 1: Structural invariants
        if (!book.validate_invariants()) {
            std::cerr << "Step " << step << ": OrderBook::validate_invariants() failed!\n";
            return 1;
        }

        // Invariant 2: Spread invariant
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (bb.has_value() && ba.has_value()) {
            if (*bb >= *ba) {
                std::cerr << "Step " << step << ": Spread crossed! Best bid: "
                          << bb->ticks() << ", Best ask: " << ba->ticks() << '\n';
                return 1;
            }
        }

        // Invariant 3: Resting count equality
        if (book.resting_order_count() != active_orders.size()) {
            std::cerr << "Step " << step << ": Resting count mismatch! Book: "
                      << book.resting_order_count() << ", Tracker: " << active_orders.size() << '\n';
            return 1;
        }

        // Invariant 4: Volume Conservation
        std::uint64_t actual_buy_resting = 0;
        std::uint64_t actual_sell_resting = 0;
        for (const auto& o : active_orders) {
            if (o.side == Side::buy) {
                actual_buy_resting += o.remaining_quantity.units();
            } else {
                actual_sell_resting += o.remaining_quantity.units();
            }
        }

        const std::uint64_t buy_accounted = buy_executed_units + buy_cancelled_units + actual_buy_resting;
        if (buy_accounted != buy_submitted_units) {
            std::cerr << "Step " << step << ": Buy volume conservation violated!\n"
                      << "  Submitted: " << buy_submitted_units << '\n'
                      << "  Accounted: " << buy_accounted
                      << " (Executed: " << buy_executed_units
                      << ", Cancelled: " << buy_cancelled_units
                      << ", Resting: " << actual_buy_resting << ")\n";
            return 1;
        }

        const std::uint64_t sell_accounted = sell_executed_units + sell_cancelled_units + actual_sell_resting;
        if (sell_accounted != sell_submitted_units) {
            std::cerr << "Step " << step << ": Sell volume conservation violated!\n"
                      << "  Submitted: " << sell_submitted_units << '\n'
                      << "  Accounted: " << sell_accounted
                      << " (Executed: " << sell_executed_units
                      << ", Cancelled: " << sell_cancelled_units
                      << ", Resting: " << actual_sell_resting << ")\n";
            return 1;
        }
    }

    std::cout << "[Property Test] PASSED: All invariants verified across "
              << iterations << " randomized operations.\n";
    std::cout << "  Total Executed Units: " << buy_executed_units << '\n'
              << "  Total Cancelled Units: " << (buy_cancelled_units + sell_cancelled_units) << '\n'
              << "  Final Resting Orders: " << book.resting_order_count() << '\n';

    return 0;
}

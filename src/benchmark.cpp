#include "low_latency_exchange/benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace low_latency_exchange {

namespace {

struct ActiveOrder {
    std::size_t symbol_index = 0;
    OrderId order_id = *OrderId::from_value(1);
};

bool apply(OrderBook& book, const InboundCommand& command, BenchmarkResult& result) {
    if (std::holds_alternative<NewOrder>(command)) {
        const auto submission = book.submit(std::get<NewOrder>(command));
        result.executions += submission.executions.size();
        return submission.accepted();
    }
    if (std::holds_alternative<CancelOrder>(command)) {
        const auto cancellation = book.cancel(std::get<CancelOrder>(command));
        if (cancellation.accepted()) {
            ++result.accepted_cancels;
        } else {
            ++result.rejected_cancels;
        }
        return cancellation.accepted();
    }
    if (std::holds_alternative<ReplaceOrder>(command)) {
        const auto replacement = book.replace(std::get<ReplaceOrder>(command));
        result.executions += replacement.executions.size();
        return replacement.accepted();
    }
    return false;
}

void run_warmup(const std::vector<WorkloadCommand>& workload, std::size_t symbol_count, std::size_t count) {
    std::vector<OrderBook> books(symbol_count);
    BenchmarkResult ignored;
    const std::size_t limit = std::min(count, workload.size());
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& item = workload[index];
        static_cast<void>(apply(books[item.symbol_index], item.command, ignored));
    }
}

}  // namespace

std::vector<WorkloadCommand> make_workload(const WorkloadConfig& config) {
    if (config.command_count == 0 || config.symbol_count == 0 || config.book_levels == 0 ||
        config.cancel_rate_percent > 100 || config.crossing_rate_percent > 100) {
        return {};
    }

    std::mt19937_64 random(config.seed);
    std::vector<WorkloadCommand> workload;
    workload.reserve(config.command_count);
    std::vector<ActiveOrder> active_orders;
    active_orders.reserve(config.command_count);
    std::uint64_t next_order_id = 1;
    std::uint64_t next_sequence = 1;

    for (std::size_t index = 0; index < config.command_count; ++index) {
        const bool create_cancel = !active_orders.empty() && (random() % 100U) < config.cancel_rate_percent;
        if (create_cancel) {
            const std::size_t active_index = static_cast<std::size_t>(random() % active_orders.size());
            const ActiveOrder active = active_orders[active_index];
            workload.push_back(WorkloadCommand{
                .symbol_index = active.symbol_index,
                .command = CancelOrder{
                    .sequence = *SequenceNumber::from_value(next_sequence++),
                    .order_id = active.order_id,
                },
            });
            active_orders[active_index] = active_orders.back();
            active_orders.pop_back();
            continue;
        }

        const std::size_t symbol_index = static_cast<std::size_t>(random() % config.symbol_count);
        const bool buy = (random() & 1U) == 0;
        const bool crossing = (random() % 100U) < config.crossing_rate_percent;
        const std::int64_t level = static_cast<std::int64_t>(random() % config.book_levels);
        const std::int64_t price = crossing ? (buy ? 100'002 + level : 100'000 - level)
                                            : (buy ? 100'000 - level : 100'002 + level);
        const OrderId order_id = *OrderId::from_value(next_order_id++);
        workload.push_back(WorkloadCommand{
            .symbol_index = symbol_index,
            .command = NewOrder{
                .sequence = *SequenceNumber::from_value(next_sequence++),
                .order_id = order_id,
                .side = buy ? Side::buy : Side::sell,
                .type = OrderType::limit,
                .quantity = *Quantity::from_units(1 + (random() % 100U)),
                .limit_price = Price::from_ticks(price),
            },
        });
        active_orders.push_back(ActiveOrder{.symbol_index = symbol_index, .order_id = order_id});
    }
    return workload;
}

BenchmarkResult run_core_benchmark(const std::vector<WorkloadCommand>& workload,
                                   std::size_t symbol_count,
                                   std::size_t warmup_commands) {
    BenchmarkResult result;
    if (workload.empty() || symbol_count == 0) {
        return result;
    }

    run_warmup(workload, symbol_count, warmup_commands);
    std::vector<OrderBook> books(symbol_count);
    LatencyHistogram histogram;
    const auto started_at = std::chrono::steady_clock::now();
    for (const auto& item : workload) {
        const auto command_started_at = std::chrono::steady_clock::now();
        const bool accepted = apply(books[item.symbol_index], item.command, result);
        const auto elapsed = std::chrono::steady_clock::now() - command_started_at;
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        histogram.observe(nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 0);
        if (accepted) {
            ++result.accepted_commands;
        } else {
            ++result.rejected_commands;
        }
    }
    result.elapsed_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count());
    result.commands = workload.size();
    result.throughput_per_second = result.elapsed_nanoseconds == 0
                                       ? 0.0
                                       : static_cast<double>(result.commands) * 1'000'000'000.0 /
                                             static_cast<double>(result.elapsed_nanoseconds);
    result.latency = histogram.snapshot();
    return result;
}

}  // namespace low_latency_exchange

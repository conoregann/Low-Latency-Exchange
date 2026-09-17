#include "low_latency_exchange/benchmark.hpp"
#include "test_util.hpp"

#include <variant>

namespace {

bool commands_equal(const low_latency_exchange::InboundCommand& left,
                    const low_latency_exchange::InboundCommand& right) {
    if (left.index() != right.index()) {
        return false;
    }
    if (std::holds_alternative<low_latency_exchange::NewOrder>(left)) {
        const auto& lhs = std::get<low_latency_exchange::NewOrder>(left);
        const auto& rhs = std::get<low_latency_exchange::NewOrder>(right);
        return lhs.sequence == rhs.sequence && lhs.order_id == rhs.order_id && lhs.side == rhs.side &&
               lhs.type == rhs.type && lhs.quantity == rhs.quantity && lhs.limit_price == rhs.limit_price;
    }
    if (std::holds_alternative<low_latency_exchange::CancelOrder>(left)) {
        const auto& lhs = std::get<low_latency_exchange::CancelOrder>(left);
        const auto& rhs = std::get<low_latency_exchange::CancelOrder>(right);
        return lhs.sequence == rhs.sequence && lhs.order_id == rhs.order_id;
    }
    return true;
}

}  // namespace

int main() {
    const low_latency_exchange::WorkloadConfig config{
        .command_count = 1'000,
        .symbol_count = 3,
        .book_levels = 8,
        .cancel_rate_percent = 25,
        .crossing_rate_percent = 15,
        .seed = 1234,
    };
    const auto first = low_latency_exchange::make_workload(config);
    const auto second = low_latency_exchange::make_workload(config);

    bool passed = true;
    passed &= test_util::check(first.size() == config.command_count, "generate requested command count");
    passed &= test_util::check(first.size() == second.size(), "same seed has same size");
    for (std::size_t index = 0; index < first.size(); ++index) {
        passed &= test_util::check(first[index].symbol_index == second[index].symbol_index &&
                                       commands_equal(first[index].command, second[index].command),
                                   "same seed reproduces command stream");
        if (!passed) {
            break;
        }
    }

    const auto result = low_latency_exchange::run_core_benchmark(first, config.symbol_count, 100);
    passed &= test_util::check(result.commands == config.command_count, "benchmark processes all commands");
    passed &= test_util::check(result.accepted_commands + result.rejected_commands == config.command_count,
                               "benchmark accounts for every command");
    passed &= test_util::check(result.latency.count == config.command_count, "benchmark records every latency");
    passed &= test_util::check(result.throughput_per_second > 0.0, "benchmark reports throughput");
    return passed ? 0 : 1;
}

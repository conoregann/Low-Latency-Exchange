#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "low_latency_exchange/observability.hpp"
#include "low_latency_exchange/tcp_gateway.hpp"

namespace low_latency_exchange {

struct WorkloadConfig {
    std::size_t command_count = 100'000;
    std::size_t symbol_count = 1;
    std::size_t book_levels = 32;
    std::uint32_t cancel_rate_percent = 20;
    std::uint32_t crossing_rate_percent = 10;
    std::uint64_t seed = 0x5EED'0006ULL;
};

struct WorkloadCommand {
    std::size_t symbol_index = 0;
    InboundCommand command = std::monostate{};
};

struct BenchmarkResult {
    std::size_t commands = 0;
    std::uint64_t accepted_commands = 0;
    std::uint64_t rejected_commands = 0;
    std::uint64_t accepted_cancels = 0;
    std::uint64_t rejected_cancels = 0;
    std::uint64_t executions = 0;
    std::uint64_t inbound_bytes = 0;
    std::uint64_t outbound_bytes = 0;
    std::size_t inbound_queue_high_water = 0;
    std::size_t outbound_queue_high_water = 0;
    std::uint64_t elapsed_nanoseconds = 0;
    double throughput_per_second = 0.0;
    LatencyHistogramSnapshot latency{};
};

/** Generates a repeatable mix of posts, crossing orders, and cancels. */
[[nodiscard]] std::vector<WorkloadCommand> make_workload(const WorkloadConfig& config);

/** Benchmarks matching only: no queue, gateway, transport, or disk work. */
[[nodiscard]] BenchmarkResult run_core_benchmark(const std::vector<WorkloadCommand>& workload,
                                                  std::size_t symbol_count,
                                                  std::size_t warmup_commands);

}  // namespace low_latency_exchange

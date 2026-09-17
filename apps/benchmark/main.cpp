#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "low_latency_exchange/benchmark.hpp"
#include "low_latency_exchange/protocol.hpp"
#include "low_latency_exchange/tcp_gateway.hpp"

namespace {

using low_latency_exchange::BenchmarkResult;
using low_latency_exchange::CancelOrder;
using low_latency_exchange::InboundCommand;
using low_latency_exchange::InboundQueue;
using low_latency_exchange::LatencyHistogram;
using low_latency_exchange::MatchingEngineService;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OutboundQueue;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::TcpGateway;
using low_latency_exchange::WorkloadConfig;
using low_latency_exchange::WorkloadCommand;
using low_latency_exchange::protocol::MessageType;

enum class Mode {
    core,
    gateway,
};

struct Options {
    Mode mode = Mode::core;
    WorkloadConfig workload{};
    std::size_t warmup_commands = 10'000;
};

void print_usage(std::ostream& out, std::string_view program) {
    out << "Usage: " << program << " [--mode core|gateway] [options]\n\n"
        << "Options:\n"
        << "  --commands N       Measured commands (default 100000)\n"
        << "  --warmup N         Commands applied before timing core mode (default 10000)\n"
        << "  --symbols N        Independent books in core mode (default 1)\n"
        << "  --levels N         Price levels in synthetic book shape (default 32)\n"
        << "  --cancel-rate N    Percentage of generated cancellations (default 20)\n"
        << "  --cross-rate N     Percentage of crossing new orders (default 10)\n"
        << "  --seed N           Deterministic workload seed\n";
}

bool parse_size(std::string_view text, std::size_t& out) {
    unsigned long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

bool parse_u32(std::string_view text, std::uint32_t& out) {
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    unsigned long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            return false;
        }
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value = argv[++index];
        if (argument == "--mode") {
            if (value == "core") {
                options.mode = Mode::core;
            } else if (value == "gateway") {
                options.mode = Mode::gateway;
            } else {
                return false;
            }
        } else if (argument == "--commands") {
            if (!parse_size(value, options.workload.command_count)) {
                return false;
            }
        } else if (argument == "--warmup") {
            if (!parse_size(value, options.warmup_commands)) {
                return false;
            }
        } else if (argument == "--symbols") {
            if (!parse_size(value, options.workload.symbol_count)) {
                return false;
            }
        } else if (argument == "--levels") {
            if (!parse_size(value, options.workload.book_levels)) {
                return false;
            }
        } else if (argument == "--cancel-rate") {
            if (!parse_u32(value, options.workload.cancel_rate_percent)) {
                return false;
            }
        } else if (argument == "--cross-rate") {
            if (!parse_u32(value, options.workload.crossing_rate_percent)) {
                return false;
            }
        } else if (argument == "--seed") {
            if (!parse_u64(value, options.workload.seed)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return options.workload.command_count > 0 && options.workload.symbol_count > 0 && options.workload.book_levels > 0 &&
           options.workload.cancel_rate_percent <= 100 && options.workload.crossing_rate_percent <= 100;
}

template <std::size_t PayloadSize>
bool encode_frame(MessageType type,
                  const std::array<std::byte, PayloadSize>& payload,
                  std::vector<std::byte>& frame) {
    frame.resize(low_latency_exchange::protocol::kHeaderSize + payload.size());
    return low_latency_exchange::protocol::encode_frame(type, payload, frame, 0);
}

bool encode_command(const InboundCommand& command, std::vector<std::byte>& frame) {
    if (std::holds_alternative<NewOrder>(command)) {
        std::array<std::byte, low_latency_exchange::protocol::layout::kNewOrderSize> payload{};
        return low_latency_exchange::protocol::encode_new_order(std::get<NewOrder>(command), payload) &&
               encode_frame(MessageType::new_order, payload, frame);
    }
    if (std::holds_alternative<CancelOrder>(command)) {
        std::array<std::byte, low_latency_exchange::protocol::layout::kCancelOrderSize> payload{};
        return low_latency_exchange::protocol::encode_cancel_order(std::get<CancelOrder>(command), payload) &&
               encode_frame(MessageType::cancel_order, payload, frame);
    }
    if (std::holds_alternative<ReplaceOrder>(command)) {
        std::array<std::byte, low_latency_exchange::protocol::layout::kReplaceOrderSize> payload{};
        return low_latency_exchange::protocol::encode_replace_order(std::get<ReplaceOrder>(command), payload) &&
               encode_frame(MessageType::replace_order, payload, frame);
    }
    return false;
}

bool read_frame(boost::asio::ip::tcp::socket& socket, low_latency_exchange::protocol::WireHeader& header) {
    std::array<std::byte, low_latency_exchange::protocol::kHeaderSize> header_bytes{};
    boost::system::error_code error;
    boost::asio::read(socket, boost::asio::buffer(header_bytes), error);
    if (error) {
        return false;
    }
    const auto decoded = low_latency_exchange::protocol::decode_header(header_bytes);
    if (!decoded.has_value()) {
        return false;
    }
    header = *decoded;
    std::array<std::byte, low_latency_exchange::protocol::kMaxPayloadLength> payload{};
    if (header.payload_length > 0) {
        boost::asio::read(socket, boost::asio::buffer(payload.data(), header.payload_length), error);
    }
    return !error;
}

bool is_terminal(MessageType type) {
    return type == MessageType::order_accepted || type == MessageType::order_rejected ||
           type == MessageType::cancel_accepted || type == MessageType::cancel_rejected ||
           type == MessageType::replace_accepted || type == MessageType::replace_rejected;
}

void print_result(std::string_view label, const BenchmarkResult& result) {
    const auto& latency = result.latency;
    std::cout << label << "\n"
              << "  commands: " << result.commands << "\n"
              << "  accepted/rejected: " << result.accepted_commands << '/' << result.rejected_commands << "\n"
              << "  accepted/rejected cancels: " << result.accepted_cancels << '/' << result.rejected_cancels << "\n"
              << "  executions: " << result.executions << "\n"
              << "  inbound/outbound bytes: " << result.inbound_bytes << '/' << result.outbound_bytes << "\n"
              << "  inbound/outbound queue high-water: " << result.inbound_queue_high_water << '/'
              << result.outbound_queue_high_water << "\n"
              << "  throughput commands/s: " << std::fixed << std::setprecision(0) << result.throughput_per_second << "\n"
              << "  latency ns min/p50/p99/p99.9/max: " << latency.min_nanoseconds << '/'
              << latency.percentile_upper_bound(0.50) << '/' << latency.percentile_upper_bound(0.99) << '/'
              << latency.percentile_upper_bound(0.999) << '/' << latency.max_nanoseconds << "\n";
}

BenchmarkResult run_gateway_benchmark(const std::vector<WorkloadCommand>& workload) {
    BenchmarkResult result;
    if (workload.empty()) {
        return result;
    }

    boost::asio::io_context server_context;
    InboundQueue inbound;
    OutboundQueue outbound;
    MatchingEngineService engine(inbound, outbound);
    TcpGateway gateway(server_context, 0, inbound, outbound);
    gateway.start();
    std::atomic<bool> running{true};
    std::thread io_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            server_context.poll();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::thread engine_thread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (!engine.process_one()) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    });

    boost::asio::io_context client_context;
    boost::asio::ip::tcp::socket client(client_context);
    boost::system::error_code error;
    client.connect({boost::asio::ip::address_v4::loopback(), gateway.local_port()}, error);
    if (error) {
        running.store(false, std::memory_order_relaxed);
        server_context.stop();
        engine_thread.join();
        io_thread.join();
        gateway.stop();
        return result;
    }

    LatencyHistogram latency;
    const auto started_at = std::chrono::steady_clock::now();
    for (const auto& item : workload) {
        std::vector<std::byte> frame;
        if (!encode_command(item.command, frame)) {
            break;
        }
        const auto command_started_at = std::chrono::steady_clock::now();
        boost::asio::write(client, boost::asio::buffer(frame), error);
        if (error) {
            break;
        }
        low_latency_exchange::protocol::WireHeader header;
        do {
            if (!read_frame(client, header)) {
                error = boost::asio::error::connection_aborted;
                break;
            }
        } while (!is_terminal(header.type));
        if (error) {
            break;
        }
        const auto command_elapsed = std::chrono::steady_clock::now() - command_started_at;
        const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(command_elapsed).count();
        latency.observe(nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 0);
        ++result.commands;
    }
    result.elapsed_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count());
    result.throughput_per_second = result.elapsed_nanoseconds == 0
                                       ? 0.0
                                       : static_cast<double>(result.commands) * 1'000'000'000.0 /
                                             static_cast<double>(result.elapsed_nanoseconds);
    boost::system::error_code ignored;
    client.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    client.close(ignored);
    running.store(false, std::memory_order_relaxed);
    server_context.stop();
    engine_thread.join();
    io_thread.join();
    gateway.stop();

    result.latency = latency.snapshot();
    const auto metrics = engine.metrics();
    result.accepted_commands = metrics.accepted_commands;
    result.rejected_commands = metrics.rejected_commands;
    result.accepted_cancels = metrics.accepted_cancels;
    result.rejected_cancels = metrics.rejected_cancels;
    result.executions = metrics.executions;
    result.inbound_bytes = metrics.inbound_bytes;
    result.outbound_bytes = metrics.outbound_bytes;
    result.inbound_queue_high_water = metrics.inbound_queue_high_water;
    result.outbound_queue_high_water = metrics.outbound_queue_high_water;
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(std::cerr, argc > 0 ? argv[0] : "low_latency_exchange_benchmark");
        return 2;
    }

    const auto workload = low_latency_exchange::make_workload(options.workload);
    if (workload.empty()) {
        std::cerr << "Invalid workload configuration.\n";
        return 2;
    }
    if (options.mode == Mode::gateway && options.workload.symbol_count != 1) {
        std::cerr << "Gateway v1 carries no symbol identifier; --mode gateway requires --symbols 1.\n";
        return 2;
    }

    std::cout << "seed=" << options.workload.seed << " commands=" << options.workload.command_count
              << " symbols=" << options.workload.symbol_count << " levels=" << options.workload.book_levels
              << " cancel_rate=" << options.workload.cancel_rate_percent
              << " crossing_rate=" << options.workload.crossing_rate_percent << "\n";
    if (options.mode == Mode::core) {
        print_result("core matching benchmark",
                     low_latency_exchange::run_core_benchmark(
                         workload, options.workload.symbol_count, options.warmup_commands));
        return 0;
    }
    print_result("loopback TCP gateway benchmark", run_gateway_benchmark(workload));
    return 0;
}

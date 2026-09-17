#include <boost/asio.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "low_latency_exchange/event_log.hpp"
#include "low_latency_exchange/tcp_gateway.hpp"
#include "low_latency_exchange/version.hpp"

namespace {

constexpr std::uint16_t kDefaultGatewayPort = 9'000;

std::optional<std::uint16_t> parse_port(std::string_view text) {
    unsigned int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value == 0 || value > 65'535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

void print_usage(std::ostream& out, std::string_view program) {
    out << "Usage:\n"
        << "  " << program << "\n"
        << "  " << program << " --gateway [port] [event_log_path]\n";
}

int run_gateway(std::uint16_t port, const std::optional<std::filesystem::path>& event_log_path) {
    boost::asio::io_context io_ctx;
    low_latency_exchange::InboundQueue inbound;
    low_latency_exchange::OutboundQueue outbound;
    low_latency_exchange::MarketDataQueue market_data_queue;
    low_latency_exchange::MarketDataFeed market_data_feed(market_data_queue);
    low_latency_exchange::EventCaptureQueue event_capture_queue;
    low_latency_exchange::EventCaptureFeed event_capture_feed(event_capture_queue);
    std::optional<low_latency_exchange::EventLogWriter> event_log_writer;
    if (event_log_path.has_value()) {
        event_log_writer.emplace(*event_log_path);
        if (!event_log_writer->good()) {
            throw std::runtime_error("could not open event log");
        }
    }

    low_latency_exchange::MatchingEngineService engine(
        inbound, outbound, &market_data_feed, event_log_writer.has_value() ? &event_capture_feed : nullptr);
    low_latency_exchange::MarketDataPublisher market_data_publisher(market_data_queue);
    low_latency_exchange::MarketDataBook published_book;
    low_latency_exchange::TcpGateway gateway(io_ctx, port, inbound, outbound);

    boost::asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&gateway, &io_ctx](const boost::system::error_code& ec, int) {
        if (!ec) {
            gateway.stop();
            io_ctx.stop();
        }
    });

    std::jthread engine_thread([&engine](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (engine.process_available() == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });
    std::jthread publisher_thread([&market_data_publisher, &published_book](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (!market_data_publisher.publish_one(published_book)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });
    std::optional<std::jthread> recorder_thread;
    if (event_log_writer.has_value()) {
        recorder_thread.emplace([&event_capture_queue, &event_log_writer](std::stop_token stop_token) {
            while (!stop_token.stop_requested() && event_log_writer->good()) {
                if (!low_latency_exchange::drain_event_capture_one(event_capture_queue, *event_log_writer)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            while (event_log_writer->good() &&
                   low_latency_exchange::drain_event_capture_one(event_capture_queue, *event_log_writer)) {
            }
            if (!event_log_writer->flush()) {
                std::cerr << "Event log flush failed\n";
            }
        });
    }

    gateway.start();
    std::cout << "Low-Latency Exchange " << low_latency_exchange::version() << " gateway listening on 127.0.0.1:"
              << gateway.local_port() << "\nPress Ctrl-C to stop.\n";
    io_ctx.run();

    engine_thread.request_stop();
    engine_thread.join();
    if (recorder_thread.has_value()) {
        recorder_thread->request_stop();
        recorder_thread->join();
    }
    publisher_thread.request_stop();
    publisher_thread.join();
    gateway.stop();
    if (event_log_writer.has_value()) {
        const auto capture_metrics = event_capture_feed.metrics();
        if (capture_metrics.dropped_commands != 0 || !event_log_writer->good()) {
            std::cerr << "Event log is incomplete: dropped=" << capture_metrics.dropped_commands << '\n';
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string_view program = argc > 0 ? argv[0] : "low_latency_exchange";

    if (argc == 1) {
        std::cout << "Low-Latency Exchange " << low_latency_exchange::version() << '\n';
        print_usage(std::cout, program);
        return 0;
    }

    if (std::string_view(argv[1]) == "--help") {
        print_usage(std::cout, program);
        return 0;
    }

    if (std::string_view(argv[1]) != "--gateway" || argc > 4) {
        print_usage(std::cerr, program);
        return 2;
    }

    const auto port = argc >= 3 ? parse_port(argv[2]) : std::optional{kDefaultGatewayPort};
    if (!port.has_value()) {
        std::cerr << "Gateway port must be an integer from 1 through 65535.\n";
        return 2;
    }

    try {
        const auto event_log_path = argc == 4 ? std::optional{std::filesystem::path{argv[3]}}
                                              : std::optional<std::filesystem::path>{};
        return run_gateway(*port, event_log_path);
    } catch (const std::exception& ex) {
        std::cerr << "Unable to start gateway: " << ex.what() << '\n';
        return 1;
    }
}

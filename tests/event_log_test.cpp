#include "low_latency_exchange/event_log.hpp"
#include "low_latency_exchange/tcp_gateway.hpp"
#include "test_util.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

using low_latency_exchange::CancelOrder;
using low_latency_exchange::CapturedCommand;
using low_latency_exchange::EventCaptureFeed;
using low_latency_exchange::EventCaptureQueue;
using low_latency_exchange::EventLogError;
using low_latency_exchange::EventLogWriter;
using low_latency_exchange::InboundMessage;
using low_latency_exchange::InboundQueue;
using low_latency_exchange::MatchingEngineService;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderType;
using low_latency_exchange::OutboundQueue;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

namespace {

NewOrder limit_order(std::uint64_t sequence,
                     std::uint64_t order_id,
                     Side side,
                     std::uint64_t quantity,
                     std::int64_t price) {
    return NewOrder{
        .sequence = *SequenceNumber::from_value(sequence),
        .order_id = *OrderId::from_value(order_id),
        .side = side,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(quantity),
        .limit_price = Price::from_ticks(price),
    };
}

std::filesystem::path log_path() {
    return std::filesystem::temp_directory_path() / "low_latency_exchange_event_log_test.bin";
}

bool write_commands(const std::filesystem::path& path, const std::vector<CapturedCommand>& commands) {
    EventLogWriter writer(path);
    if (!writer.good()) {
        return false;
    }
    for (const auto& command : commands) {
        if (!writer.append(command)) {
            return false;
        }
    }
    return writer.flush();
}

}  // namespace

int main() {
    bool passed = true;
    const std::filesystem::path path = log_path();
    std::error_code cleanup_ec;
    std::filesystem::remove(path, cleanup_ec);

    const std::vector<CapturedCommand> commands{
        limit_order(1, 5'001, Side::buy, 20, 100),
        limit_order(2, 5'002, Side::buy, 10, 99),
        ReplaceOrder{
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(5'001),
            .new_quantity = *Quantity::from_units(15),
            .new_price = *Price::from_ticks(100),
        },
        limit_order(4, 5'003, Side::sell, 10, 100),
        CancelOrder{
            .sequence = *SequenceNumber::from_value(5),
            .order_id = *OrderId::from_value(5'001),
        },
    };

    // Accepted engine commands are captured asynchronously and replay to the same final digest.
    {
        InboundQueue inbound;
        OutboundQueue outbound;
        EventCaptureQueue capture_queue;
        EventCaptureFeed capture_feed(capture_queue);
        MatchingEngineService engine(inbound, outbound, nullptr, &capture_feed);
        EventLogWriter writer(path);
        passed &= test_util::check(writer.good(), "event log writer opened");

        for (const auto& command : commands) {
            passed &= test_util::check(inbound.try_push(InboundMessage{1, command}), "enqueue accepted command");
            passed &= test_util::check(engine.process_available() == 1, "engine processes accepted command");
        }

        while (low_latency_exchange::drain_event_capture_one(capture_queue, writer)) {
        }
        passed &= test_util::check(writer.flush(), "flush accepted command log");
        passed &= test_util::check(capture_feed.metrics().dropped_commands == 0, "no accepted commands dropped");

        const auto replay = low_latency_exchange::replay_event_log(path);
        passed &= test_util::check(replay.succeeded(), "accepted command log replays successfully");
        passed &= test_util::check(replay.replayed_commands == commands.size(), "replay command count matches capture");
        passed &= test_util::check(replay.state_digest == engine.order_book().state_digest(),
                                   "replayed final state digest matches live engine");
    }

    // A checksum mismatch is detected before a corrupted command reaches replay.
    {
        passed &= test_util::check(write_commands(path, commands), "write clean log for checksum test");
        std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
        output.seekp(16 + 5);
        const char corrupted = static_cast<char>(0x7F);
        output.write(&corrupted, 1);
        output.close();

        const auto replay = low_latency_exchange::replay_event_log(path);
        passed &= test_util::check(replay.error == EventLogError::checksum_mismatch,
                                   "checksum corruption fails clearly");
    }

    // A partial final record is detected rather than silently replayed.
    {
        passed &= test_util::check(write_commands(path, commands), "write clean log for truncation test");
        const auto size = std::filesystem::file_size(path);
        std::filesystem::resize_file(path, size - 1);

        const auto replay = low_latency_exchange::replay_event_log(path);
        passed &= test_util::check(replay.error == EventLogError::truncated_payload,
                                   "truncated final payload fails clearly");
    }

    // A slow recorder cannot stall matching; dropped capture work is measured.
    {
        EventCaptureQueue capture_queue;
        EventCaptureFeed capture_feed(capture_queue);
        const auto command = CapturedCommand{CancelOrder{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
        }};
        for (std::size_t index = 0; index <= EventCaptureQueue::capacity(); ++index) {
            capture_feed.capture(command);
        }
        const auto metrics = capture_feed.metrics();
        passed &= test_util::check(capture_queue.full(), "event capture queue is bounded");
        passed &= test_util::check(metrics.dropped_commands == 1, "full capture queue records dropped command");
        passed &= test_util::check(metrics.max_queue_occupancy == EventCaptureQueue::capacity(),
                                   "capture queue high-water mark is recorded");
    }

    std::filesystem::remove(path, cleanup_ec);
    return passed ? 0 : 1;
}

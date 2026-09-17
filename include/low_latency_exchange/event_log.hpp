#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <variant>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/spsc_queue.hpp"

namespace low_latency_exchange {

using CapturedCommand = std::variant<std::monostate, NewOrder, CancelOrder, ReplaceOrder>;

struct EventCaptureMessage {
    CapturedCommand command = std::monostate{};
};

struct EventCaptureMetrics {
    std::uint64_t captured_commands = 0;
    std::uint64_t dropped_commands = 0;
    std::size_t queue_occupancy = 0;
    std::size_t max_queue_occupancy = 0;
};

using EventCaptureQueue = BoundedSPSCQueue<EventCaptureMessage, 4096>;

/** The matching-engine thread is the sole producer of accepted commands. */
class EventCaptureFeed final {
  public:
    explicit EventCaptureFeed(EventCaptureQueue& queue) noexcept : queue_{queue} {}

    void capture(CapturedCommand command) noexcept;
    [[nodiscard]] EventCaptureMetrics metrics() const noexcept;

  private:
    void record_queue_occupancy() noexcept;

    EventCaptureQueue& queue_;
    std::uint64_t captured_commands_ = 0;
    std::uint64_t dropped_commands_ = 0;
    std::size_t max_queue_occupancy_ = 0;
};

enum class EventLogError : std::uint8_t {
    io_error,
    truncated_header,
    malformed_header,
    truncated_payload,
    checksum_mismatch,
    invalid_command,
    replay_command_rejected,
};

[[nodiscard]] const char* event_log_error_name(EventLogError error) noexcept;

/** File-writer edge: it is consumed by a recorder thread, never by matching. */
class EventLogWriter final {
  public:
    explicit EventLogWriter(const std::filesystem::path& path);

    EventLogWriter(const EventLogWriter&) = delete;
    EventLogWriter& operator=(const EventLogWriter&) = delete;
    EventLogWriter(EventLogWriter&&) = default;
    EventLogWriter& operator=(EventLogWriter&&) = default;

    [[nodiscard]] bool append(const CapturedCommand& command);
    [[nodiscard]] bool flush();
    [[nodiscard]] bool good() const noexcept;

  private:
    std::ofstream output_;
};

[[nodiscard]] bool drain_event_capture_one(EventCaptureQueue& queue, EventLogWriter& writer);

struct ReplayResult {
    std::optional<EventLogError> error;
    std::size_t replayed_commands = 0;
    std::uint64_t state_digest = 0;

    [[nodiscard]] bool succeeded() const noexcept {
        return !error.has_value();
    }
};

/** Rebuilds a fresh deterministic order book from a framed, checked log. */
[[nodiscard]] ReplayResult replay_event_log(const std::filesystem::path& path);

}  // namespace low_latency_exchange

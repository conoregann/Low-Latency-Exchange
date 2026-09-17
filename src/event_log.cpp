#include "low_latency_exchange/event_log.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <limits>
#include <span>

#include "low_latency_exchange/protocol.hpp"

namespace low_latency_exchange {

namespace {

inline constexpr std::uint32_t kRecordMagic = 0x474C584C;  // "LXLG" little-endian
inline constexpr std::uint8_t kRecordVersion = 1;
inline constexpr std::size_t kRecordHeaderSize = 16;
inline constexpr std::size_t kRecordPayloadMax = protocol::layout::kNewOrderSize;
inline constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void write_u16_le(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
    out[offset] = static_cast<std::byte>(value & 0xFFU);
    out[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u32_le(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        out[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xFFU);
    }
}

void write_u64_le(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        out[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xFFU);
    }
}

std::uint16_t read_u16_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[offset])) |
           (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[offset + 1])) << 8U);
}

std::uint32_t read_u32_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[offset + byte])) << (byte * 8U);
    }
    return value;
}

std::uint64_t read_u64_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[offset + byte])) << (byte * 8U);
    }
    return value;
}

std::uint64_t checksum(protocol::MessageType type, std::span<const std::byte> payload) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    const auto mix = [&hash](std::uint8_t value) {
        hash ^= value;
        hash *= kFnvPrime;
    };

    mix(kRecordVersion);
    mix(static_cast<std::uint8_t>(type));
    mix(static_cast<std::uint8_t>(payload.size() & 0xFFU));
    mix(static_cast<std::uint8_t>((payload.size() >> 8U) & 0xFFU));
    for (const std::byte byte : payload) {
        mix(std::to_integer<std::uint8_t>(byte));
    }
    return hash;
}

bool encode_command(const CapturedCommand& command,
                    protocol::MessageType& type,
                    std::array<std::byte, kRecordPayloadMax>& payload,
                    std::size_t& payload_size) noexcept {
    if (std::holds_alternative<NewOrder>(command)) {
        type = protocol::MessageType::new_order;
        payload_size = protocol::layout::kNewOrderSize;
        return protocol::encode_new_order(std::get<NewOrder>(command),
                                          std::span<std::byte>(payload.data(), payload_size));
    }
    if (std::holds_alternative<CancelOrder>(command)) {
        type = protocol::MessageType::cancel_order;
        payload_size = protocol::layout::kCancelOrderSize;
        return protocol::encode_cancel_order(std::get<CancelOrder>(command),
                                             std::span<std::byte>(payload.data(), payload_size));
    }
    if (std::holds_alternative<ReplaceOrder>(command)) {
        type = protocol::MessageType::replace_order;
        payload_size = protocol::layout::kReplaceOrderSize;
        return protocol::encode_replace_order(std::get<ReplaceOrder>(command),
                                              std::span<std::byte>(payload.data(), payload_size));
    }
    return false;
}

std::optional<EventLogError> decode_command(protocol::MessageType type,
                                            std::span<const std::byte> payload,
                                            CapturedCommand& command) noexcept {
    if (type == protocol::MessageType::new_order) {
        NewOrder order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(1),
            .limit_price = std::nullopt,
        };
        if (protocol::decode_new_order(payload, order).has_value()) {
            return EventLogError::invalid_command;
        }
        command = order;
        return std::nullopt;
    }
    if (type == protocol::MessageType::cancel_order) {
        CancelOrder order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
        };
        if (protocol::decode_cancel_order(payload, order).has_value()) {
            return EventLogError::invalid_command;
        }
        command = order;
        return std::nullopt;
    }
    if (type == protocol::MessageType::replace_order) {
        ReplaceOrder order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .new_quantity = *Quantity::from_units(1),
            .new_price = *Price::from_ticks(1),
        };
        if (protocol::decode_replace_order(payload, order).has_value()) {
            return EventLogError::invalid_command;
        }
        command = order;
        return std::nullopt;
    }
    return EventLogError::invalid_command;
}

bool apply_command(OrderBook& book, const CapturedCommand& command) noexcept {
    if (std::holds_alternative<NewOrder>(command)) {
        return book.submit(std::get<NewOrder>(command)).accepted();
    }
    if (std::holds_alternative<CancelOrder>(command)) {
        return book.cancel(std::get<CancelOrder>(command)).accepted();
    }
    if (std::holds_alternative<ReplaceOrder>(command)) {
        return book.replace(std::get<ReplaceOrder>(command)).accepted();
    }
    return false;
}

enum class ReadStatus : std::uint8_t {
    record,
    end,
    error,
};

struct ReadResult {
    ReadStatus status = ReadStatus::end;
    std::optional<EventLogError> error;
    CapturedCommand command = std::monostate{};
};

ReadResult read_record(std::ifstream& input) {
    std::array<std::byte, kRecordHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::streamsize header_bytes = input.gcount();
    if (header_bytes == 0 && input.eof()) {
        return {.status = ReadStatus::end};
    }
    if (header_bytes != static_cast<std::streamsize>(header.size())) {
        return {.status = ReadStatus::error, .error = EventLogError::truncated_header};
    }

    const auto header_span = std::span<const std::byte>(header);
    const std::uint32_t magic = read_u32_le(header_span, 0);
    const std::uint8_t version = std::to_integer<std::uint8_t>(header[4]);
    const auto type = static_cast<protocol::MessageType>(std::to_integer<std::uint8_t>(header[5]));
    const std::uint16_t payload_size = read_u16_le(header_span, 6);
    const std::uint64_t expected_checksum = read_u64_le(header_span, 8);
    const auto expected_payload_size = protocol::expected_payload_length(type);
    if (magic != kRecordMagic || version != kRecordVersion || !protocol::is_inbound(type) ||
        !expected_payload_size.has_value() || payload_size != *expected_payload_size || payload_size > kRecordPayloadMax) {
        return {.status = ReadStatus::error, .error = EventLogError::malformed_header};
    }

    std::array<std::byte, kRecordPayloadMax> payload{};
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload_size));
    if (input.gcount() != static_cast<std::streamsize>(payload_size)) {
        return {.status = ReadStatus::error, .error = EventLogError::truncated_payload};
    }

    const auto payload_span = std::span<const std::byte>(payload.data(), payload_size);
    if (checksum(type, payload_span) != expected_checksum) {
        return {.status = ReadStatus::error, .error = EventLogError::checksum_mismatch};
    }

    CapturedCommand command;
    if (const auto error = decode_command(type, payload_span, command); error.has_value()) {
        return {.status = ReadStatus::error, .error = error};
    }
    return {.status = ReadStatus::record, .command = std::move(command)};
}

}  // namespace

void EventCaptureFeed::capture(CapturedCommand command) noexcept {
    if (!queue_.try_push(EventCaptureMessage{.command = std::move(command)})) {
        ++dropped_commands_;
        record_queue_occupancy();
        return;
    }
    ++captured_commands_;
    record_queue_occupancy();
}

EventCaptureMetrics EventCaptureFeed::metrics() const noexcept {
    return EventCaptureMetrics{
        .captured_commands = captured_commands_,
        .dropped_commands = dropped_commands_,
        .queue_occupancy = queue_.size(),
        .max_queue_occupancy = max_queue_occupancy_,
    };
}

void EventCaptureFeed::record_queue_occupancy() noexcept {
    if (queue_.size() > max_queue_occupancy_) {
        max_queue_occupancy_ = queue_.size();
    }
}

const char* event_log_error_name(EventLogError error) noexcept {
    switch (error) {
        case EventLogError::io_error:
            return "io_error";
        case EventLogError::truncated_header:
            return "truncated_header";
        case EventLogError::malformed_header:
            return "malformed_header";
        case EventLogError::truncated_payload:
            return "truncated_payload";
        case EventLogError::checksum_mismatch:
            return "checksum_mismatch";
        case EventLogError::invalid_command:
            return "invalid_command";
        case EventLogError::replay_command_rejected:
            return "replay_command_rejected";
    }
    return "unknown";
}

EventLogWriter::EventLogWriter(const std::filesystem::path& path) : output_(path, std::ios::binary | std::ios::trunc) {}

bool EventLogWriter::append(const CapturedCommand& command) {
    std::array<std::byte, kRecordPayloadMax> payload{};
    protocol::MessageType type = protocol::MessageType::new_order;
    std::size_t payload_size = 0;
    if (!output_.good() || !encode_command(command, type, payload, payload_size)) {
        return false;
    }

    std::array<std::byte, kRecordHeaderSize> header{};
    const auto payload_span = std::span<const std::byte>(payload.data(), payload_size);
    write_u32_le(header, 0, kRecordMagic);
    header[4] = static_cast<std::byte>(kRecordVersion);
    header[5] = static_cast<std::byte>(static_cast<std::uint8_t>(type));
    write_u16_le(header, 6, static_cast<std::uint16_t>(payload_size));
    write_u64_le(header, 8, checksum(type, payload_span));

    output_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    output_.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload_size));
    return output_.good();
}

bool EventLogWriter::flush() {
    output_.flush();
    return output_.good();
}

bool EventLogWriter::good() const noexcept {
    return output_.good();
}

bool drain_event_capture_one(EventCaptureQueue& queue, EventLogWriter& writer) {
    EventCaptureMessage message;
    if (!queue.try_pop(message)) {
        return false;
    }
    return writer.append(message.command);
}

ReplayResult replay_event_log(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {.error = EventLogError::io_error};
    }

    OrderBook book;
    std::size_t replayed_commands = 0;
    while (true) {
        ReadResult record = read_record(input);
        if (record.status == ReadStatus::end) {
            return {.error = std::nullopt, .replayed_commands = replayed_commands, .state_digest = book.state_digest()};
        }
        if (record.status == ReadStatus::error) {
            return {.error = record.error, .replayed_commands = replayed_commands, .state_digest = book.state_digest()};
        }
        if (!apply_command(book, record.command)) {
            return {.error = EventLogError::replay_command_rejected,
                    .replayed_commands = replayed_commands,
                    .state_digest = book.state_digest()};
        }
        ++replayed_commands;
    }
}

}  // namespace low_latency_exchange

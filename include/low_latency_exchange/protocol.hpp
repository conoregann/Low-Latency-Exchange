#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/types.hpp"

namespace low_latency_exchange::protocol {

inline constexpr std::uint16_t kMagic = 0x584C;
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::uint16_t kMaxPayloadLength = 1024;
inline constexpr std::uint16_t kFlagMore = 0x0001;
inline constexpr std::uint16_t kFlagReservedMask = 0xFFFE;

enum class MessageType : std::uint8_t {
    new_order = 1,
    cancel_order = 2,
    replace_order = 3,
    order_accepted = 16,
    order_rejected = 17,
    execution = 18,
    cancel_accepted = 19,
    cancel_rejected = 20,
    replace_accepted = 21,
    replace_rejected = 22,
    protocol_error = 23,
};

enum class ErrorCode : std::uint16_t {
    malformed_frame = 1,
    unsupported_version = 2,
    unknown_message_type = 3,
    payload_length_invalid = 4,
    payload_too_large = 5,
    invalid_field = 6,
    limit_order_missing_price = 7,
    market_order_has_price = 8,
    invalid_order = 100,
    duplicate_order_id = 101,
    order_not_found = 102,
    session_overloaded = 200,
    sequence_out_of_order = 201,
};

struct WireHeader {
    std::uint16_t magic = kMagic;
    std::uint8_t version = kVersion;
    MessageType type = MessageType::new_order;
    std::uint16_t flags = 0;
    std::uint16_t payload_length = 0;
};

namespace layout {

inline constexpr std::size_t kNewOrderSize = 40;
inline constexpr std::size_t kCancelOrderSize = 16;
inline constexpr std::size_t kReplaceOrderSize = 32;
inline constexpr std::size_t kOrderAcceptedSize = 24;
inline constexpr std::size_t kOrderRejectedSize = 24;
inline constexpr std::size_t kExecutionSize = 40;
inline constexpr std::size_t kCancelAcceptedSize = 24;
inline constexpr std::size_t kCancelRejectedSize = 24;
inline constexpr std::size_t kReplaceAcceptedSize = 24;
inline constexpr std::size_t kReplaceRejectedSize = 24;
inline constexpr std::size_t kProtocolErrorSize = 8;

inline constexpr std::size_t kSequenceOffset = 0;
inline constexpr std::size_t kOrderIdOffset = 8;
inline constexpr std::size_t kNewOrderQuantityOffset = 16;
inline constexpr std::size_t kNewOrderPriceOffset = 24;
inline constexpr std::size_t kNewOrderSideOffset = 32;
inline constexpr std::size_t kNewOrderTypeOffset = 33;
inline constexpr std::size_t kNewOrderReservedOffset = 34;
inline constexpr std::size_t kNewOrderReservedSize = 6;

inline constexpr std::size_t kReplaceQuantityOffset = 16;
inline constexpr std::size_t kReplacePriceOffset = 24;

inline constexpr std::size_t kRemainingQuantityOffset = 16;
inline constexpr std::size_t kRejectErrorCodeOffset = 16;
inline constexpr std::size_t kRejectReservedOffset = 18;
inline constexpr std::size_t kRejectReservedSize = 6;

inline constexpr std::size_t kExecutionRestingIdOffset = 8;
inline constexpr std::size_t kExecutionIncomingIdOffset = 16;
inline constexpr std::size_t kExecutionPriceOffset = 24;
inline constexpr std::size_t kExecutionQuantityOffset = 32;

inline constexpr std::size_t kCancelledQuantityOffset = 16;

inline constexpr std::size_t kProtocolErrorCodeOffset = 0;
inline constexpr std::size_t kProtocolErrorTypeOffset = 2;
inline constexpr std::size_t kProtocolErrorReservedOffset = 3;
inline constexpr std::size_t kProtocolErrorReservedSize = 5;

}  // namespace layout

[[nodiscard]] constexpr bool is_inbound(MessageType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);
    return value >= 1 && value <= 15;
}

[[nodiscard]] constexpr bool is_outbound(MessageType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);
    return value >= 16 && value <= 31;
}

[[nodiscard]] constexpr bool flags_reserved_bits_clear(std::uint16_t flags) noexcept {
    return (flags & kFlagReservedMask) == 0;
}

[[nodiscard]] constexpr std::optional<std::uint16_t> expected_payload_length(MessageType type) noexcept {
    switch (type) {
        case MessageType::new_order:
            return static_cast<std::uint16_t>(layout::kNewOrderSize);
        case MessageType::cancel_order:
            return static_cast<std::uint16_t>(layout::kCancelOrderSize);
        case MessageType::replace_order:
            return static_cast<std::uint16_t>(layout::kReplaceOrderSize);
        case MessageType::order_accepted:
            return static_cast<std::uint16_t>(layout::kOrderAcceptedSize);
        case MessageType::order_rejected:
            return static_cast<std::uint16_t>(layout::kOrderRejectedSize);
        case MessageType::execution:
            return static_cast<std::uint16_t>(layout::kExecutionSize);
        case MessageType::cancel_accepted:
            return static_cast<std::uint16_t>(layout::kCancelAcceptedSize);
        case MessageType::cancel_rejected:
            return static_cast<std::uint16_t>(layout::kCancelRejectedSize);
        case MessageType::replace_accepted:
            return static_cast<std::uint16_t>(layout::kReplaceAcceptedSize);
        case MessageType::replace_rejected:
            return static_cast<std::uint16_t>(layout::kReplaceRejectedSize);
        case MessageType::protocol_error:
            return static_cast<std::uint16_t>(layout::kProtocolErrorSize);
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_known_message_type(MessageType type) noexcept {
    return expected_payload_length(type).has_value();
}

[[nodiscard]] std::optional<ErrorCode> validate_header(const WireHeader& header) noexcept;

[[nodiscard]] bool encode_header(const WireHeader& header, std::span<std::byte> out) noexcept;

[[nodiscard]] std::optional<WireHeader> decode_header(std::span<const std::byte> in) noexcept;

// Event structures for wire payloads
struct OrderAcceptedEvent {
    SequenceNumber sequence;
    OrderId order_id;
    std::uint64_t remaining_quantity = 0;
};

struct OrderRejectedEvent {
    SequenceNumber sequence;
    OrderId order_id;
    ErrorCode error_code;
};

struct ExecutionEvent {
    SequenceNumber sequence;
    OrderId resting_order_id;
    OrderId incoming_order_id;
    Price price;
    Quantity quantity;
};

struct CancelAcceptedEvent {
    SequenceNumber sequence;
    OrderId order_id;
    Quantity cancelled_quantity;
};

struct ProtocolErrorEvent {
    ErrorCode error_code;
    std::uint8_t offending_type = 0;
};

// Error code mapping helpers
[[nodiscard]] constexpr ErrorCode to_error_code(OrderRejectReason reason) noexcept {
    switch (reason) {
        case OrderRejectReason::invalid_order:
            return ErrorCode::invalid_order;
        case OrderRejectReason::market_orders_not_supported:
            return ErrorCode::invalid_order;
        case OrderRejectReason::duplicate_order_id:
            return ErrorCode::duplicate_order_id;
    }
    return ErrorCode::invalid_order;
}

[[nodiscard]] constexpr ErrorCode to_error_code(CancelRejectReason) noexcept {
    return ErrorCode::order_not_found;
}

[[nodiscard]] constexpr ErrorCode to_error_code(ReplaceRejectReason) noexcept {
    return ErrorCode::order_not_found;
}

// Inbound command codecs
[[nodiscard]] std::optional<ErrorCode> decode_new_order(std::span<const std::byte> in, NewOrder& out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_cancel_order(std::span<const std::byte> in, CancelOrder& out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_replace_order(std::span<const std::byte> in, ReplaceOrder& out) noexcept;

[[nodiscard]] bool encode_new_order(const NewOrder& order, std::span<std::byte> out) noexcept;
[[nodiscard]] bool encode_cancel_order(const CancelOrder& order, std::span<std::byte> out) noexcept;
[[nodiscard]] bool encode_replace_order(const ReplaceOrder& order, std::span<std::byte> out) noexcept;

// Outbound event codecs
[[nodiscard]] bool encode_order_accepted(const OrderAcceptedEvent& event, std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_order_accepted(std::span<const std::byte> in, OrderAcceptedEvent& out) noexcept;

[[nodiscard]] bool encode_order_rejected(const OrderRejectedEvent& event, std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_order_rejected(std::span<const std::byte> in, OrderRejectedEvent& out) noexcept;

[[nodiscard]] bool encode_execution(const ExecutionEvent& event, std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_execution(std::span<const std::byte> in, ExecutionEvent& out) noexcept;

[[nodiscard]] bool encode_cancel_accepted(const CancelAcceptedEvent& event, std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_cancel_accepted(std::span<const std::byte> in, CancelAcceptedEvent& out) noexcept;

[[nodiscard]] bool encode_protocol_error(const ProtocolErrorEvent& event, std::span<std::byte> out) noexcept;
[[nodiscard]] std::optional<ErrorCode> decode_protocol_error(std::span<const std::byte> in, ProtocolErrorEvent& out) noexcept;

// Full frame encoder helper (writes 8-byte header + payload)
[[nodiscard]] bool encode_frame(MessageType type,
                                std::span<const std::byte> payload,
                                std::span<std::byte> out,
                                std::uint16_t flags = 0) noexcept;

}  // namespace low_latency_exchange::protocol

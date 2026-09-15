#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

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

}  // namespace low_latency_exchange::protocol

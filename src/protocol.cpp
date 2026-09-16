#include "low_latency_exchange/protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace low_latency_exchange::protocol {
namespace {

[[nodiscard]] std::uint16_t read_u16_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    const auto low = std::to_integer<std::uint16_t>(in[offset]);
    const auto high = std::to_integer<std::uint16_t>(in[offset + 1]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

void write_u16_le(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
    out[offset] = static_cast<std::byte>(value & 0xFFU);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFU);
}

}  // namespace

std::optional<ErrorCode> validate_header(const WireHeader& header) noexcept {
    if (header.magic != kMagic) {
        return ErrorCode::malformed_frame;
    }
    if (header.version != kVersion) {
        return ErrorCode::unsupported_version;
    }
    if (header.payload_length > kMaxPayloadLength) {
        return ErrorCode::payload_too_large;
    }
    if (!flags_reserved_bits_clear(header.flags)) {
        return ErrorCode::invalid_field;
    }

    const auto expected = expected_payload_length(header.type);
    if (!expected.has_value()) {
        return ErrorCode::unknown_message_type;
    }
    if (header.payload_length != *expected) {
        return ErrorCode::payload_length_invalid;
    }
    return std::nullopt;
}

bool encode_header(const WireHeader& header, std::span<std::byte> out) noexcept {
    if (out.size() < kHeaderSize) {
        return false;
    }
    write_u16_le(out, 0, header.magic);
    out[2] = static_cast<std::byte>(header.version);
    out[3] = static_cast<std::byte>(static_cast<std::uint8_t>(header.type));
    write_u16_le(out, 4, header.flags);
    write_u16_le(out, 6, header.payload_length);
    return true;
}

std::optional<WireHeader> decode_header(std::span<const std::byte> in) noexcept {
    if (in.size() < kHeaderSize) {
        return std::nullopt;
    }
    WireHeader header;
    header.magic = read_u16_le(in, 0);
    header.version = static_cast<std::uint8_t>(in[2]);
    header.type = static_cast<MessageType>(static_cast<std::uint8_t>(in[3]));
    header.flags = read_u16_le(in, 4);
    header.payload_length = read_u16_le(in, 6);
    return header;
}

namespace {

[[nodiscard]] std::uint64_t read_u64_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    std::uint64_t val = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        val |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[offset + i])) << (i * 8);
    }
    return val;
}

[[nodiscard]] std::int64_t read_i64_le(std::span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::int64_t>(read_u64_le(in, offset));
}

void write_u64_le(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFFU);
    }
}

void write_i64_le(std::span<std::byte> out, std::size_t offset, std::int64_t value) noexcept {
    write_u64_le(out, offset, static_cast<std::uint64_t>(value));
}

}  // namespace

std::optional<ErrorCode> decode_new_order(std::span<const std::byte> in, NewOrder& out) noexcept {
    if (in.size() != layout::kNewOrderSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq_val = read_u64_le(in, layout::kSequenceOffset);
    const auto oid_val = read_u64_le(in, layout::kOrderIdOffset);
    const auto qty_val = read_u64_le(in, layout::kNewOrderQuantityOffset);
    const auto price_val = read_i64_le(in, layout::kNewOrderPriceOffset);
    const auto side_byte = std::to_integer<std::uint8_t>(in[layout::kNewOrderSideOffset]);
    const auto type_byte = std::to_integer<std::uint8_t>(in[layout::kNewOrderTypeOffset]);

    for (std::size_t i = 0; i < layout::kNewOrderReservedSize; ++i) {
        if (in[layout::kNewOrderReservedOffset + i] != std::byte{0}) {
            return ErrorCode::invalid_field;
        }
    }

    if (seq_val == 0 || oid_val == 0 || qty_val == 0 || side_byte > 1 || type_byte > 1 || price_val < 0) {
        return ErrorCode::invalid_field;
    }

    const auto seq = SequenceNumber::from_value(seq_val);
    const auto oid = OrderId::from_value(oid_val);
    const auto qty = Quantity::from_units(qty_val);
    if (!seq || !oid || !qty) {
        return ErrorCode::invalid_field;
    }

    const Side side = (side_byte == 0) ? Side::buy : Side::sell;
    const OrderType type = (type_byte == 0) ? OrderType::limit : OrderType::market;

    std::optional<Price> limit_price;
    if (type == OrderType::limit) {
        if (price_val == 0) {
            return ErrorCode::limit_order_missing_price;
        }
        limit_price = Price::from_ticks(price_val);
        if (!limit_price) {
            return ErrorCode::invalid_field;
        }
    } else {
        if (price_val != 0) {
            return ErrorCode::market_order_has_price;
        }
    }

    out = NewOrder{
        .sequence = *seq,
        .order_id = *oid,
        .side = side,
        .type = type,
        .quantity = *qty,
        .limit_price = limit_price,
    };
    return std::nullopt;
}

std::optional<ErrorCode> decode_cancel_order(std::span<const std::byte> in, CancelOrder& out) noexcept {
    if (in.size() != layout::kCancelOrderSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq_val = read_u64_le(in, layout::kSequenceOffset);
    const auto oid_val = read_u64_le(in, layout::kOrderIdOffset);
    if (seq_val == 0 || oid_val == 0) {
        return ErrorCode::invalid_field;
    }
    const auto seq = SequenceNumber::from_value(seq_val);
    const auto oid = OrderId::from_value(oid_val);
    if (!seq || !oid) {
        return ErrorCode::invalid_field;
    }
    out = CancelOrder{.sequence = *seq, .order_id = *oid};
    return std::nullopt;
}

std::optional<ErrorCode> decode_replace_order(std::span<const std::byte> in, ReplaceOrder& out) noexcept {
    if (in.size() != layout::kReplaceOrderSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq_val = read_u64_le(in, layout::kSequenceOffset);
    const auto oid_val = read_u64_le(in, layout::kOrderIdOffset);
    const auto qty_val = read_u64_le(in, layout::kReplaceQuantityOffset);
    const auto price_val = read_i64_le(in, layout::kReplacePriceOffset);

    if (seq_val == 0 || oid_val == 0 || qty_val == 0 || price_val <= 0) {
        return ErrorCode::invalid_field;
    }
    const auto seq = SequenceNumber::from_value(seq_val);
    const auto oid = OrderId::from_value(oid_val);
    const auto qty = Quantity::from_units(qty_val);
    const auto price = Price::from_ticks(price_val);
    if (!seq || !oid || !qty || !price) {
        return ErrorCode::invalid_field;
    }
    out = ReplaceOrder{
        .sequence = *seq,
        .order_id = *oid,
        .new_quantity = *qty,
        .new_price = *price,
    };
    return std::nullopt;
}

bool encode_new_order(const NewOrder& order, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kNewOrderSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, order.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, order.order_id.value());
    write_u64_le(out, layout::kNewOrderQuantityOffset, order.quantity.units());
    write_i64_le(out, layout::kNewOrderPriceOffset, order.limit_price.has_value() ? order.limit_price->ticks() : 0);
    out[layout::kNewOrderSideOffset] = static_cast<std::byte>(order.side == Side::buy ? 0 : 1);
    out[layout::kNewOrderTypeOffset] = static_cast<std::byte>(order.type == OrderType::limit ? 0 : 1);
    for (std::size_t i = 0; i < layout::kNewOrderReservedSize; ++i) {
        out[layout::kNewOrderReservedOffset + i] = std::byte{0};
    }
    return true;
}

bool encode_cancel_order(const CancelOrder& order, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kCancelOrderSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, order.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, order.order_id.value());
    return true;
}

bool encode_replace_order(const ReplaceOrder& order, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kReplaceOrderSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, order.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, order.order_id.value());
    write_u64_le(out, layout::kReplaceQuantityOffset, order.new_quantity.units());
    write_i64_le(out, layout::kReplacePriceOffset, order.new_price.ticks());
    return true;
}

bool encode_order_accepted(const OrderAcceptedEvent& event, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kOrderAcceptedSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, event.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, event.order_id.value());
    write_u64_le(out, layout::kRemainingQuantityOffset, event.remaining_quantity);
    return true;
}

std::optional<ErrorCode> decode_order_accepted(std::span<const std::byte> in, OrderAcceptedEvent& out) noexcept {
    if (in.size() != layout::kOrderAcceptedSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq = SequenceNumber::from_value(read_u64_le(in, layout::kSequenceOffset));
    const auto oid = OrderId::from_value(read_u64_le(in, layout::kOrderIdOffset));
    if (!seq || !oid) {
        return ErrorCode::invalid_field;
    }
    out.sequence = *seq;
    out.order_id = *oid;
    out.remaining_quantity = read_u64_le(in, layout::kRemainingQuantityOffset);
    return std::nullopt;
}

bool encode_order_rejected(const OrderRejectedEvent& event, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kOrderRejectedSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, event.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, event.order_id.value());
    write_u16_le(out, layout::kRejectErrorCodeOffset, static_cast<std::uint16_t>(event.error_code));
    for (std::size_t i = 0; i < layout::kRejectReservedSize; ++i) {
        out[layout::kRejectReservedOffset + i] = std::byte{0};
    }
    return true;
}

std::optional<ErrorCode> decode_order_rejected(std::span<const std::byte> in, OrderRejectedEvent& out) noexcept {
    if (in.size() != layout::kOrderRejectedSize) {
        return ErrorCode::payload_length_invalid;
    }
    for (std::size_t i = 0; i < layout::kRejectReservedSize; ++i) {
        if (in[layout::kRejectReservedOffset + i] != std::byte{0}) {
            return ErrorCode::invalid_field;
        }
    }
    const auto seq = SequenceNumber::from_value(read_u64_le(in, layout::kSequenceOffset));
    const auto oid_val = read_u64_le(in, layout::kOrderIdOffset);
    if (!seq) {
        return ErrorCode::invalid_field;
    }
    out.sequence = *seq;
    out.order_id = oid_val == 0 ? *OrderId::from_value(1) : *OrderId::from_value(oid_val);
    out.error_code = static_cast<ErrorCode>(read_u16_le(in, layout::kRejectErrorCodeOffset));
    return std::nullopt;
}

bool encode_execution(const ExecutionEvent& event, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kExecutionSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, event.sequence.value());
    write_u64_le(out, layout::kExecutionRestingIdOffset, event.resting_order_id.value());
    write_u64_le(out, layout::kExecutionIncomingIdOffset, event.incoming_order_id.value());
    write_i64_le(out, layout::kExecutionPriceOffset, event.price.ticks());
    write_u64_le(out, layout::kExecutionQuantityOffset, event.quantity.units());
    return true;
}

std::optional<ErrorCode> decode_execution(std::span<const std::byte> in, ExecutionEvent& out) noexcept {
    if (in.size() != layout::kExecutionSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq = SequenceNumber::from_value(read_u64_le(in, layout::kSequenceOffset));
    const auto resting_oid = OrderId::from_value(read_u64_le(in, layout::kExecutionRestingIdOffset));
    const auto incoming_oid = OrderId::from_value(read_u64_le(in, layout::kExecutionIncomingIdOffset));
    const auto price = Price::from_ticks(read_i64_le(in, layout::kExecutionPriceOffset));
    const auto qty = Quantity::from_units(read_u64_le(in, layout::kExecutionQuantityOffset));
    if (!seq || !resting_oid || !incoming_oid || !price || !qty) {
        return ErrorCode::invalid_field;
    }
    out.sequence = *seq;
    out.resting_order_id = *resting_oid;
    out.incoming_order_id = *incoming_oid;
    out.price = *price;
    out.quantity = *qty;
    return std::nullopt;
}

bool encode_cancel_accepted(const CancelAcceptedEvent& event, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kCancelAcceptedSize) {
        return false;
    }
    write_u64_le(out, layout::kSequenceOffset, event.sequence.value());
    write_u64_le(out, layout::kOrderIdOffset, event.order_id.value());
    write_u64_le(out, layout::kCancelledQuantityOffset, event.cancelled_quantity.units());
    return true;
}

std::optional<ErrorCode> decode_cancel_accepted(std::span<const std::byte> in, CancelAcceptedEvent& out) noexcept {
    if (in.size() != layout::kCancelAcceptedSize) {
        return ErrorCode::payload_length_invalid;
    }
    const auto seq = SequenceNumber::from_value(read_u64_le(in, layout::kSequenceOffset));
    const auto oid = OrderId::from_value(read_u64_le(in, layout::kOrderIdOffset));
    const auto qty = Quantity::from_units(read_u64_le(in, layout::kCancelledQuantityOffset));
    if (!seq || !oid || !qty) {
        return ErrorCode::invalid_field;
    }
    out.sequence = *seq;
    out.order_id = *oid;
    out.cancelled_quantity = *qty;
    return std::nullopt;
}

bool encode_protocol_error(const ProtocolErrorEvent& event, std::span<std::byte> out) noexcept {
    if (out.size() < layout::kProtocolErrorSize) {
        return false;
    }
    write_u16_le(out, layout::kProtocolErrorCodeOffset, static_cast<std::uint16_t>(event.error_code));
    out[layout::kProtocolErrorTypeOffset] = static_cast<std::byte>(event.offending_type);
    for (std::size_t i = 0; i < layout::kProtocolErrorReservedSize; ++i) {
        out[layout::kProtocolErrorReservedOffset + i] = std::byte{0};
    }
    return true;
}

std::optional<ErrorCode> decode_protocol_error(std::span<const std::byte> in, ProtocolErrorEvent& out) noexcept {
    if (in.size() != layout::kProtocolErrorSize) {
        return ErrorCode::payload_length_invalid;
    }
    for (std::size_t i = 0; i < layout::kProtocolErrorReservedSize; ++i) {
        if (in[layout::kProtocolErrorReservedOffset + i] != std::byte{0}) {
            return ErrorCode::invalid_field;
        }
    }
    out.error_code = static_cast<ErrorCode>(read_u16_le(in, layout::kProtocolErrorCodeOffset));
    out.offending_type = std::to_integer<std::uint8_t>(in[layout::kProtocolErrorTypeOffset]);
    return std::nullopt;
}

bool encode_frame(MessageType type,
                  std::span<const std::byte> payload,
                  std::span<std::byte> out,
                  std::uint16_t flags) noexcept {
    if (out.size() < kHeaderSize + payload.size()) {
        return false;
    }
    const WireHeader header{
        .magic = kMagic,
        .version = kVersion,
        .type = type,
        .flags = flags,
        .payload_length = static_cast<std::uint16_t>(payload.size()),
    };
    if (!encode_header(header, out.subspan(0, kHeaderSize))) {
        return false;
    }
    if (!payload.empty()) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            out[kHeaderSize + i] = payload[i];
        }
    }
    return true;
}

}  // namespace low_latency_exchange::protocol

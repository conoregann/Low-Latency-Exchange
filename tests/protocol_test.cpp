#include "low_latency_exchange/protocol.hpp"
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>

using low_latency_exchange::protocol::decode_header;
using low_latency_exchange::protocol::encode_header;
using low_latency_exchange::protocol::ErrorCode;
using low_latency_exchange::protocol::expected_payload_length;
using low_latency_exchange::protocol::is_inbound;
using low_latency_exchange::protocol::is_known_message_type;
using low_latency_exchange::protocol::is_outbound;
using low_latency_exchange::protocol::kFlagMore;
using low_latency_exchange::protocol::kHeaderSize;
using low_latency_exchange::protocol::kMagic;
using low_latency_exchange::protocol::kMaxPayloadLength;
using low_latency_exchange::protocol::kVersion;
using low_latency_exchange::protocol::MessageType;
using low_latency_exchange::protocol::validate_header;
using low_latency_exchange::protocol::WireHeader;

namespace {

WireHeader valid_header(MessageType type, std::uint16_t flags = 0) {
    return WireHeader{
        .magic = kMagic,
        .version = kVersion,
        .type = type,
        .flags = flags,
        .payload_length = *expected_payload_length(type),
    };
}

bool headers_equal(const WireHeader& left, const WireHeader& right) {
    return left.magic == right.magic && left.version == right.version && left.type == right.type &&
           left.flags == right.flags && left.payload_length == right.payload_length;
}

}  // namespace

int main() {
    bool passed = true;

    passed &= test_util::check(kHeaderSize == 8, "v1 header is exactly 8 bytes");
    passed &= test_util::check(kMagic == 0x584C, "magic is little-endian LX");
    passed &= test_util::check(kVersion == 1, "wire version is 1");
    passed &= test_util::check(kMaxPayloadLength == 1024, "max payload is 1024 bytes");

    passed &= test_util::check(is_inbound(MessageType::new_order), "new_order is inbound");
    passed &= test_util::check(is_inbound(MessageType::cancel_order), "cancel_order is inbound");
    passed &= test_util::check(is_inbound(MessageType::replace_order), "replace_order is inbound");
    passed &= test_util::check(!is_outbound(MessageType::new_order), "new_order is not outbound");
    passed &= test_util::check(is_outbound(MessageType::execution), "execution is outbound");
    passed &= test_util::check(is_outbound(MessageType::protocol_error), "protocol_error is outbound");
    passed &= test_util::check(!is_inbound(MessageType::execution), "execution is not inbound");
    passed &= test_util::check(!is_known_message_type(static_cast<MessageType>(99)),
                               "unknown type is rejected");

    using low_latency_exchange::protocol::layout::kCancelOrderSize;
    using low_latency_exchange::protocol::layout::kExecutionSize;
    using low_latency_exchange::protocol::layout::kNewOrderSize;
    using low_latency_exchange::protocol::layout::kProtocolErrorSize;
    using low_latency_exchange::protocol::layout::kReplaceOrderSize;

    passed &= test_util::check(expected_payload_length(MessageType::new_order) == kNewOrderSize,
                               "new_order payload is 40 bytes");
    passed &= test_util::check(expected_payload_length(MessageType::cancel_order) == kCancelOrderSize,
                               "cancel_order payload is 16 bytes");
    passed &= test_util::check(expected_payload_length(MessageType::replace_order) == kReplaceOrderSize,
                               "replace_order payload is 32 bytes");
    passed &= test_util::check(expected_payload_length(MessageType::execution) == kExecutionSize,
                               "execution payload is 40 bytes");
    passed &= test_util::check(expected_payload_length(MessageType::protocol_error) == kProtocolErrorSize,
                               "protocol_error payload is 8 bytes");
    passed &= test_util::check(
        expected_payload_length(MessageType::order_accepted) ==
            expected_payload_length(MessageType::replace_accepted),
        "accepted events share a 24-byte remainder layout");
    passed &= test_util::check(
        expected_payload_length(MessageType::order_rejected) ==
            expected_payload_length(MessageType::cancel_rejected),
        "reject events share a 24-byte error layout");

    using low_latency_exchange::protocol::layout::kNewOrderPriceOffset;
    using low_latency_exchange::protocol::layout::kNewOrderQuantityOffset;
    using low_latency_exchange::protocol::layout::kNewOrderReservedOffset;
    using low_latency_exchange::protocol::layout::kNewOrderReservedSize;
    using low_latency_exchange::protocol::layout::kNewOrderSideOffset;
    using low_latency_exchange::protocol::layout::kNewOrderTypeOffset;
    using low_latency_exchange::protocol::layout::kOrderIdOffset;
    using low_latency_exchange::protocol::layout::kSequenceOffset;

    passed &= test_util::check(kSequenceOffset == 0 && kOrderIdOffset == 8, "id fields occupy the first 16 bytes");
    passed &= test_util::check(kNewOrderQuantityOffset == 16 && kNewOrderPriceOffset == 24,
                               "new_order quantity and price are 8-byte aligned");
    passed &= test_util::check(kNewOrderSideOffset == 32 && kNewOrderTypeOffset == 33,
                               "new_order side and type occupy the packed tail");
    passed &= test_util::check(kNewOrderReservedOffset + kNewOrderReservedSize == kNewOrderSize,
                               "new_order reserved bytes fill the payload");

    const std::array kErrorCodes{
        ErrorCode::malformed_frame,
        ErrorCode::unsupported_version,
        ErrorCode::unknown_message_type,
        ErrorCode::payload_length_invalid,
        ErrorCode::payload_too_large,
        ErrorCode::invalid_field,
        ErrorCode::limit_order_missing_price,
        ErrorCode::market_order_has_price,
        ErrorCode::invalid_order,
        ErrorCode::duplicate_order_id,
        ErrorCode::order_not_found,
        ErrorCode::session_overloaded,
        ErrorCode::sequence_out_of_order,
    };
    std::unordered_set<std::uint16_t> unique_codes;
    for (const auto code : kErrorCodes) {
        unique_codes.insert(static_cast<std::uint16_t>(code));
    }
    passed &= test_util::check(unique_codes.size() == kErrorCodes.size(), "error codes are unique");
    passed &= test_util::check(!unique_codes.contains(0), "error code 0 is unused");

    passed &= test_util::check(!validate_header(valid_header(MessageType::new_order)).has_value(),
                               "canonical new_order header is valid");
    passed &= test_util::check(!validate_header(valid_header(MessageType::execution, kFlagMore)).has_value(),
                               "more flag is permitted");

    auto bad_magic = valid_header(MessageType::cancel_order);
    bad_magic.magic = 0x0000;
    passed &= test_util::check(validate_header(bad_magic) == ErrorCode::malformed_frame, "wrong magic is malformed");

    auto bad_version = valid_header(MessageType::cancel_order);
    bad_version.version = 2;
    passed &= test_util::check(validate_header(bad_version) == ErrorCode::unsupported_version,
                               "version 2 is rejected");

    auto unknown_type = valid_header(MessageType::cancel_order);
    unknown_type.type = static_cast<MessageType>(50);
    unknown_type.payload_length = 16;
    passed &= test_util::check(validate_header(unknown_type) == ErrorCode::unknown_message_type,
                               "unknown type is rejected");

    auto wrong_length = valid_header(MessageType::new_order);
    wrong_length.payload_length = 16;
    passed &= test_util::check(validate_header(wrong_length) == ErrorCode::payload_length_invalid,
                               "fixed-size mismatch is rejected");

    auto too_large = valid_header(MessageType::new_order);
    too_large.payload_length = static_cast<std::uint16_t>(kMaxPayloadLength + 1);
    passed &= test_util::check(validate_header(too_large) == ErrorCode::payload_too_large,
                               "oversize length is rejected before payload allocation");

    auto reserved_flag = valid_header(MessageType::new_order);
    reserved_flag.flags = 0x0002;
    passed &= test_util::check(validate_header(reserved_flag) == ErrorCode::invalid_field,
                               "reserved flag bits must be zero");

    std::array<std::byte, kHeaderSize> encoded{};
    const auto original = valid_header(MessageType::replace_order, kFlagMore);
    passed &= test_util::check(encode_header(original, encoded), "header encode succeeds into 8-byte buffer");
    passed &= test_util::check(static_cast<unsigned>(encoded[0]) == 0x4C && static_cast<unsigned>(encoded[1]) == 0x58,
                               "magic encodes as LX bytes");

    const auto decoded = decode_header(encoded);
    passed &= test_util::check(decoded.has_value() && headers_equal(*decoded, original),
                               "header encode/decode round-trips");

    std::array<std::byte, 4> truncated{};
    passed &= test_util::check(!encode_header(original, truncated), "short encode buffer is rejected");
    passed &= test_util::check(!decode_header(std::span<const std::byte>(truncated)).has_value(),
                               "truncated header does not decode");

    std::array<std::byte, kHeaderSize> hostile{};
    hostile.fill(std::byte{0xFF});
    const auto hostile_header = decode_header(hostile);
    passed &= test_util::check(hostile_header.has_value(), "hostile bytes still parse as a header struct");
    passed &= test_util::check(validate_header(*hostile_header).has_value(),
                               "hostile header fails validation without trusting length");

    // ==========================================
    // Payload Codec Tests: Inbound Commands
    // ==========================================
    using low_latency_exchange::OrderId;
    using low_latency_exchange::OrderType;
    using low_latency_exchange::Price;
    using low_latency_exchange::Quantity;
    using low_latency_exchange::SequenceNumber;
    using low_latency_exchange::Side;
    using low_latency_exchange::protocol::CancelAcceptedEvent;
    using low_latency_exchange::protocol::decode_cancel_accepted;
    using low_latency_exchange::protocol::decode_cancel_order;
    using low_latency_exchange::protocol::decode_execution;
    using low_latency_exchange::protocol::decode_new_order;
    using low_latency_exchange::protocol::decode_order_accepted;
    using low_latency_exchange::protocol::decode_order_rejected;
    using low_latency_exchange::protocol::decode_protocol_error;
    using low_latency_exchange::protocol::decode_replace_order;
    using low_latency_exchange::protocol::encode_cancel_accepted;
    using low_latency_exchange::protocol::encode_cancel_order;
    using low_latency_exchange::protocol::encode_execution;
    using low_latency_exchange::protocol::encode_frame;
    using low_latency_exchange::protocol::encode_new_order;
    using low_latency_exchange::protocol::encode_order_accepted;
    using low_latency_exchange::protocol::encode_order_rejected;
    using low_latency_exchange::protocol::encode_protocol_error;
    using low_latency_exchange::protocol::encode_replace_order;
    using low_latency_exchange::protocol::ExecutionEvent;
    using low_latency_exchange::protocol::OrderAcceptedEvent;
    using low_latency_exchange::protocol::OrderRejectedEvent;
    using low_latency_exchange::protocol::ProtocolErrorEvent;

    // 1. NewOrder Limit Buy round trip
    {
        const low_latency_exchange::NewOrder order{
            .sequence = *SequenceNumber::from_value(101),
            .order_id = *OrderId::from_value(5001),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(100),
            .limit_price = Price::from_ticks(15000),
        };
        std::array<std::byte, kNewOrderSize> buf{};
        passed &= test_util::check(encode_new_order(order, buf), "encode_new_order limit buy succeeds");

        low_latency_exchange::NewOrder decoded_order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .side = Side::sell,
            .type = OrderType::market,
            .quantity = *Quantity::from_units(1),
            .limit_price = std::nullopt,
        };
        const auto err = decode_new_order(buf, decoded_order);
        passed &= test_util::check(!err.has_value(), "decode_new_order limit buy has no error");
        passed &= test_util::check(decoded_order.sequence.value() == 101, "decoded sequence matches");
        passed &= test_util::check(decoded_order.order_id.value() == 5001, "decoded order_id matches");
        passed &= test_util::check(decoded_order.side == Side::buy, "decoded side matches");
        passed &= test_util::check(decoded_order.type == OrderType::limit, "decoded type matches");
        passed &= test_util::check(decoded_order.quantity.units() == 100, "decoded quantity matches");
        passed &= test_util::check(decoded_order.limit_price.has_value() && decoded_order.limit_price->ticks() == 15000,
                                   "decoded price matches");
    }

    // 2. NewOrder Market Sell round trip
    {
        const low_latency_exchange::NewOrder order{
            .sequence = *SequenceNumber::from_value(102),
            .order_id = *OrderId::from_value(5002),
            .side = Side::sell,
            .type = OrderType::market,
            .quantity = *Quantity::from_units(50),
            .limit_price = std::nullopt,
        };
        std::array<std::byte, kNewOrderSize> buf{};
        passed &= test_util::check(encode_new_order(order, buf), "encode_new_order market sell succeeds");

        low_latency_exchange::NewOrder decoded_order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(1),
            .limit_price = Price::from_ticks(100),
        };
        const auto err = decode_new_order(buf, decoded_order);
        passed &= test_util::check(!err.has_value(), "decode_new_order market sell has no error");
        passed &= test_util::check(decoded_order.sequence.value() == 102, "decoded sequence matches");
        passed &= test_util::check(decoded_order.order_id.value() == 5002, "decoded order_id matches");
        passed &= test_util::check(decoded_order.side == Side::sell, "decoded side matches");
        passed &= test_util::check(decoded_order.type == OrderType::market, "decoded type matches");
        passed &= test_util::check(decoded_order.quantity.units() == 50, "decoded quantity matches");
        passed &= test_util::check(!decoded_order.limit_price.has_value(), "decoded price is market (no limit price)");
    }

    // 3. NewOrder Negative Tests
    {
        const low_latency_exchange::NewOrder order{
            .sequence = *SequenceNumber::from_value(103),
            .order_id = *OrderId::from_value(5003),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(1000),
        };
        std::array<std::byte, kNewOrderSize> valid_buf{};
        passed &= test_util::check(encode_new_order(order, valid_buf), "valid buffer encoded");

        low_latency_exchange::NewOrder out{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(1),
            .limit_price = Price::from_ticks(1),
        };

        // Short buffer
        passed &= test_util::check(decode_new_order(std::span<const std::byte>(valid_buf.data(), 30), out) ==
                                       ErrorCode::payload_length_invalid,
                                   "short buffer rejected");

        // Small encode buffer
        std::array<std::byte, 10> small_buf{};
        passed &= test_util::check(!encode_new_order(order, small_buf), "small encode buffer rejected");

        // Invalid side
        auto bad_side = valid_buf;
        bad_side[kNewOrderSideOffset] = std::byte{2};
        passed &= test_util::check(decode_new_order(bad_side, out) == ErrorCode::invalid_field,
                                   "invalid side byte rejected");

        // Invalid type
        auto bad_type = valid_buf;
        bad_type[kNewOrderTypeOffset] = std::byte{2};
        passed &= test_util::check(decode_new_order(bad_type, out) == ErrorCode::invalid_field,
                                   "invalid type byte rejected");

        // Limit order with 0 price
        auto limit_zero_price = valid_buf;
        for (std::size_t i = 0; i < 8; ++i) {
            limit_zero_price[kNewOrderPriceOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_new_order(limit_zero_price, out) == ErrorCode::limit_order_missing_price,
                                   "limit order missing price rejected");

        // Market order with non-zero price
        const low_latency_exchange::NewOrder market_order{
            .sequence = *SequenceNumber::from_value(104),
            .order_id = *OrderId::from_value(5004),
            .side = Side::buy,
            .type = OrderType::market,
            .quantity = *Quantity::from_units(10),
            .limit_price = std::nullopt,
        };
        std::array<std::byte, kNewOrderSize> market_buf{};
        passed &= test_util::check(encode_new_order(market_order, market_buf), "market order encoded");
        market_buf[kNewOrderPriceOffset] = std::byte{1};  // corrupt price to 1 tick
        passed &= test_util::check(decode_new_order(market_buf, out) == ErrorCode::market_order_has_price,
                                   "market order with price rejected");

        // Non-zero reserved byte
        auto bad_reserved = valid_buf;
        bad_reserved[kNewOrderReservedOffset] = std::byte{1};
        passed &= test_util::check(decode_new_order(bad_reserved, out) == ErrorCode::invalid_field,
                                   "non-zero reserved byte rejected");

        // Zero quantity
        auto zero_qty = valid_buf;
        for (std::size_t i = 0; i < 8; ++i) {
            zero_qty[kNewOrderQuantityOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_new_order(zero_qty, out) == ErrorCode::invalid_field,
                                   "zero quantity rejected");

        // Zero order id
        auto zero_oid = valid_buf;
        for (std::size_t i = 0; i < 8; ++i) {
            zero_oid[kOrderIdOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_new_order(zero_oid, out) == ErrorCode::invalid_field,
                                   "zero order id rejected");
    }

    // 4. CancelOrder Round Trip & Errors
    {
        const low_latency_exchange::CancelOrder cancel{
            .sequence = *SequenceNumber::from_value(201),
            .order_id = *OrderId::from_value(6001),
        };
        std::array<std::byte, kCancelOrderSize> buf{};
        passed &= test_util::check(encode_cancel_order(cancel, buf), "encode_cancel_order succeeds");

        low_latency_exchange::CancelOrder decoded{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
        };
        const auto err = decode_cancel_order(buf, decoded);
        passed &= test_util::check(!err.has_value(), "decode_cancel_order succeeds");
        passed &= test_util::check(decoded.sequence.value() == 201, "cancel sequence matches");
        passed &= test_util::check(decoded.order_id.value() == 6001, "cancel order_id matches");

        std::array<std::byte, 4> small_buf{};
        passed &= test_util::check(!encode_cancel_order(cancel, small_buf), "small cancel buffer rejected");
        passed &= test_util::check(decode_cancel_order(small_buf, decoded) == ErrorCode::payload_length_invalid,
                                   "short cancel payload rejected");

        auto zero_oid = buf;
        for (std::size_t i = 0; i < 8; ++i) {
            zero_oid[kOrderIdOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_cancel_order(zero_oid, decoded) == ErrorCode::invalid_field,
                                   "cancel zero order_id rejected");
    }

    // 5. ReplaceOrder Round Trip & Errors
    {
        const low_latency_exchange::ReplaceOrder replace{
            .sequence = *SequenceNumber::from_value(301),
            .order_id = *OrderId::from_value(7001),
            .new_quantity = *Quantity::from_units(250),
            .new_price = *Price::from_ticks(19900),
        };
        std::array<std::byte, kReplaceOrderSize> buf{};
        passed &= test_util::check(encode_replace_order(replace, buf), "encode_replace_order succeeds");

        low_latency_exchange::ReplaceOrder decoded{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .new_quantity = *Quantity::from_units(1),
            .new_price = *Price::from_ticks(1),
        };
        const auto err = decode_replace_order(buf, decoded);
        passed &= test_util::check(!err.has_value(), "decode_replace_order succeeds");
        passed &= test_util::check(decoded.sequence.value() == 301, "replace sequence matches");
        passed &= test_util::check(decoded.order_id.value() == 7001, "replace order_id matches");
        passed &= test_util::check(decoded.new_quantity.units() == 250, "replace quantity matches");
        passed &= test_util::check(decoded.new_price.ticks() == 19900, "replace price matches");

        // Zero quantity rejected
        auto zero_qty = buf;
        using low_latency_exchange::protocol::layout::kReplaceQuantityOffset;
        for (std::size_t i = 0; i < 8; ++i) {
            zero_qty[kReplaceQuantityOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_replace_order(zero_qty, decoded) == ErrorCode::invalid_field,
                                   "replace zero quantity rejected");

        // Negative/zero price rejected
        auto zero_price = buf;
        using low_latency_exchange::protocol::layout::kReplacePriceOffset;
        for (std::size_t i = 0; i < 8; ++i) {
            zero_price[kReplacePriceOffset + i] = std::byte{0};
        }
        passed &= test_util::check(decode_replace_order(zero_price, decoded) == ErrorCode::invalid_field,
                                   "replace zero price rejected");
    }

    // ==========================================
    // Payload Codec Tests: Outbound Events
    // ==========================================

    // 6. OrderAcceptedEvent
    {
        const OrderAcceptedEvent event{
            .sequence = *SequenceNumber::from_value(401),
            .order_id = *OrderId::from_value(8001),
            .remaining_quantity = 50,
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kOrderAcceptedSize> buf{};
        passed &= test_util::check(encode_order_accepted(event, buf), "encode_order_accepted succeeds");

        OrderAcceptedEvent decoded{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .remaining_quantity = 0,
        };
        passed &= test_util::check(!decode_order_accepted(buf, decoded).has_value(), "decode_order_accepted succeeds");
        passed &= test_util::check(decoded.sequence.value() == 401, "order accepted sequence matches");
        passed &= test_util::check(decoded.order_id.value() == 8001, "order accepted order_id matches");
        passed &= test_util::check(decoded.remaining_quantity == 50, "order accepted remaining qty matches");
    }

    // 7. OrderRejectedEvent
    {
        const OrderRejectedEvent event{
            .sequence = *SequenceNumber::from_value(402),
            .order_id = *OrderId::from_value(8002),
            .error_code = ErrorCode::duplicate_order_id,
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kOrderRejectedSize> buf{};
        passed &= test_util::check(encode_order_rejected(event, buf), "encode_order_rejected succeeds");

        OrderRejectedEvent decoded{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .error_code = ErrorCode::invalid_field,
        };
        passed &= test_util::check(!decode_order_rejected(buf, decoded).has_value(), "decode_order_rejected succeeds");
        passed &= test_util::check(decoded.sequence.value() == 402, "order rejected sequence matches");
        passed &= test_util::check(decoded.order_id.value() == 8002, "order rejected order_id matches");
        passed &= test_util::check(decoded.error_code == ErrorCode::duplicate_order_id,
                                   "order rejected error code matches");

        // Non-zero reserved bytes check
        buf[low_latency_exchange::protocol::layout::kRejectReservedOffset] = std::byte{0xFF};
        passed &= test_util::check(decode_order_rejected(buf, decoded) == ErrorCode::invalid_field,
                                   "order rejected with non-zero reserved bytes rejected");
    }

    // 8. ExecutionEvent
    {
        const ExecutionEvent event{
            .sequence = *SequenceNumber::from_value(501),
            .resting_order_id = *OrderId::from_value(1001),
            .incoming_order_id = *OrderId::from_value(2001),
            .price = *Price::from_ticks(12345),
            .quantity = *Quantity::from_units(42),
        };
        std::array<std::byte, kExecutionSize> buf{};
        passed &= test_util::check(encode_execution(event, buf), "encode_execution succeeds");

        ExecutionEvent decoded{
            .sequence = *SequenceNumber::from_value(1),
            .resting_order_id = *OrderId::from_value(1),
            .incoming_order_id = *OrderId::from_value(1),
            .price = *Price::from_ticks(1),
            .quantity = *Quantity::from_units(1),
        };
        passed &= test_util::check(!decode_execution(buf, decoded).has_value(), "decode_execution succeeds");
        passed &= test_util::check(decoded.sequence.value() == 501, "execution sequence matches");
        passed &= test_util::check(decoded.resting_order_id.value() == 1001, "execution resting id matches");
        passed &= test_util::check(decoded.incoming_order_id.value() == 2001, "execution incoming id matches");
        passed &= test_util::check(decoded.price.ticks() == 12345, "execution price matches");
        passed &= test_util::check(decoded.quantity.units() == 42, "execution quantity matches");
    }

    // 9. CancelAcceptedEvent
    {
        const CancelAcceptedEvent event{
            .sequence = *SequenceNumber::from_value(601),
            .order_id = *OrderId::from_value(3001),
            .cancelled_quantity = *Quantity::from_units(77),
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kCancelAcceptedSize> buf{};
        passed &= test_util::check(encode_cancel_accepted(event, buf), "encode_cancel_accepted succeeds");

        CancelAcceptedEvent decoded{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .cancelled_quantity = *Quantity::from_units(1),
        };
        passed &= test_util::check(!decode_cancel_accepted(buf, decoded).has_value(), "decode_cancel_accepted succeeds");
        passed &= test_util::check(decoded.sequence.value() == 601, "cancel accepted sequence matches");
        passed &= test_util::check(decoded.order_id.value() == 3001, "cancel accepted order_id matches");
        passed &= test_util::check(decoded.cancelled_quantity.units() == 77, "cancel accepted qty matches");
    }

    // 10. ProtocolErrorEvent
    {
        const ProtocolErrorEvent event{
            .error_code = ErrorCode::malformed_frame,
            .offending_type = static_cast<std::uint8_t>(MessageType::new_order),
        };
        std::array<std::byte, kProtocolErrorSize> buf{};
        passed &= test_util::check(encode_protocol_error(event, buf), "encode_protocol_error succeeds");

        ProtocolErrorEvent decoded{};
        passed &= test_util::check(!decode_protocol_error(buf, decoded).has_value(), "decode_protocol_error succeeds");
        passed &= test_util::check(decoded.error_code == ErrorCode::malformed_frame, "protocol error code matches");
        passed &= test_util::check(decoded.offending_type == static_cast<std::uint8_t>(MessageType::new_order),
                                   "offending type matches");

        // Non-zero reserved bytes check
        buf[low_latency_exchange::protocol::layout::kProtocolErrorReservedOffset] = std::byte{0x01};
        passed &= test_util::check(decode_protocol_error(buf, decoded) == ErrorCode::invalid_field,
                                   "protocol error with non-zero reserved bytes rejected");
    }

    // 11. Full Frame Encoder (`encode_frame`)
    {
        const OrderAcceptedEvent event{
            .sequence = *SequenceNumber::from_value(777),
            .order_id = *OrderId::from_value(888),
            .remaining_quantity = 10,
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kOrderAcceptedSize> payload_buf{};
        passed &= test_util::check(encode_order_accepted(event, payload_buf), "payload encode succeeds");

        std::array<std::byte, kHeaderSize + low_latency_exchange::protocol::layout::kOrderAcceptedSize> frame_buf{};
        passed &= test_util::check(encode_frame(MessageType::order_accepted, payload_buf, frame_buf, kFlagMore),
                                   "encode_frame succeeds");

        const auto header = decode_header(frame_buf);
        passed &= test_util::check(header.has_value(), "frame header decodes");
        passed &= test_util::check(header->type == MessageType::order_accepted, "frame type matches");
        passed &= test_util::check((header->flags & kFlagMore) != 0, "frame flags match");
        passed &= test_util::check(header->payload_length == payload_buf.size(), "frame payload length matches");

        OrderAcceptedEvent decoded_event{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .remaining_quantity = 0,
        };
        const auto payload_slice = std::span<const std::byte>(frame_buf).subspan(kHeaderSize, header->payload_length);
        passed &= test_util::check(!decode_order_accepted(payload_slice, decoded_event).has_value(),
                                   "frame payload decodes");
        passed &= test_util::check(decoded_event.sequence.value() == 777, "frame event sequence matches");
    }

    // 12. Error Code mapping helper verification
    {
        using low_latency_exchange::CancelRejectReason;
        using low_latency_exchange::OrderRejectReason;
        using low_latency_exchange::ReplaceRejectReason;
        using low_latency_exchange::protocol::to_error_code;

        passed &= test_util::check(to_error_code(OrderRejectReason::invalid_order) == ErrorCode::invalid_order,
                                   "invalid_order maps correctly");
        passed &= test_util::check(to_error_code(OrderRejectReason::duplicate_order_id) == ErrorCode::duplicate_order_id,
                                   "duplicate_order_id maps correctly");
        passed &= test_util::check(to_error_code(CancelRejectReason::order_not_found) == ErrorCode::order_not_found,
                                   "cancel reject maps correctly");
        passed &= test_util::check(to_error_code(ReplaceRejectReason::order_not_found) == ErrorCode::order_not_found,
                                   "replace reject maps correctly");
    }

    return passed ? 0 : 1;
}


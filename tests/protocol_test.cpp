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

    return passed ? 0 : 1;
}

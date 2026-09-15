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

}  // namespace low_latency_exchange::protocol

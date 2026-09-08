#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace low_latency_exchange {

class Price final {
  public:
    [[nodiscard]] static constexpr std::optional<Price> from_ticks(std::int64_t ticks) noexcept {
        if (ticks <= 0) {
            return std::nullopt;
        }
        return Price{ticks};
    }

    [[nodiscard]] constexpr std::int64_t ticks() const noexcept {
        return ticks_;
    }

    constexpr auto operator<=>(const Price&) const noexcept = default;

  private:
    explicit constexpr Price(std::int64_t ticks) noexcept : ticks_{ticks} {}

    std::int64_t ticks_;
};

class Quantity final {
  public:
    [[nodiscard]] static constexpr std::optional<Quantity> from_units(std::uint64_t units) noexcept {
        if (units == 0) {
            return std::nullopt;
        }
        return Quantity{units};
    }

    [[nodiscard]] constexpr std::uint64_t units() const noexcept {
        return units_;
    }

    constexpr auto operator<=>(const Quantity&) const noexcept = default;

  private:
    explicit constexpr Quantity(std::uint64_t units) noexcept : units_{units} {}

    std::uint64_t units_;
};

class OrderId final {
  public:
    [[nodiscard]] static constexpr std::optional<OrderId> from_value(std::uint64_t value) noexcept {
        if (value == 0) {
            return std::nullopt;
        }
        return OrderId{value};
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    constexpr auto operator<=>(const OrderId&) const noexcept = default;

  private:
    explicit constexpr OrderId(std::uint64_t value) noexcept : value_{value} {}

    std::uint64_t value_;
};

class SequenceNumber final {
  public:
    [[nodiscard]] static constexpr std::optional<SequenceNumber> from_value(std::uint64_t value) noexcept {
        if (value == 0) {
            return std::nullopt;
        }
        return SequenceNumber{value};
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    constexpr auto operator<=>(const SequenceNumber&) const noexcept = default;

  private:
    explicit constexpr SequenceNumber(std::uint64_t value) noexcept : value_{value} {}

    std::uint64_t value_;
};

enum class Side : std::uint8_t {
    buy,
    sell,
};

enum class OrderType : std::uint8_t {
    limit,
    market,
};

}  // namespace low_latency_exchange

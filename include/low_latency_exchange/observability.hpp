#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace low_latency_exchange {

inline constexpr std::size_t kLatencyHistogramBuckets = 64;

struct LatencyHistogramSnapshot {
    std::array<std::uint64_t, kLatencyHistogramBuckets> buckets{};
    std::uint64_t count = 0;
    std::uint64_t min_nanoseconds = 0;
    std::uint64_t max_nanoseconds = 0;

    /** Returns the upper bound of the logarithmic bucket at a percentile. */
    [[nodiscard]] std::uint64_t percentile_upper_bound(double percentile) const noexcept {
        if (count == 0) {
            return 0;
        }
        const auto rank = static_cast<std::uint64_t>(percentile * static_cast<double>(count - 1));
        std::uint64_t observed = 0;
        for (std::size_t index = 0; index < buckets.size(); ++index) {
            observed += buckets[index];
            if (observed > rank) {
                return index == 0 ? 1 : (std::uint64_t{1} << index);
            }
        }
        return max_nanoseconds;
    }
};

/** Fixed-memory logarithmic histogram for nanosecond latency observations. */
class LatencyHistogram final {
  public:
    void observe(std::uint64_t nanoseconds) noexcept {
        const std::uint64_t normalized = nanoseconds == 0 ? 1 : nanoseconds;
        const std::size_t bucket = std::min<std::size_t>(
            static_cast<std::size_t>(std::bit_width(normalized) - 1), kLatencyHistogramBuckets - 1);
        ++buckets_[bucket];
        ++count_;
        if (normalized < min_nanoseconds_) {
            min_nanoseconds_ = normalized;
        }
        if (normalized > max_nanoseconds_) {
            max_nanoseconds_ = normalized;
        }
    }

    [[nodiscard]] LatencyHistogramSnapshot snapshot() const noexcept {
        return LatencyHistogramSnapshot{
            .buckets = buckets_,
            .count = count_,
            .min_nanoseconds = count_ == 0 ? 0 : min_nanoseconds_,
            .max_nanoseconds = max_nanoseconds_,
        };
    }

  private:
    std::array<std::uint64_t, kLatencyHistogramBuckets> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t min_nanoseconds_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_nanoseconds_ = 0;
};

}  // namespace low_latency_exchange

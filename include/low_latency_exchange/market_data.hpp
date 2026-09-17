#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/spsc_queue.hpp"
#include "low_latency_exchange/types.hpp"

namespace low_latency_exchange {

inline constexpr std::size_t kMarketDataDepth = 5;

struct MarketDataLevel {
    Price price;
    Quantity quantity;
    std::size_t order_count = 0;

    constexpr auto operator<=>(const MarketDataLevel&) const noexcept = default;
};

using MarketDataDepth = std::array<std::optional<MarketDataLevel>, kMarketDataDepth>;

enum class MarketDataEventKind : std::uint8_t {
    snapshot,
    depth_update,
};

struct MarketDataEvent {
    std::optional<FeedSequence> sequence;
    MarketDataEventKind kind = MarketDataEventKind::snapshot;
    MarketDataDepth bids{};
    MarketDataDepth asks{};

    [[nodiscard]] bool valid() const noexcept {
        return sequence.has_value();
    }
};

struct MarketDataMetrics {
    std::uint64_t published_events = 0;
    std::uint64_t dropped_events = 0;
    std::size_t queue_occupancy = 0;
    std::size_t max_queue_occupancy = 0;
};

using MarketDataQueue = BoundedSPSCQueue<MarketDataEvent, 1024>;

/**
 * The matching-engine thread is the sole producer. Queue-full events are
 * counted and dropped; the next successfully queued event is a snapshot so a
 * consumer can recover without ever stalling matching.
 */
class MarketDataFeed final {
  public:
    explicit MarketDataFeed(MarketDataQueue& queue, std::size_t snapshot_interval = 64) noexcept;

    void publish(const OrderBook& book) noexcept;

    [[nodiscard]] MarketDataMetrics metrics() const noexcept;

  private:
    [[nodiscard]] MarketDataEvent make_event(const OrderBook& book,
                                             MarketDataEventKind kind,
                                             FeedSequence sequence) const noexcept;
    void record_queue_occupancy() noexcept;

    MarketDataQueue& queue_;
    std::uint64_t next_sequence_ = 1;
    std::size_t snapshot_interval_ = 64;
    std::size_t updates_since_snapshot_ = 0;
    std::uint64_t published_events_ = 0;
    std::uint64_t dropped_events_ = 0;
    std::size_t max_queue_occupancy_ = 0;
    bool snapshot_pending_ = true;
};

/**
 * Consumer-side materialized depth book. A snapshot resets recovery state;
 * incremental updates are accepted only when their feed sequence is exactly
 * contiguous with the prior event.
 */
class MarketDataBook final {
  public:
    [[nodiscard]] bool apply(const MarketDataEvent& event) noexcept;

    [[nodiscard]] bool synchronized() const noexcept;
    [[nodiscard]] std::optional<FeedSequence> last_sequence() const noexcept;
    [[nodiscard]] const MarketDataDepth& bids() const noexcept;
    [[nodiscard]] const MarketDataDepth& asks() const noexcept;

  private:
    MarketDataDepth bids_{};
    MarketDataDepth asks_{};
    std::optional<FeedSequence> last_sequence_;
    bool synchronized_ = false;
};

/**
 * Publisher-side queue consumer. Delivery to concrete transports is a later
 * edge concern; this class deliberately keeps queue draining and subscriber
 * state reconstruction independent of sockets.
 */
class MarketDataPublisher final {
  public:
    explicit MarketDataPublisher(MarketDataQueue& queue) noexcept : queue_{queue} {}

    [[nodiscard]] bool publish_one(MarketDataBook& subscriber) noexcept;
    [[nodiscard]] std::size_t publish_available(MarketDataBook& subscriber) noexcept;

  private:
    MarketDataQueue& queue_;
};

}  // namespace low_latency_exchange

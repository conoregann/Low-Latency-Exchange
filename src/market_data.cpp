#include "low_latency_exchange/market_data.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace low_latency_exchange {

namespace {

MarketDataLevel to_market_data_level(const LevelQuote& level) noexcept {
    return MarketDataLevel{
        .price = level.price,
        .quantity = level.quantity,
        .order_count = level.order_count,
    };
}

bool is_next_sequence(FeedSequence previous, FeedSequence current) noexcept {
    return previous.value() != std::numeric_limits<std::uint64_t>::max() &&
           current.value() == previous.value() + 1;
}

}  // namespace

MarketDataFeed::MarketDataFeed(MarketDataQueue& queue, std::size_t snapshot_interval) noexcept
    : queue_{queue}, snapshot_interval_{std::max<std::size_t>(snapshot_interval, 1)} {}

void MarketDataFeed::publish(const OrderBook& book) noexcept {
    const auto sequence = FeedSequence::from_value(next_sequence_);
    if (!sequence.has_value() || next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        ++dropped_events_;
        snapshot_pending_ = true;
        return;
    }
    ++next_sequence_;

    const MarketDataEventKind kind = snapshot_pending_ ? MarketDataEventKind::snapshot
                                                         : MarketDataEventKind::depth_update;
    MarketDataEvent event = make_event(book, kind, *sequence);
    if (!queue_.try_push(std::move(event))) {
        ++dropped_events_;
        snapshot_pending_ = true;
        record_queue_occupancy();
        return;
    }

    ++published_events_;
    record_queue_occupancy();
    if (kind == MarketDataEventKind::snapshot) {
        snapshot_pending_ = false;
        updates_since_snapshot_ = 0;
    } else {
        ++updates_since_snapshot_;
        if (updates_since_snapshot_ >= snapshot_interval_) {
            snapshot_pending_ = true;
        }
    }
}

MarketDataMetrics MarketDataFeed::metrics() const noexcept {
    return MarketDataMetrics{
        .published_events = published_events_,
        .dropped_events = dropped_events_,
        .queue_occupancy = queue_.size(),
        .max_queue_occupancy = max_queue_occupancy_,
    };
}

MarketDataEvent MarketDataFeed::make_event(const OrderBook& book,
                                           MarketDataEventKind kind,
                                           FeedSequence sequence) const noexcept {
    std::array<std::optional<LevelQuote>, kMarketDataDepth> bid_levels{};
    std::array<std::optional<LevelQuote>, kMarketDataDepth> ask_levels{};
    book.copy_depth(bid_levels, ask_levels);

    MarketDataEvent event{
        .sequence = sequence,
        .kind = kind,
    };
    for (std::size_t index = 0; index < kMarketDataDepth; ++index) {
        if (bid_levels[index].has_value()) {
            event.bids[index] = to_market_data_level(*bid_levels[index]);
        }
        if (ask_levels[index].has_value()) {
            event.asks[index] = to_market_data_level(*ask_levels[index]);
        }
    }
    return event;
}

void MarketDataFeed::record_queue_occupancy() noexcept {
    max_queue_occupancy_ = std::max(max_queue_occupancy_, queue_.size());
}

bool MarketDataBook::apply(const MarketDataEvent& event) noexcept {
    if (!event.valid()) {
        return false;
    }

    if (event.kind == MarketDataEventKind::snapshot) {
        bids_ = event.bids;
        asks_ = event.asks;
        last_sequence_ = event.sequence;
        synchronized_ = true;
        return true;
    }

    if (!synchronized_ || !last_sequence_.has_value() || !is_next_sequence(*last_sequence_, *event.sequence)) {
        synchronized_ = false;
        return false;
    }

    bids_ = event.bids;
    asks_ = event.asks;
    last_sequence_ = event.sequence;
    return true;
}

bool MarketDataBook::synchronized() const noexcept {
    return synchronized_;
}

std::optional<FeedSequence> MarketDataBook::last_sequence() const noexcept {
    return last_sequence_;
}

const MarketDataDepth& MarketDataBook::bids() const noexcept {
    return bids_;
}

const MarketDataDepth& MarketDataBook::asks() const noexcept {
    return asks_;
}

bool MarketDataPublisher::publish_one(MarketDataBook& subscriber) noexcept {
    MarketDataEvent event;
    if (!queue_.try_pop(event)) {
        return false;
    }
    return subscriber.apply(event);
}

std::size_t MarketDataPublisher::publish_available(MarketDataBook& subscriber) noexcept {
    std::size_t published = 0;
    MarketDataEvent event;
    while (queue_.try_pop(event)) {
        if (subscriber.apply(event)) {
            ++published;
        }
    }
    return published;
}

}  // namespace low_latency_exchange

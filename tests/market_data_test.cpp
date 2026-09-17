#include "low_latency_exchange/market_data.hpp"
#include "low_latency_exchange/tcp_gateway.hpp"
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

using low_latency_exchange::FeedSequence;
using low_latency_exchange::InboundMessage;
using low_latency_exchange::InboundQueue;
using low_latency_exchange::LevelQuote;
using low_latency_exchange::MarketDataBook;
using low_latency_exchange::MarketDataDepth;
using low_latency_exchange::MarketDataEvent;
using low_latency_exchange::MarketDataEventKind;
using low_latency_exchange::MarketDataFeed;
using low_latency_exchange::MarketDataPublisher;
using low_latency_exchange::MarketDataQueue;
using low_latency_exchange::MatchingEngineService;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderType;
using low_latency_exchange::OrderBook;
using low_latency_exchange::OutboundQueue;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;
using low_latency_exchange::kMarketDataDepth;

namespace {

NewOrder limit_order(std::uint64_t sequence,
                     std::uint64_t order_id,
                     Side side,
                     std::uint64_t quantity,
                     std::int64_t price) {
    return NewOrder{
        .sequence = *SequenceNumber::from_value(sequence),
        .order_id = *OrderId::from_value(order_id),
        .side = side,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(quantity),
        .limit_price = Price::from_ticks(price),
    };
}

bool matches_book(const MarketDataBook& market_data_book, const OrderBook& order_book) {
    std::array<std::optional<LevelQuote>, kMarketDataDepth> expected_bids{};
    std::array<std::optional<LevelQuote>, kMarketDataDepth> expected_asks{};
    order_book.copy_depth(expected_bids, expected_asks);

    const auto matches_side = [](const MarketDataDepth& actual,
                                 const std::array<std::optional<LevelQuote>, kMarketDataDepth>& expected) {
        for (std::size_t index = 0; index < kMarketDataDepth; ++index) {
            if (actual[index].has_value() != expected[index].has_value()) {
                return false;
            }
            if (actual[index].has_value() &&
                (actual[index]->price != expected[index]->price || actual[index]->quantity != expected[index]->quantity ||
                 actual[index]->order_count != expected[index]->order_count)) {
                return false;
            }
        }
        return true;
    };

    return matches_side(market_data_book.bids(), expected_bids) && matches_side(market_data_book.asks(), expected_asks);
}

}  // namespace

int main() {
    bool passed = true;

    // The engine emits an initial recovery snapshot, then sequenced depth updates.
    {
        InboundQueue inbound;
        OutboundQueue outbound;
        MarketDataQueue queue;
        MarketDataFeed feed(queue, 8);
        MatchingEngineService engine(inbound, outbound, &feed);
        MarketDataPublisher publisher(queue);
        MarketDataBook subscriber;

        const NewOrder buy = limit_order(1, 1'001, Side::buy, 20, 100);
        passed &= test_util::check(inbound.try_push(InboundMessage{1, buy}), "enqueue initial buy");
        passed &= test_util::check(engine.process_available() == 1, "engine processes initial buy");
        passed &= test_util::check(publisher.publish_one(subscriber), "publisher applies initial snapshot");
        passed &= test_util::check(subscriber.synchronized(), "subscriber synchronized by snapshot");
        passed &= test_util::check(subscriber.last_sequence() == FeedSequence::from_value(1), "snapshot sequence is 1");
        passed &= test_util::check(matches_book(subscriber, engine.order_book()), "snapshot matches engine depth");

        const NewOrder sell = limit_order(2, 1'002, Side::sell, 5, 100);
        passed &= test_util::check(inbound.try_push(InboundMessage{1, sell}), "enqueue crossing sell");
        passed &= test_util::check(engine.process_available() == 1, "engine processes crossing sell");
        passed &= test_util::check(publisher.publish_one(subscriber), "publisher applies sequenced depth update");
        passed &= test_util::check(subscriber.last_sequence() == FeedSequence::from_value(2), "depth update sequence is 2");
        passed &= test_util::check(matches_book(subscriber, engine.order_book()),
                                   "snapshot plus depth update reconstructs engine depth");
    }

    // A later snapshot recovers a subscriber that missed an incremental update.
    {
        MarketDataQueue queue;
        MarketDataFeed feed(queue, 1);
        MarketDataPublisher publisher(queue);
        MarketDataBook subscriber;
        OrderBook book;

        const auto first = book.submit(limit_order(1, 2'001, Side::buy, 10, 100));
        passed &= test_util::check(first.accepted(), "first recovery order accepted");
        feed.publish(book);
        passed &= test_util::check(publisher.publish_one(subscriber), "subscriber consumes initial recovery snapshot");

        const auto second = book.submit(limit_order(2, 2'002, Side::buy, 10, 99));
        passed &= test_util::check(second.accepted(), "second recovery order accepted");
        feed.publish(book);
        MarketDataEvent missed_update;
        passed &= test_util::check(queue.try_pop(missed_update) && missed_update.kind == MarketDataEventKind::depth_update,
                                   "simulate a missed incremental update");

        const auto third = book.submit(limit_order(3, 2'003, Side::sell, 5, 101));
        passed &= test_util::check(third.accepted(), "third recovery order accepted");
        feed.publish(book);
        passed &= test_util::check(publisher.publish_one(subscriber), "later snapshot is accepted after a feed gap");
        passed &= test_util::check(subscriber.synchronized(), "later snapshot restores synchronization");
        passed &= test_util::check(matches_book(subscriber, book), "recovered subscriber matches current engine depth");
    }

    // A full market-data queue drops updates and records metrics without stalling the matching engine.
    {
        InboundQueue inbound;
        OutboundQueue outbound;
        MarketDataQueue queue;
        MarketDataFeed feed(queue, 64);
        MatchingEngineService engine(inbound, outbound, &feed);

        bool processed_all = true;
        for (std::uint64_t value = 1; value <= MarketDataQueue::capacity() + 1; ++value) {
            const NewOrder order = limit_order(value, 3'000 + value, Side::buy, 1, 100);
            if (!inbound.try_push(InboundMessage{1, order}) || engine.process_available() != 1) {
                processed_all = false;
                break;
            }
        }

        const auto metrics = feed.metrics();
        passed &= test_util::check(processed_all, "matching engine continues while market-data consumer is slow");
        passed &= test_util::check(engine.order_book().resting_order_count() == MarketDataQueue::capacity() + 1,
                                   "all orders reached the matching engine despite feed backpressure");
        passed &= test_util::check(queue.full(), "slow publisher fills the bounded market-data queue");
        passed &= test_util::check(metrics.dropped_events > 0, "queue-full market-data updates are dropped and measured");
        passed &= test_util::check(metrics.max_queue_occupancy == MarketDataQueue::capacity(),
                                   "market-data metrics record peak queue occupancy");
    }

    return passed ? 0 : 1;
}

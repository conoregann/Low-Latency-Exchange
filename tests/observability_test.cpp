#include "low_latency_exchange/tcp_gateway.hpp"
#include "test_util.hpp"

#include <chrono>
#include <cstdint>

using low_latency_exchange::CancelOrder;
using low_latency_exchange::InboundMessage;
using low_latency_exchange::InboundQueue;
using low_latency_exchange::MatchingEngineService;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderType;
using low_latency_exchange::OutboundQueue;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;

int main() {
    InboundQueue inbound;
    OutboundQueue outbound;
    MatchingEngineService engine(inbound, outbound);
    const auto received_at = std::chrono::steady_clock::now();

    const NewOrder buy{
        .sequence = *SequenceNumber::from_value(1),
        .order_id = *OrderId::from_value(1),
        .side = Side::buy,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(10),
        .limit_price = Price::from_ticks(100),
    };
    const NewOrder sell{
        .sequence = *SequenceNumber::from_value(2),
        .order_id = *OrderId::from_value(2),
        .side = Side::sell,
        .type = OrderType::limit,
        .quantity = *Quantity::from_units(10),
        .limit_price = Price::from_ticks(100),
    };
    const CancelOrder missing_cancel{
        .sequence = *SequenceNumber::from_value(3),
        .order_id = *OrderId::from_value(999),
    };

    bool passed = true;
    passed &= test_util::check(inbound.try_push(InboundMessage{1, buy, 48, received_at}), "enqueue buy");
    passed &= test_util::check(inbound.try_push(InboundMessage{1, sell, 48, received_at}), "enqueue sell");
    passed &= test_util::check(inbound.try_push(InboundMessage{1, missing_cancel, 24, received_at}), "enqueue cancel");
    passed &= test_util::check(engine.process_available() == 3, "process workload");

    const auto metrics = engine.metrics();
    passed &= test_util::check(metrics.accepted_commands == 2, "count accepted commands");
    passed &= test_util::check(metrics.rejected_commands == 1, "count rejected commands");
    passed &= test_util::check(metrics.accepted_cancels == 0, "count accepted cancels");
    passed &= test_util::check(metrics.rejected_cancels == 1, "count rejected cancels");
    passed &= test_util::check(metrics.executions == 1, "count executions");
    passed &= test_util::check(metrics.inbound_bytes == 120, "count inbound bytes");
    passed &= test_util::check(metrics.outbound_bytes == 144, "count outbound bytes");
    passed &= test_util::check(metrics.inbound_queue_high_water == 3, "measure inbound high-water");
    passed &= test_util::check(metrics.outbound_queue_high_water == 4, "measure outbound high-water");
    passed &= test_util::check(metrics.command_to_ack_latency.count == 3, "record command latency");
    passed &= test_util::check(metrics.command_to_ack_latency.max_nanoseconds > 0, "latency has a maximum");
    return passed ? 0 : 1;
}

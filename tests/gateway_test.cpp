#include "low_latency_exchange/tcp_gateway.hpp"
#include "test_util.hpp"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <thread>
#include <vector>

using low_latency_exchange::BoundedSPSCQueue;
using low_latency_exchange::CancelOrder;
using low_latency_exchange::InboundMessage;
using low_latency_exchange::InboundQueue;
using low_latency_exchange::MatchingEngineService;
using low_latency_exchange::NewOrder;
using low_latency_exchange::OrderId;
using low_latency_exchange::OrderType;
using low_latency_exchange::OutboundMessage;
using low_latency_exchange::OutboundQueue;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::ReplaceOrder;
using low_latency_exchange::SequenceNumber;
using low_latency_exchange::Side;
using low_latency_exchange::TcpGateway;
using low_latency_exchange::protocol::CancelAcceptedEvent;
using low_latency_exchange::protocol::decode_cancel_accepted;
using low_latency_exchange::protocol::decode_execution;
using low_latency_exchange::protocol::decode_header;
using low_latency_exchange::protocol::decode_order_accepted;
using low_latency_exchange::protocol::decode_order_rejected;
using low_latency_exchange::protocol::decode_protocol_error;
using low_latency_exchange::protocol::encode_frame;
using low_latency_exchange::protocol::encode_cancel_order;
using low_latency_exchange::protocol::encode_new_order;
using low_latency_exchange::protocol::encode_replace_order;
using low_latency_exchange::protocol::ErrorCode;
using low_latency_exchange::protocol::ExecutionEvent;
using low_latency_exchange::protocol::kFlagMore;
using low_latency_exchange::protocol::kHeaderSize;
using low_latency_exchange::protocol::MessageType;
using low_latency_exchange::protocol::OrderAcceptedEvent;
using low_latency_exchange::protocol::OrderRejectedEvent;
using low_latency_exchange::protocol::ProtocolErrorEvent;
using low_latency_exchange::protocol::WireHeader;

namespace {

// Synchronous helper to read a full wire frame from a socket
bool read_frame(boost::asio::ip::tcp::socket& sock,
                WireHeader& header_out,
                std::vector<std::byte>& payload_out) {
    std::array<std::byte, kHeaderSize> header_bytes{};
    boost::system::error_code ec;
    boost::asio::read(sock, boost::asio::buffer(header_bytes), ec);
    if (ec) {
        return false;
    }
    const auto decoded = decode_header(header_bytes);
    if (!decoded.has_value()) {
        return false;
    }
    header_out = *decoded;
    payload_out.resize(header_out.payload_length);
    if (header_out.payload_length > 0) {
        boost::asio::read(sock, boost::asio::buffer(payload_out), ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

// Synchronous helper to write raw bytes to a socket
bool write_all(boost::asio::ip::tcp::socket& sock, std::span<const std::byte> bytes) {
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(bytes.data(), bytes.size()), ec);
    return !ec;
}

template <std::size_t PayloadSize>
bool write_command(boost::asio::ip::tcp::socket& sock,
                   MessageType type,
                   const std::array<std::byte, PayloadSize>& payload) {
    std::array<std::byte, kHeaderSize + PayloadSize> frame{};
    return encode_frame(type, payload, frame, 0) && write_all(sock, frame);
}

bool send_new_order(boost::asio::ip::tcp::socket& sock, const NewOrder& order) {
    std::array<std::byte, low_latency_exchange::protocol::layout::kNewOrderSize> payload{};
    return encode_new_order(order, payload) && write_command(sock, MessageType::new_order, payload);
}

bool send_cancel_order(boost::asio::ip::tcp::socket& sock, const CancelOrder& order) {
    std::array<std::byte, low_latency_exchange::protocol::layout::kCancelOrderSize> payload{};
    return encode_cancel_order(order, payload) && write_command(sock, MessageType::cancel_order, payload);
}

bool send_replace_order(boost::asio::ip::tcp::socket& sock, const ReplaceOrder& order) {
    std::array<std::byte, low_latency_exchange::protocol::layout::kReplaceOrderSize> payload{};
    return encode_replace_order(order, payload) && write_command(sock, MessageType::replace_order, payload);
}

bool expect_order_accepted(boost::asio::ip::tcp::socket& sock,
                           MessageType expected_type,
                           std::uint64_t expected_sequence,
                           std::uint64_t expected_order_id,
                           std::uint64_t expected_remaining) {
    WireHeader header{};
    std::vector<std::byte> payload;
    if (!read_frame(sock, header, payload) || header.type != expected_type || header.flags != 0) {
        return false;
    }

    OrderAcceptedEvent event{
        .sequence = *SequenceNumber::from_value(1),
        .order_id = *OrderId::from_value(1),
        .remaining_quantity = 0,
    };
    return !decode_order_accepted(payload, event).has_value() &&
           event.sequence.value() == expected_sequence && event.order_id.value() == expected_order_id &&
           event.remaining_quantity == expected_remaining;
}

bool expect_cancel_accepted(boost::asio::ip::tcp::socket& sock,
                            std::uint64_t expected_sequence,
                            std::uint64_t expected_order_id,
                            std::uint64_t expected_cancelled_quantity) {
    WireHeader header{};
    std::vector<std::byte> payload;
    if (!read_frame(sock, header, payload) || header.type != MessageType::cancel_accepted || header.flags != 0) {
        return false;
    }

    CancelAcceptedEvent event{
        .sequence = *SequenceNumber::from_value(1),
        .order_id = *OrderId::from_value(1),
        .cancelled_quantity = *Quantity::from_units(1),
    };
    return !decode_cancel_accepted(payload, event).has_value() &&
           event.sequence.value() == expected_sequence && event.order_id.value() == expected_order_id &&
           event.cancelled_quantity.units() == expected_cancelled_quantity;
}

bool expect_execution(boost::asio::ip::tcp::socket& sock,
                      std::uint64_t expected_sequence,
                      std::uint64_t expected_resting_order_id,
                      std::uint64_t expected_incoming_order_id,
                      std::int64_t expected_price,
                      std::uint64_t expected_quantity) {
    WireHeader header{};
    std::vector<std::byte> payload;
    if (!read_frame(sock, header, payload) || header.type != MessageType::execution ||
        (header.flags & kFlagMore) == 0) {
        return false;
    }

    ExecutionEvent event{
        .sequence = *SequenceNumber::from_value(1),
        .resting_order_id = *OrderId::from_value(1),
        .incoming_order_id = *OrderId::from_value(1),
        .price = *Price::from_ticks(1),
        .quantity = *Quantity::from_units(1),
    };
    return !decode_execution(payload, event).has_value() && event.sequence.value() == expected_sequence &&
           event.resting_order_id.value() == expected_resting_order_id &&
           event.incoming_order_id.value() == expected_incoming_order_id &&
           event.price.ticks() == expected_price && event.quantity.units() == expected_quantity;
}

bool expect_protocol_error_and_disconnect(boost::asio::ip::tcp::socket& sock,
                                          ErrorCode expected_code,
                                          MessageType expected_offending_type) {
    WireHeader header{};
    std::vector<std::byte> payload;
    if (!read_frame(sock, header, payload) || header.type != MessageType::protocol_error) {
        return false;
    }

    ProtocolErrorEvent event{};
    if (decode_protocol_error(payload, event).has_value() || event.error_code != expected_code ||
        event.offending_type != static_cast<std::uint8_t>(expected_offending_type)) {
        return false;
    }

    std::array<std::byte, 1> dummy{};
    boost::system::error_code ec;
    return sock.read_some(boost::asio::buffer(dummy), ec) == 0 && ec == boost::asio::error::eof;
}

}  // namespace

int main() {
    bool passed = true;

    // ==========================================
    // Test 1: MatchingEngineService direct queue test
    // ==========================================
    {
        InboundQueue inbound;
        OutboundQueue outbound;
        MatchingEngineService engine(inbound, outbound);

        // Submit Buy Order
        const NewOrder buy_order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(101),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(100),
            .limit_price = Price::from_ticks(5000),
        };
        passed &= test_util::check(inbound.try_push(InboundMessage{1, buy_order}), "push buy order to inbound");
        passed &= test_util::check(engine.process_available() == 1, "engine processed 1 message");

        OutboundMessage out_msg;
        passed &= test_util::check(outbound.try_pop(out_msg), "pop outbound response");
        passed &= test_util::check(out_msg.type == MessageType::order_accepted, "type is order_accepted");
        passed &= test_util::check(std::holds_alternative<OrderAcceptedEvent>(out_msg.payload),
                                   "payload is OrderAcceptedEvent");
        const auto& ack = std::get<OrderAcceptedEvent>(out_msg.payload);
        passed &= test_util::check(ack.sequence.value() == 1, "ack sequence matches");
        passed &= test_util::check(ack.order_id.value() == 101, "ack order_id matches");
        passed &= test_util::check(ack.remaining_quantity == 100, "remaining qty is 100");

        // Submit Crossing Sell Order (partial fill: 40 units)
        const NewOrder sell_order{
            .sequence = *SequenceNumber::from_value(2),
            .order_id = *OrderId::from_value(102),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(40),
            .limit_price = Price::from_ticks(5000),
        };
        passed &= test_util::check(inbound.try_push(InboundMessage{2, sell_order}), "push sell order to inbound");
        passed &= test_util::check(engine.process_available() == 1, "engine processed sell order");

        // Outbound should contain:
        // 1. ExecutionEvent to incoming trader (session 2) with kFlagMore
        // 2. ExecutionEvent to resting trader (session 1)
        // 3. OrderAcceptedEvent to incoming trader (session 2) with remaining 0
        OutboundMessage exec_incoming;
        passed &= test_util::check(outbound.try_pop(exec_incoming), "pop execution for incoming trader");
        passed &= test_util::check(exec_incoming.session_id == 2, "session_id is 2");
        passed &= test_util::check(exec_incoming.type == MessageType::execution, "type is execution");
        passed &= test_util::check((exec_incoming.flags & kFlagMore) != 0, "kFlagMore is set");
        const auto& exec_in_payload = std::get<ExecutionEvent>(exec_incoming.payload);
        passed &= test_util::check(exec_in_payload.quantity.units() == 40, "exec quantity is 40");
        passed &= test_util::check(exec_in_payload.price.ticks() == 5000, "exec price is 5000");

        OutboundMessage exec_resting;
        passed &= test_util::check(outbound.try_pop(exec_resting), "pop execution for resting trader");
        passed &= test_util::check(exec_resting.session_id == 1, "session_id is 1");
        passed &= test_util::check(exec_resting.type == MessageType::execution, "type is execution");

        OutboundMessage ack_sell;
        passed &= test_util::check(outbound.try_pop(ack_sell), "pop terminal ack for incoming trader");
        passed &= test_util::check(ack_sell.session_id == 2, "session_id is 2");
        passed &= test_util::check(ack_sell.type == MessageType::order_accepted, "type is order_accepted");
        const auto& ack_sell_payload = std::get<OrderAcceptedEvent>(ack_sell.payload);
        passed &= test_util::check(ack_sell_payload.remaining_quantity == 0, "remaining quantity is 0");
    }

    // ==========================================
    // Test 2: End-to-End TCP Client Connection & Execution
    // ==========================================
    {
        boost::asio::io_context io_ctx;
        InboundQueue inbound;
        OutboundQueue outbound;
        MatchingEngineService engine(inbound, outbound);
        TcpGateway gateway(io_ctx, 0, inbound, outbound);
        gateway.start();

        const std::uint16_t port = gateway.local_port();
        passed &= test_util::check(port > 0, "gateway assigned a valid ephemeral port");

        // Run io_context on a worker thread
        std::atomic<bool> running{true};
        std::thread io_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                io_ctx.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        // Run engine loop on a second thread
        std::thread engine_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                if (engine.process_available() > 0) {
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        });

        // Connect Client 1
        boost::asio::io_context client_ctx;
        boost::asio::ip::tcp::socket client1(client_ctx);
        boost::system::error_code conn_ec;
        client1.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port), conn_ec);
        if (conn_ec == boost::system::errc::operation_not_permitted) {
            std::cout << "Notice: loopback TCP not permitted by sandbox environment; skipping live socket tests.\n";

            running.store(false, std::memory_order_relaxed);
            io_thread.join();
            engine_thread.join();
            gateway.stop();
            return passed ? 0 : 1;
        }
        passed &= test_util::check(!conn_ec && client1.is_open(), "client 1 connected to gateway");

        // Client 1 sends Buy limit order (100 units @ 12,000 ticks)
        const NewOrder order1{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1001),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(100),
            .limit_price = Price::from_ticks(12000),
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kNewOrderSize> order1_payload{};
        passed &= test_util::check(encode_new_order(order1, order1_payload), "encode order1 payload");
        std::vector<std::byte> frame1(kHeaderSize + order1_payload.size());
        passed &= test_util::check(encode_frame(MessageType::new_order, order1_payload, frame1, 0), "encode frame1");

        passed &= test_util::check(write_all(client1, frame1), "client 1 sent new_order frame");

        // Client 1 reads response: order_accepted
        WireHeader resp_header{};
        std::vector<std::byte> resp_payload;
        passed &= test_util::check(read_frame(client1, resp_header, resp_payload), "client 1 received response");
        passed &= test_util::check(resp_header.type == MessageType::order_accepted, "client 1 got order_accepted");

        OrderAcceptedEvent ack1{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1),
            .remaining_quantity = 0,
        };
        passed &= test_util::check(!decode_order_accepted(resp_payload, ack1).has_value(), "ack1 decoded");
        passed &= test_util::check(ack1.sequence.value() == 1 && ack1.order_id.value() == 1001,
                                   "ack1 matches order1");
        passed &= test_util::check(ack1.remaining_quantity == 100, "ack1 remaining is 100");

        // Connect Client 2
        boost::asio::ip::tcp::socket client2(client_ctx);
        client2.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
        passed &= test_util::check(client2.is_open(), "client 2 connected to gateway");

        // Client 2 sends Crossing Sell limit order (60 units @ 12,000 ticks)
        const NewOrder order2{
            .sequence = *SequenceNumber::from_value(2),
            .order_id = *OrderId::from_value(2001),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(60),
            .limit_price = Price::from_ticks(12000),
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kNewOrderSize> order2_payload{};
        passed &= test_util::check(encode_new_order(order2, order2_payload), "encode order2 payload");
        std::vector<std::byte> frame2(kHeaderSize + order2_payload.size());
        passed &= test_util::check(encode_frame(MessageType::new_order, order2_payload, frame2, 0), "encode frame2");

        passed &= test_util::check(write_all(client2, frame2), "client 2 sent crossing sell order");

        // Client 2 should receive: 1. Execution, 2. Order Accepted (remaining = 0)
        WireHeader c2_h1{};
        std::vector<std::byte> c2_p1;
        passed &= test_util::check(read_frame(client2, c2_h1, c2_p1), "client 2 received first frame");
        passed &= test_util::check(c2_h1.type == MessageType::execution, "first frame is execution");
        passed &= test_util::check((c2_h1.flags & kFlagMore) != 0, "execution has kFlagMore");

        ExecutionEvent exec2{
            .sequence = *SequenceNumber::from_value(1),
            .resting_order_id = *OrderId::from_value(1),
            .incoming_order_id = *OrderId::from_value(1),
            .price = *Price::from_ticks(1),
            .quantity = *Quantity::from_units(1),
        };
        passed &= test_util::check(!decode_execution(c2_p1, exec2).has_value(), "exec2 decoded");
        passed &= test_util::check(exec2.resting_order_id.value() == 1001, "exec2 resting is 1001");
        passed &= test_util::check(exec2.incoming_order_id.value() == 2001, "exec2 incoming is 2001");
        passed &= test_util::check(exec2.quantity.units() == 60, "exec2 quantity is 60");
        passed &= test_util::check(exec2.price.ticks() == 12000, "exec2 price is 12000");

        WireHeader c2_h2{};
        std::vector<std::byte> c2_p2;
        passed &= test_util::check(read_frame(client2, c2_h2, c2_p2), "client 2 received second frame");
        passed &= test_util::check(c2_h2.type == MessageType::order_accepted, "second frame is order_accepted");

        // Meanwhile, resting Client 1 should also have received the Execution!
        WireHeader c1_exec_h{};
        std::vector<std::byte> c1_exec_p;
        passed &= test_util::check(read_frame(client1, c1_exec_h, c1_exec_p),
                                   "client 1 (resting) received execution notification");
        passed &= test_util::check(c1_exec_h.type == MessageType::execution, "resting notification is execution");

        ExecutionEvent exec1{
            .sequence = *SequenceNumber::from_value(1),
            .resting_order_id = *OrderId::from_value(1),
            .incoming_order_id = *OrderId::from_value(1),
            .price = *Price::from_ticks(1),
            .quantity = *Quantity::from_units(1),
        };
        passed &= test_util::check(!decode_execution(c1_exec_p, exec1).has_value(), "exec1 decoded");
        passed &= test_util::check(exec1.resting_order_id.value() == 1001 && exec1.quantity.units() == 60,
                                   "resting execution matches");

        // A session sequence must be strictly increasing.
        const NewOrder duplicate_sequence_order{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(1002),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(11900),
        };
        std::array<std::byte, low_latency_exchange::protocol::layout::kNewOrderSize> duplicate_payload{};
        passed &= test_util::check(encode_new_order(duplicate_sequence_order, duplicate_payload),
                                   "encode duplicate-sequence order");
        std::vector<std::byte> duplicate_frame(kHeaderSize + duplicate_payload.size());
        passed &= test_util::check(encode_frame(MessageType::new_order, duplicate_payload, duplicate_frame, 0),
                                   "encode duplicate-sequence frame");
        passed &= test_util::check(write_all(client1, duplicate_frame),
                                   "client 1 sent duplicate-sequence frame");

        WireHeader sequence_error_header{};
        std::vector<std::byte> sequence_error_payload;
        passed &= test_util::check(read_frame(client1, sequence_error_header, sequence_error_payload),
                                   "client 1 received sequence error");
        passed &= test_util::check(sequence_error_header.type == MessageType::protocol_error,
                                   "sequence violation is protocol_error");
        ProtocolErrorEvent sequence_error{};
        passed &= test_util::check(!decode_protocol_error(sequence_error_payload, sequence_error).has_value(),
                                   "sequence error decoded");
        passed &= test_util::check(sequence_error.error_code == ErrorCode::sequence_out_of_order,
                                   "error code is sequence_out_of_order");
        std::array<std::byte, 1> sequence_dummy{};
        boost::system::error_code sequence_ec;
        const std::size_t sequence_bytes =
            client1.read_some(boost::asio::buffer(sequence_dummy), sequence_ec);
        passed &= test_util::check(sequence_bytes == 0 && sequence_ec == boost::asio::error::eof,
                                   "server closed sequence-violating session");

        // Shutdown test threads
        running.store(false, std::memory_order_relaxed);
        io_thread.join();
        engine_thread.join();

        gateway.stop();
        boost::system::error_code ec;
        client1.close(ec);
        client2.close(ec);
    }

    // ==========================================
    // Test 3: Hostile & Malformed Input Handling over TCP
    // ==========================================
    {
        boost::asio::io_context io_ctx;
        InboundQueue inbound;
        OutboundQueue outbound;
        TcpGateway gateway(io_ctx, 0, inbound, outbound);
        gateway.start();

        const std::uint16_t port = gateway.local_port();

        std::atomic<bool> running{true};
        std::thread io_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                io_ctx.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        // 1. Hostile magic bytes -> malformed_frame error and disconnect
        {
            boost::asio::io_context client_ctx;
            boost::asio::ip::tcp::socket client(client_ctx);
            client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

            std::array<std::byte, kHeaderSize> bad_magic{};
            bad_magic.fill(std::byte{0xEE});
            passed &= test_util::check(write_all(client, bad_magic), "sent bad magic");

            WireHeader err_h{};
            std::vector<std::byte> err_p;
            passed &= test_util::check(read_frame(client, err_h, err_p), "read error frame from bad magic");
            passed &= test_util::check(err_h.type == MessageType::protocol_error, "response is protocol_error");

            ProtocolErrorEvent err_event{};
            passed &= test_util::check(!decode_protocol_error(err_p, err_event).has_value(), "protocol error decoded");
            passed &= test_util::check(err_event.error_code == ErrorCode::malformed_frame,
                                       "error code is malformed_frame");

            // Verify server closed the connection
            std::array<std::byte, 1> dummy{};
            boost::system::error_code ec;
            const std::size_t bytes = client.read_some(boost::asio::buffer(dummy), ec);
            passed &= test_util::check(bytes == 0 && ec == boost::asio::error::eof,
                                       "server closed connection after malformed_frame");
        }

        // 2. Oversized payload length (> 1024) -> payload_too_large and disconnect
        {
            boost::asio::io_context client_ctx;
            boost::asio::ip::tcp::socket client(client_ctx);
            client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

            // Craft header claiming 2000-byte payload
            const WireHeader oversize_hdr{
                .magic = low_latency_exchange::protocol::kMagic,
                .version = low_latency_exchange::protocol::kVersion,
                .type = MessageType::new_order,
                .flags = 0,
                .payload_length = 2000,
            };
            std::array<std::byte, kHeaderSize> hdr_buf{};
            passed &= test_util::check(low_latency_exchange::protocol::encode_header(oversize_hdr, hdr_buf),
                                       "encode oversize header");
            passed &= test_util::check(write_all(client, hdr_buf), "sent oversized header");

            WireHeader err_h{};
            std::vector<std::byte> err_p;
            passed &= test_util::check(read_frame(client, err_h, err_p), "read error frame from oversize length");
            passed &= test_util::check(err_h.type == MessageType::protocol_error, "response is protocol_error");

            ProtocolErrorEvent err_event{};
            passed &= test_util::check(!decode_protocol_error(err_p, err_event).has_value(), "protocol error decoded");
            passed &= test_util::check(err_event.error_code == ErrorCode::payload_too_large,
                                       "error code is payload_too_large");

            std::array<std::byte, 1> dummy{};
            boost::system::error_code ec;
            const std::size_t bytes = client.read_some(boost::asio::buffer(dummy), ec);
            passed &= test_util::check(bytes == 0 && ec == boost::asio::error::eof,
                                       "server closed connection after payload_too_large");
        }

        running.store(false, std::memory_order_relaxed);
        io_thread.join();
        gateway.stop();
    }

    // ==========================================
    // Test 4: Scripted TCP cancellation removes resting liquidity
    // ==========================================
    {
        boost::asio::io_context io_ctx;
        InboundQueue inbound;
        OutboundQueue outbound;
        MatchingEngineService engine(inbound, outbound);
        TcpGateway gateway(io_ctx, 0, inbound, outbound);
        gateway.start();

        std::atomic<bool> running{true};
        std::thread io_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                io_ctx.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        std::thread engine_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                if (engine.process_available() == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        });

        boost::asio::io_context client_ctx;
        boost::asio::ip::tcp::socket client(client_ctx);
        client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                       gateway.local_port()));

        const NewOrder resting_buy{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(4'001),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(20),
            .limit_price = Price::from_ticks(100),
        };
        passed &= test_util::check(send_new_order(client, resting_buy), "sent cancellable resting order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 1, 4'001, 20),
                                   "resting order accepted over TCP");

        const CancelOrder cancel{
            .sequence = *SequenceNumber::from_value(2),
            .order_id = *OrderId::from_value(4'001),
        };
        passed &= test_util::check(send_cancel_order(client, cancel), "sent cancel order over TCP");
        passed &= test_util::check(expect_cancel_accepted(client, 2, 4'001, 20),
                                   "cancel accepted with the resting quantity");

        const NewOrder sell_after_cancel{
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(4'002),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(20),
            .limit_price = Price::from_ticks(100),
        };
        passed &= test_util::check(send_new_order(client, sell_after_cancel), "sent sell after cancellation");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 3, 4'002, 20),
                                   "cancelled order no longer trades");

        running.store(false, std::memory_order_relaxed);
        io_thread.join();
        engine_thread.join();
        gateway.stop();
    }

    // ==========================================
    // Test 5: Scripted TCP replace priority and crossing flows
    // ==========================================
    {
        boost::asio::io_context io_ctx;
        InboundQueue inbound;
        OutboundQueue outbound;
        MatchingEngineService engine(inbound, outbound);
        TcpGateway gateway(io_ctx, 0, inbound, outbound);
        gateway.start();

        std::atomic<bool> running{true};
        std::thread io_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                io_ctx.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        std::thread engine_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                if (engine.process_available() == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        });

        boost::asio::io_context client_ctx;
        boost::asio::ip::tcp::socket client(client_ctx);
        client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                       gateway.local_port()));

        const NewOrder retained_first{
            .sequence = *SequenceNumber::from_value(1),
            .order_id = *OrderId::from_value(5'001),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(20),
            .limit_price = Price::from_ticks(100),
        };
        const NewOrder retained_second{
            .sequence = *SequenceNumber::from_value(2),
            .order_id = *OrderId::from_value(5'002),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(100),
        };
        passed &= test_util::check(send_new_order(client, retained_first), "sent first retained-priority order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 1, 5'001, 20),
                                   "first retained-priority order accepted");
        passed &= test_util::check(send_new_order(client, retained_second), "sent second retained-priority order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 2, 5'002, 10),
                                   "second retained-priority order accepted");

        const ReplaceOrder retained_replace{
            .sequence = *SequenceNumber::from_value(3),
            .order_id = *OrderId::from_value(5'001),
            .new_quantity = *Quantity::from_units(10),
            .new_price = *Price::from_ticks(100),
        };
        passed &= test_util::check(send_replace_order(client, retained_replace), "sent priority-retaining replace");
        passed &= test_util::check(expect_order_accepted(client, MessageType::replace_accepted, 3, 5'001, 10),
                                   "quantity reduction retains a resting order");

        const NewOrder retained_sell{
            .sequence = *SequenceNumber::from_value(4),
            .order_id = *OrderId::from_value(5'003),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(15),
            .limit_price = Price::from_ticks(100),
        };
        passed &= test_util::check(send_new_order(client, retained_sell), "sent sell against retained-priority orders");
        passed &= test_util::check(expect_execution(client, 4, 5'001, 5'003, 100, 10),
                                   "quantity decrease preserved first order priority");
        passed &= test_util::check(expect_execution(client, 4, 5'002, 5'003, 100, 5),
                                   "second order executed after retained-priority order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 4, 5'003, 0),
                                   "retained-priority crossing order completed");

        const NewOrder loss_first{
            .sequence = *SequenceNumber::from_value(5),
            .order_id = *OrderId::from_value(5'101),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(101),
        };
        const NewOrder loss_second{
            .sequence = *SequenceNumber::from_value(6),
            .order_id = *OrderId::from_value(5'102),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(101),
        };
        passed &= test_util::check(send_new_order(client, loss_first), "sent first priority-loss order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 5, 5'101, 10),
                                   "first priority-loss order accepted");
        passed &= test_util::check(send_new_order(client, loss_second), "sent second priority-loss order");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 6, 5'102, 10),
                                   "second priority-loss order accepted");

        const ReplaceOrder loss_replace{
            .sequence = *SequenceNumber::from_value(7),
            .order_id = *OrderId::from_value(5'101),
            .new_quantity = *Quantity::from_units(30),
            .new_price = *Price::from_ticks(101),
        };
        passed &= test_util::check(send_replace_order(client, loss_replace), "sent priority-losing replace");
        passed &= test_util::check(expect_order_accepted(client, MessageType::replace_accepted, 7, 5'101, 30),
                                   "quantity increase replaced order");

        const NewOrder loss_sell{
            .sequence = *SequenceNumber::from_value(8),
            .order_id = *OrderId::from_value(5'103),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(15),
            .limit_price = Price::from_ticks(101),
        };
        passed &= test_util::check(send_new_order(client, loss_sell), "sent sell against priority-losing replace");
        passed &= test_util::check(expect_execution(client, 8, 5'102, 5'103, 101, 10),
                                   "quantity increase moved the order behind its peer");
        passed &= test_util::check(expect_execution(client, 8, 5'101, 5'103, 101, 5),
                                   "priority-losing replacement traded second");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 8, 5'103, 0),
                                   "priority-loss crossing order completed");

        const NewOrder resting_ask{
            .sequence = *SequenceNumber::from_value(9),
            .order_id = *OrderId::from_value(5'201),
            .side = Side::sell,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(10),
            .limit_price = Price::from_ticks(103),
        };
        const NewOrder replaceable_buy{
            .sequence = *SequenceNumber::from_value(10),
            .order_id = *OrderId::from_value(5'202),
            .side = Side::buy,
            .type = OrderType::limit,
            .quantity = *Quantity::from_units(15),
            .limit_price = Price::from_ticks(101),
        };
        passed &= test_util::check(send_new_order(client, resting_ask), "sent resting ask for crossing replace");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 9, 5'201, 10),
                                   "resting ask accepted");
        passed &= test_util::check(send_new_order(client, replaceable_buy), "sent replaceable buy");
        passed &= test_util::check(expect_order_accepted(client, MessageType::order_accepted, 10, 5'202, 15),
                                   "replaceable buy accepted");

        const ReplaceOrder crossing_replace{
            .sequence = *SequenceNumber::from_value(11),
            .order_id = *OrderId::from_value(5'202),
            .new_quantity = *Quantity::from_units(15),
            .new_price = *Price::from_ticks(103),
        };
        passed &= test_util::check(send_replace_order(client, crossing_replace), "sent spread-crossing replace");
        passed &= test_util::check(expect_execution(client, 11, 5'201, 5'202, 103, 10),
                                   "replace price crossing the spread executed immediately");
        passed &= test_util::check(expect_order_accepted(client, MessageType::replace_accepted, 11, 5'202, 5),
                                   "crossing replace retained the residual quantity");

        running.store(false, std::memory_order_relaxed);
        io_thread.join();
        engine_thread.join();
        gateway.stop();
    }

    // ==========================================
    // Test 6: Bounded inbound queue overload disconnects the session
    // ==========================================
    {
        boost::asio::io_context io_ctx;
        InboundQueue inbound;
        OutboundQueue outbound;
        TcpGateway gateway(io_ctx, 0, inbound, outbound);
        gateway.start();

        std::atomic<bool> running{true};
        std::thread io_thread([&]() {
            while (running.load(std::memory_order_relaxed)) {
                io_ctx.poll();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        boost::asio::io_context client_ctx;
        boost::asio::ip::tcp::socket client(client_ctx);
        client.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                       gateway.local_port()));

        bool sent_all = true;
        for (std::uint64_t value = 1; value <= InboundQueue::capacity(); ++value) {
            const NewOrder order{
                .sequence = *SequenceNumber::from_value(value),
                .order_id = *OrderId::from_value(value),
                .side = Side::buy,
                .type = OrderType::limit,
                .quantity = *Quantity::from_units(1),
                .limit_price = Price::from_ticks(100),
            };
            sent_all = sent_all && send_new_order(client, order);
        }
        passed &= test_util::check(sent_all, "filled the inbound queue through the TCP gateway");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!inbound.full() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool queue_full = inbound.full();
        passed &= test_util::check(queue_full, "inbound queue became full without an engine consumer");

        if (queue_full) {
            const NewOrder overflow{
                .sequence = *SequenceNumber::from_value(InboundQueue::capacity() + 1),
                .order_id = *OrderId::from_value(InboundQueue::capacity() + 1),
                .side = Side::buy,
                .type = OrderType::limit,
                .quantity = *Quantity::from_units(1),
                .limit_price = Price::from_ticks(100),
            };
            passed &= test_util::check(send_new_order(client, overflow), "sent the overload-triggering command");
            passed &= test_util::check(
                expect_protocol_error_and_disconnect(client, ErrorCode::session_overloaded, MessageType::new_order),
                "gateway returned session_overloaded and disconnected");
        }

        running.store(false, std::memory_order_relaxed);
        io_thread.join();
        gateway.stop();
    }

    return passed ? 0 : 1;
}

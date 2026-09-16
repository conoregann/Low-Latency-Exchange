#include "low_latency_exchange/tcp_gateway.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace low_latency_exchange {

namespace {

std::vector<std::byte> serialize_outbound(const OutboundMessage& msg) {
    std::array<std::byte, protocol::kMaxPayloadLength> payload_buf{};
    std::size_t payload_size = 0;
    bool encoded = false;

    if (std::holds_alternative<protocol::OrderAcceptedEvent>(msg.payload)) {
        payload_size = protocol::layout::kOrderAcceptedSize;
        encoded = protocol::encode_order_accepted(std::get<protocol::OrderAcceptedEvent>(msg.payload),
                                                 std::span<std::byte>(payload_buf.data(), payload_size));
    } else if (std::holds_alternative<protocol::OrderRejectedEvent>(msg.payload)) {
        payload_size = protocol::layout::kOrderRejectedSize;
        encoded = protocol::encode_order_rejected(std::get<protocol::OrderRejectedEvent>(msg.payload),
                                                 std::span<std::byte>(payload_buf.data(), payload_size));
    } else if (std::holds_alternative<protocol::ExecutionEvent>(msg.payload)) {
        payload_size = protocol::layout::kExecutionSize;
        encoded = protocol::encode_execution(std::get<protocol::ExecutionEvent>(msg.payload),
                                            std::span<std::byte>(payload_buf.data(), payload_size));
    } else if (std::holds_alternative<protocol::CancelAcceptedEvent>(msg.payload)) {
        payload_size = protocol::layout::kCancelAcceptedSize;
        encoded = protocol::encode_cancel_accepted(std::get<protocol::CancelAcceptedEvent>(msg.payload),
                                                  std::span<std::byte>(payload_buf.data(), payload_size));
    } else if (std::holds_alternative<protocol::ProtocolErrorEvent>(msg.payload)) {
        payload_size = protocol::layout::kProtocolErrorSize;
        encoded = protocol::encode_protocol_error(std::get<protocol::ProtocolErrorEvent>(msg.payload),
                                                 std::span<std::byte>(payload_buf.data(), payload_size));
    }

    if (!encoded) {
        return {};
    }

    std::vector<std::byte> frame(protocol::kHeaderSize + payload_size);
    if (!protocol::encode_frame(msg.type,
                                std::span<const std::byte>(payload_buf.data(), payload_size),
                                frame,
                                msg.flags)) {
        return {};
    }
    return frame;
}

std::vector<std::byte> make_error_frame(protocol::ErrorCode code, std::uint8_t offending_type) {
    const protocol::ProtocolErrorEvent event{
        .error_code = code,
        .offending_type = offending_type,
    };
    std::array<std::byte, protocol::layout::kProtocolErrorSize> payload{};
    if (!protocol::encode_protocol_error(event, payload)) {
        return {};
    }
    std::vector<std::byte> frame(protocol::kHeaderSize + payload.size());
    if (!protocol::encode_frame(protocol::MessageType::protocol_error, payload, frame, 0)) {
        return {};
    }
    return frame;
}

}  // namespace

// ==========================================
// MatchingEngineService Implementation
// ==========================================

void MatchingEngineService::push_outbound(OutboundMessage&& msg) {
    while (!outbound_.try_push(std::move(msg))) {
        std::this_thread::yield();
    }
}

void MatchingEngineService::handle_new_order(std::uint64_t session_id, const NewOrder& order) {
    const auto result = book_.submit(order);

    if (result.rejection.has_value()) {
        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::order_rejected,
            .flags = 0,
            .payload = protocol::OrderRejectedEvent{
                .sequence = order.sequence,
                .order_id = order.order_id,
                .error_code = protocol::to_error_code(*result.rejection),
            },
        });
        return;
    }

    // Emit execution events (with more flag set)
    for (const auto& exec : result.executions) {
        const auto resting_session_it = order_sessions_.find(exec.resting_order_id);
        const std::uint64_t resting_session_id =
            (resting_session_it != order_sessions_.end()) ? resting_session_it->second : 0;

        if (!book_.contains(exec.resting_order_id)) {
            order_sessions_.erase(exec.resting_order_id);
        }

        const protocol::ExecutionEvent exec_event{
            .sequence = order.sequence,
            .resting_order_id = exec.resting_order_id,
            .incoming_order_id = exec.incoming_order_id,
            .price = exec.price,
            .quantity = exec.quantity,
        };

        // Send execution to incoming trader
        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::execution,
            .flags = protocol::kFlagMore,
            .payload = exec_event,
        });

        // Send execution to resting trader if distinct
        if (resting_session_id != 0 && resting_session_id != session_id) {
            push_outbound(OutboundMessage{
                .session_id = resting_session_id,
                .type = protocol::MessageType::execution,
                .flags = 0,
                .payload = exec_event,
            });
        }
    }

    // Terminal order_accepted event
    const std::uint64_t remaining_units = result.remaining_quantity ? result.remaining_quantity->units() : 0;
    push_outbound(OutboundMessage{
        .session_id = session_id,
        .type = protocol::MessageType::order_accepted,
        .flags = 0,
        .payload = protocol::OrderAcceptedEvent{
            .sequence = order.sequence,
            .order_id = order.order_id,
            .remaining_quantity = remaining_units,
        },
    });

    if (remaining_units > 0 && book_.contains(order.order_id)) {
        order_sessions_[order.order_id] = session_id;
    }
}

void MatchingEngineService::handle_cancel_order(std::uint64_t session_id, const CancelOrder& order) {
    const auto result = book_.cancel(order);

    if (result.accepted()) {
        order_sessions_.erase(order.order_id);
        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::cancel_accepted,
            .flags = 0,
            .payload = protocol::CancelAcceptedEvent{
                .sequence = order.sequence,
                .order_id = order.order_id,
                .cancelled_quantity = *result.cancelled_quantity,
            },
        });
    } else {
        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::cancel_rejected,
            .flags = 0,
            .payload = protocol::OrderRejectedEvent{
                .sequence = order.sequence,
                .order_id = order.order_id,
                .error_code = protocol::to_error_code(*result.rejection),
            },
        });
    }
}

void MatchingEngineService::handle_replace_order(std::uint64_t session_id, const ReplaceOrder& order) {
    const auto result = book_.replace(order);

    if (result.rejection.has_value()) {
        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::replace_rejected,
            .flags = 0,
            .payload = protocol::OrderRejectedEvent{
                .sequence = order.sequence,
                .order_id = order.order_id,
                .error_code = protocol::to_error_code(*result.rejection),
            },
        });
        return;
    }

    for (const auto& exec : result.executions) {
        const auto resting_session_it = order_sessions_.find(exec.resting_order_id);
        const std::uint64_t resting_session_id =
            (resting_session_it != order_sessions_.end()) ? resting_session_it->second : 0;

        if (!book_.contains(exec.resting_order_id)) {
            order_sessions_.erase(exec.resting_order_id);
        }

        const protocol::ExecutionEvent exec_event{
            .sequence = order.sequence,
            .resting_order_id = exec.resting_order_id,
            .incoming_order_id = exec.incoming_order_id,
            .price = exec.price,
            .quantity = exec.quantity,
        };

        push_outbound(OutboundMessage{
            .session_id = session_id,
            .type = protocol::MessageType::execution,
            .flags = protocol::kFlagMore,
            .payload = exec_event,
        });

        if (resting_session_id != 0 && resting_session_id != session_id) {
            push_outbound(OutboundMessage{
                .session_id = resting_session_id,
                .type = protocol::MessageType::execution,
                .flags = 0,
                .payload = exec_event,
            });
        }
    }

    const std::uint64_t remaining_units = result.remaining_quantity ? result.remaining_quantity->units() : 0;
    if (remaining_units > 0 && book_.contains(order.order_id)) {
        order_sessions_[order.order_id] = session_id;
    } else {
        order_sessions_.erase(order.order_id);
    }

    push_outbound(OutboundMessage{
        .session_id = session_id,
        .type = protocol::MessageType::replace_accepted,
        .flags = 0,
        .payload = protocol::OrderAcceptedEvent{
            .sequence = order.sequence,
            .order_id = order.order_id,
            .remaining_quantity = remaining_units,
        },
    });
}

bool MatchingEngineService::process_one() {
    InboundMessage msg;
    if (!inbound_.try_pop(msg)) {
        return false;
    }

    if (std::holds_alternative<NewOrder>(msg.command)) {
        handle_new_order(msg.session_id, std::get<NewOrder>(msg.command));
    } else if (std::holds_alternative<CancelOrder>(msg.command)) {
        handle_cancel_order(msg.session_id, std::get<CancelOrder>(msg.command));
    } else if (std::holds_alternative<ReplaceOrder>(msg.command)) {
        handle_replace_order(msg.session_id, std::get<ReplaceOrder>(msg.command));
    }
    return true;
}

std::size_t MatchingEngineService::process_available() {
    std::size_t count = 0;
    while (process_one()) {
        ++count;
    }
    return count;
}

// ==========================================
// TcpSession Implementation
// ==========================================

class TcpSession final : public std::enable_shared_from_this<TcpSession> {
  public:
    TcpSession(boost::asio::ip::tcp::socket socket,
               TcpGateway& gateway,
               std::uint64_t session_id)
        : socket_{std::move(socket)},
          gateway_{gateway},
          session_id_{session_id} {
        payload_buf_.resize(protocol::kMaxPayloadLength);
    }

    void start() {
        do_read_header();
    }

    void send_raw_frame(std::vector<std::byte> frame) {
        if (frame.empty() || !socket_.is_open()) {
            return;
        }
        const bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(std::move(frame));
        if (!write_in_progress) {
            do_write();
        }
    }

    void send_protocol_error(protocol::ErrorCode code, std::uint8_t offending_type, bool fatal = true) {
        if (fatal) {
            close_after_write_ = true;
        }
        auto frame = make_error_frame(code, offending_type);
        if (frame.empty()) {
            close();
            gateway_.unregister_session(session_id_);
            return;
        }
        send_raw_frame(std::move(frame));
    }

    void close() {
        boost::system::error_code ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

  private:
    bool enqueue_command(std::uint64_t sequence,
                         protocol::MessageType type,
                         InboundCommand command) {
        if (sequence <= last_sequence_) {
            send_protocol_error(protocol::ErrorCode::sequence_out_of_order,
                                static_cast<std::uint8_t>(type));
            return false;
        }
        if (!gateway_.inbound_.try_push(InboundMessage{session_id_, std::move(command)})) {
            send_protocol_error(protocol::ErrorCode::session_overloaded,
                                static_cast<std::uint8_t>(type));
            return false;
        }
        last_sequence_ = sequence;
        return true;
    }

    void do_read_header() {
        auto self = shared_from_this();
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(header_buf_),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    gateway_.unregister_session(session_id_);
                    return;
                }

                const auto header = protocol::decode_header(header_buf_);
                if (!header.has_value()) {
                    send_protocol_error(protocol::ErrorCode::malformed_frame, 0, true);
                    gateway_.unregister_session(session_id_);
                    return;
                }

                const auto val_err = protocol::validate_header(*header);
                if (val_err.has_value()) {
                    send_protocol_error(*val_err, static_cast<std::uint8_t>(header->type));
                    return;
                }

                do_read_payload(*header);
            });
    }

    void do_read_payload(const protocol::WireHeader& header) {
        auto self = shared_from_this();
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(payload_buf_.data(), header.payload_length),
            [this, self, header](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    gateway_.unregister_session(session_id_);
                    return;
                }

                const std::span<const std::byte> payload_slice(payload_buf_.data(), header.payload_length);

                switch (header.type) {
                    case protocol::MessageType::new_order: {
                        NewOrder order{
                            .sequence = *SequenceNumber::from_value(1),
                            .order_id = *OrderId::from_value(1),
                            .side = Side::buy,
                            .type = OrderType::limit,
                            .quantity = *Quantity::from_units(1),
                            .limit_price = std::nullopt,
                        };
                        const auto err = protocol::decode_new_order(payload_slice, order);
                        if (err.has_value()) {
                            send_protocol_error(*err, static_cast<std::uint8_t>(header.type));
                            return;
                        }
                        if (!enqueue_command(order.sequence.value(), header.type, std::move(order))) {
                            return;
                        }
                        break;
                    }
                    case protocol::MessageType::cancel_order: {
                        CancelOrder order{
                            .sequence = *SequenceNumber::from_value(1),
                            .order_id = *OrderId::from_value(1),
                        };
                        const auto err = protocol::decode_cancel_order(payload_slice, order);
                        if (err.has_value()) {
                            send_protocol_error(*err, static_cast<std::uint8_t>(header.type));
                            return;
                        }
                        if (!enqueue_command(order.sequence.value(), header.type, std::move(order))) {
                            return;
                        }
                        break;
                    }
                    case protocol::MessageType::replace_order: {
                        ReplaceOrder order{
                            .sequence = *SequenceNumber::from_value(1),
                            .order_id = *OrderId::from_value(1),
                            .new_quantity = *Quantity::from_units(1),
                            .new_price = *Price::from_ticks(1),
                        };
                        const auto err = protocol::decode_replace_order(payload_slice, order);
                        if (err.has_value()) {
                            send_protocol_error(*err, static_cast<std::uint8_t>(header.type));
                            return;
                        }
                        if (!enqueue_command(order.sequence.value(), header.type, std::move(order))) {
                            return;
                        }
                        break;
                    }
                    default: {
                        send_protocol_error(protocol::ErrorCode::unknown_message_type,
                                            static_cast<std::uint8_t>(header.type));
                        return;
                    }
                }

                do_read_header();
            });
    }

    void do_write() {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(write_queue_.front()),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    gateway_.unregister_session(session_id_);
                    return;
                }

                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    do_write();
                    return;
                }
                if (close_after_write_) {
                    close_after_write_ = false;
                    close();
                    gateway_.unregister_session(session_id_);
                }
            });
    }

    boost::asio::ip::tcp::socket socket_;
    TcpGateway& gateway_;
    std::uint64_t session_id_;
    std::array<std::byte, protocol::kHeaderSize> header_buf_{};
    std::vector<std::byte> payload_buf_{};
    std::deque<std::vector<std::byte>> write_queue_{};
    std::uint64_t last_sequence_{0};
    bool close_after_write_{false};
};

// ==========================================
// TcpGateway Implementation
// ==========================================

TcpGateway::TcpGateway(boost::asio::io_context& io_ctx,
                       std::uint16_t port,
                       InboundQueue& inbound,
                       OutboundQueue& outbound)
    : io_ctx_{io_ctx},
      acceptor_{io_ctx, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)},
      inbound_{inbound},
      outbound_{outbound},
      drain_timer_{io_ctx} {}

TcpGateway::~TcpGateway() {
    stop();
}

void TcpGateway::start() {
    if (!stopped_) {
        return;
    }
    stopped_ = false;
    do_accept();
    schedule_outbound_drain();
}

void TcpGateway::schedule_outbound_drain() {
    drain_timer_.expires_after(std::chrono::milliseconds(1));
    drain_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec && !stopped_) {
            poll_outbound();
            schedule_outbound_drain();
        }
    });
}

void TcpGateway::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    boost::system::error_code ec;
    drain_timer_.cancel();
    acceptor_.cancel(ec);
    acceptor_.close(ec);

    for (auto& [_, session] : sessions_) {
        session->close();
    }
    sessions_.clear();
}

std::uint16_t TcpGateway::local_port() const noexcept {
    boost::system::error_code ec;
    const auto endpoint = acceptor_.local_endpoint(ec);
    return ec ? 0 : endpoint.port();
}

std::size_t TcpGateway::active_session_count() const noexcept {
    return sessions_.size();
}

std::size_t TcpGateway::poll_outbound() {
    std::size_t count = 0;
    OutboundMessage msg;
    while (outbound_.try_pop(msg)) {
        ++count;
        auto frame = serialize_outbound(msg);

        if (msg.session_id == 0) {
            for (auto& [_, session] : sessions_) {
                session->send_raw_frame(frame);
            }
        } else {
            auto it = sessions_.find(msg.session_id);
            if (it != sessions_.end()) {
                it->second->send_raw_frame(std::move(frame));
            }
        }
    }
    return count;
}

void TcpGateway::do_accept() {
    acceptor_.async_accept([this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
        if (!ec) {
            const std::uint64_t session_id = next_session_id_++;
            auto session = std::make_shared<TcpSession>(std::move(socket), *this, session_id);
            sessions_.emplace(session_id, session);
            session->start();
        }

        if (!stopped_) {
            do_accept();
        }
    });
}

void TcpGateway::unregister_session(std::uint64_t session_id) {
    sessions_.erase(session_id);
}

}  // namespace low_latency_exchange

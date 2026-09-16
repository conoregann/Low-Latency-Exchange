#pragma once

#include <boost/asio.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "low_latency_exchange/order_book.hpp"
#include "low_latency_exchange/order_command.hpp"
#include "low_latency_exchange/protocol.hpp"
#include "low_latency_exchange/spsc_queue.hpp"
#include "low_latency_exchange/types.hpp"

namespace low_latency_exchange {

using InboundCommand = std::variant<std::monostate, NewOrder, CancelOrder, ReplaceOrder>;

struct InboundMessage {
    std::uint64_t session_id = 0;
    InboundCommand command = std::monostate{};
};

struct OutboundMessage {
    std::uint64_t session_id = 0;  // 0 means broadcast to all active sessions
    protocol::MessageType type = protocol::MessageType::order_accepted;
    std::uint16_t flags = 0;
    std::variant<std::monostate,
                 protocol::OrderAcceptedEvent,
                 protocol::OrderRejectedEvent,
                 protocol::ExecutionEvent,
                 protocol::CancelAcceptedEvent,
                 protocol::ProtocolErrorEvent>
        payload = std::monostate{};
};

using InboundQueue = BoundedSPSCQueue<InboundMessage, 4096>;
using OutboundQueue = BoundedSPSCQueue<OutboundMessage, 4096>;

/**
 * @brief Deterministic matching engine service operating as a single-writer.
 * Drains inbound commands, executes matches against the OrderBook, and pushes
 * outbound events to the outbound SPSC queue.
 */
class MatchingEngineService final {
  public:
    explicit MatchingEngineService(InboundQueue& inbound, OutboundQueue& outbound) noexcept
        : inbound_{inbound}, outbound_{outbound} {}

    bool process_one();
    std::size_t process_available();

    [[nodiscard]] OrderBook& order_book() noexcept {
        return book_;
    }
    [[nodiscard]] const OrderBook& order_book() const noexcept {
        return book_;
    }

  private:
    // Applies bounded outbound-queue backpressure; socket I/O remains outside the engine.
    void push_outbound(OutboundMessage&& msg);
    void handle_new_order(std::uint64_t session_id, const NewOrder& order);
    void handle_cancel_order(std::uint64_t session_id, const CancelOrder& order);
    void handle_replace_order(std::uint64_t session_id, const ReplaceOrder& order);

    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    OrderBook book_{};
    std::unordered_map<OrderId, std::uint64_t> order_sessions_{};
};

class TcpSession;

/**
 * @brief Asynchronous TCP Gateway using Boost.Asio.
 * Manages TCP client connections, decodes wire protocol frames, pushes
 * validated commands to the inbound SPSC queue, and writes matching engine
 * responses back to client sockets.
 */
class TcpGateway final {
  public:
    TcpGateway(boost::asio::io_context& io_ctx,
               std::uint16_t port,
               InboundQueue& inbound,
               OutboundQueue& outbound);
    ~TcpGateway();

    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;
    TcpGateway(TcpGateway&&) = delete;
    TcpGateway& operator=(TcpGateway&&) = delete;

    void start();
    void stop();

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::size_t active_session_count() const noexcept;
    [[nodiscard]] boost::asio::io_context& io_context() noexcept {
        return io_ctx_;
    }

    // Must be called from the io_context thread. start() calls this through a
    // periodic timer; the public method is useful for deterministic callers
    // that own the io_context loop.
    std::size_t poll_outbound();

  private:
    void do_accept();
    void unregister_session(std::uint64_t session_id);
    void schedule_outbound_drain();

    friend class TcpSession;

    boost::asio::io_context& io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    InboundQueue& inbound_;
    OutboundQueue& outbound_;
    std::uint64_t next_session_id_{1};
    std::unordered_map<std::uint64_t, std::shared_ptr<TcpSession>> sessions_{};
    boost::asio::steady_timer drain_timer_;
    bool stopped_{true};
};

}  // namespace low_latency_exchange

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace low_latency_exchange {

/**
 * @brief Lock-free, wait-free bounded Single-Producer Single-Consumer (SPSC) ring buffer queue.
 *
 * Designed for ultra-low latency exchange environments.
 * - Power-of-two capacity for fast bitwise modulo operations.
 * - Cacheline padding (64 bytes) to avoid false sharing between producer and consumer.
 * - Cached indices to minimize cross-core atomic traffic on hot paths.
 * - Fixed memory layout with zero runtime heap allocation.
 */
template <typename T, std::size_t Capacity = 1024>
class BoundedSPSCQueue final {
    static_assert((Capacity >= 2) && ((Capacity & (Capacity - 1)) == 0),
                  "BoundedSPSCQueue Capacity must be a power of two and at least 2");
    static_assert(std::is_default_constructible_v<T>,
                  "BoundedSPSCQueue element type must be default-constructible");

    static constexpr std::size_t kCacheLine = 64;
    static constexpr std::size_t kMask = Capacity - 1;

  public:
    using value_type = T;

    BoundedSPSCQueue() noexcept = default;
    ~BoundedSPSCQueue() = default;

    BoundedSPSCQueue(const BoundedSPSCQueue&) = delete;
    BoundedSPSCQueue& operator=(const BoundedSPSCQueue&) = delete;
    BoundedSPSCQueue(BoundedSPSCQueue&&) = delete;
    BoundedSPSCQueue& operator=(BoundedSPSCQueue&&) = delete;

    /**
     * @brief Attempt to enqueue an element (producer thread only).
     * @param item The value to push by const reference.
     * @return true if enqueued, false if queue is full.
     */
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::uint64_t current_write = write_index_.load(std::memory_order_relaxed);
        if (current_write - cached_read_index_ >= Capacity) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (current_write - cached_read_index_ >= Capacity) {
                return false;
            }
        }

        buffer_[current_write & kMask] = item;
        write_index_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Attempt to enqueue an element by move (producer thread only).
     * @param item The value to push by rvalue reference.
     * @return true if enqueued, false if queue is full.
     */
    [[nodiscard]] bool try_push(T&& item) noexcept {
        const std::uint64_t current_write = write_index_.load(std::memory_order_relaxed);
        if (current_write - cached_read_index_ >= Capacity) {
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (current_write - cached_read_index_ >= Capacity) {
                return false;
            }
        }

        buffer_[current_write & kMask] = std::move(item);
        write_index_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Attempt to dequeue an element (consumer thread only).
     * @param out The destination reference to receive the popped element.
     * @return true if dequeued, false if queue is empty.
     */
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t current_read = read_index_.load(std::memory_order_relaxed);
        if (current_read == cached_write_index_) {
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (current_read == cached_write_index_) {
                return false;
            }
        }

        out = std::move(buffer_[current_read & kMask]);
        read_index_.store(current_read + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Approximate number of items currently in the queue.
     */
    [[nodiscard]] std::size_t size() const noexcept {
        const std::uint64_t w = write_index_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_index_.load(std::memory_order_relaxed);
        return (w >= r) ? static_cast<std::size_t>(w - r) : 0;
    }

    /**
     * @brief Check whether the queue is currently empty.
     */
    [[nodiscard]] bool empty() const noexcept {
        return write_index_.load(std::memory_order_relaxed) == read_index_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Check whether the queue is currently full.
     */
    [[nodiscard]] bool full() const noexcept {
        const std::uint64_t w = write_index_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_index_.load(std::memory_order_relaxed);
        return (w - r) >= Capacity;
    }

    /**
     * @brief Fixed capacity of the queue.
     */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

  private:
    // Buffer storage
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};

    // Producer state on its own cacheline
    alignas(kCacheLine) std::atomic<std::uint64_t> write_index_{0};
    std::uint64_t cached_read_index_{0};

    // Consumer state on its own cacheline
    alignas(kCacheLine) std::atomic<std::uint64_t> read_index_{0};
    std::uint64_t cached_write_index_{0};
};

}  // namespace low_latency_exchange

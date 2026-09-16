#include "low_latency_exchange/spsc_queue.hpp"
#include "test_util.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using low_latency_exchange::BoundedSPSCQueue;

int main() {
    bool passed = true;

    // 1. Basic Single-Threaded FIFO and Empty/Full state
    {
        BoundedSPSCQueue<int, 4> queue;
        passed &= test_util::check(queue.empty(), "newly created queue is empty");
        passed &= test_util::check(!queue.full(), "newly created queue is not full");
        passed &= test_util::check(queue.size() == 0, "size is 0");
        passed &= test_util::check(queue.capacity() == 4, "capacity is 4");

        int val = 0;
        passed &= test_util::check(!queue.try_pop(val), "pop from empty queue returns false");

        passed &= test_util::check(queue.try_push(10), "push 10 succeeds");
        passed &= test_util::check(!queue.empty(), "queue is not empty");
        passed &= test_util::check(queue.size() == 1, "size is 1");

        passed &= test_util::check(queue.try_push(20), "push 20 succeeds");
        passed &= test_util::check(queue.try_push(30), "push 30 succeeds");
        passed &= test_util::check(queue.try_push(40), "push 40 succeeds");
        passed &= test_util::check(queue.full(), "queue is full after 4 pushes");
        passed &= test_util::check(queue.size() == 4, "size is 4");

        // Overflow attempt
        passed &= test_util::check(!queue.try_push(50), "push to full queue fails");
        passed &= test_util::check(queue.full(), "queue remains full");
        passed &= test_util::check(queue.size() == 4, "size remains 4");

        // Dequeue elements in FIFO order
        passed &= test_util::check(queue.try_pop(val) && val == 10, "pop item 1 is 10");
        passed &= test_util::check(!queue.full(), "queue is no longer full");
        passed &= test_util::check(queue.size() == 3, "size is 3");

        passed &= test_util::check(queue.try_pop(val) && val == 20, "pop item 2 is 20");
        passed &= test_util::check(queue.try_pop(val) && val == 30, "pop item 3 is 30");
        passed &= test_util::check(queue.try_pop(val) && val == 40, "pop item 4 is 40");
        passed &= test_util::check(queue.empty(), "queue is empty after popping all");
        passed &= test_util::check(queue.size() == 0, "size is 0");
        passed &= test_util::check(!queue.try_pop(val), "pop on emptied queue fails");
    }

    // 2. Wraparound single-threaded test
    {
        BoundedSPSCQueue<std::uint64_t, 4> queue;
        constexpr std::size_t kIterations = 10000;
        bool seq_ok = true;
        for (std::uint64_t i = 0; i < kIterations; ++i) {
            if (!queue.try_push(i)) {
                seq_ok = false;
                break;
            }
            std::uint64_t popped = 0;
            if (!queue.try_pop(popped) || popped != i) {
                seq_ok = false;
                break;
            }
        }
        passed &= test_util::check(seq_ok, "10000 push/pop cycles wrap around cleanly");
        passed &= test_util::check(queue.empty(), "queue is empty after wraparound cycles");
    }

    // 3. Move-only type support
    {
        BoundedSPSCQueue<std::unique_ptr<int>, 4> queue;
        passed &= test_util::check(queue.try_push(std::make_unique<int>(42)), "push unique_ptr rvalue succeeds");
        passed &= test_util::check(queue.try_push(std::make_unique<int>(84)), "push 2nd unique_ptr succeeds");

        std::unique_ptr<int> res;
        passed &= test_util::check(queue.try_pop(res) && res != nullptr && *res == 42, "popped 1st unique_ptr is 42");
        passed &= test_util::check(queue.try_pop(res) && res != nullptr && *res == 84, "popped 2nd unique_ptr is 84");
        passed &= test_util::check(queue.empty(), "queue is empty");
    }

    // 4. Cacheline alignment
    {
        passed &= test_util::check(alignof(BoundedSPSCQueue<int, 64>) >= 64,
                                   "BoundedSPSCQueue has >= 64-byte cacheline alignment");
    }

    // 5. Multi-threaded SPSC stress test
    {
        constexpr std::size_t kQueueCap = 1024;
        constexpr std::uint64_t kTotalMessages = 250000;

        auto queue = std::make_unique<BoundedSPSCQueue<std::uint64_t, kQueueCap>>();
        std::atomic<bool> producer_done{false};
        std::atomic<bool> consumer_ok{true};
        std::atomic<std::uint64_t> total_consumed{0};

        std::thread producer([&]() {
            for (std::uint64_t i = 1; i <= kTotalMessages; ++i) {
                while (!queue->try_push(i)) {
                    std::this_thread::yield();
                }
            }
            producer_done.store(true, std::memory_order_release);
        });

        std::thread consumer([&]() {
            std::uint64_t expected = 1;
            while (expected <= kTotalMessages) {
                std::uint64_t val = 0;
                if (queue->try_pop(val)) {
                    if (val != expected) {
                        consumer_ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                    ++expected;
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (producer_done.load(std::memory_order_acquire) && queue->empty()) {
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });

        producer.join();
        consumer.join();

        passed &= test_util::check(consumer_ok.load(), "all 250,000 messages arrived in exact strict monotonic order");
        passed &= test_util::check(total_consumed.load() == kTotalMessages, "all messages were received");
        passed &= test_util::check(queue->empty(), "queue empty after test");
    }

    return passed ? 0 : 1;
}

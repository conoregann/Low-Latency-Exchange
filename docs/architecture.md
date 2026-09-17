# Architecture

## Current Baseline: Phase 6 Observability and Performance Baselines

The matching core (`low_latency_exchange_core`) is an in-memory, deterministic limit order book implemented in modern C++20. It operates as a single-writer domain model free from external I/O, threads, or locks.

### 1. Strongly Typed Domain Primitives
Financial quantities and identifiers are encapsulated in non-zero, strongly typed value wrappers:
- `Price`: Integer tick representation (guaranteed $> 0$). Floating-point arithmetic is strictly avoided.
- `Quantity`: Non-zero unit counts (guaranteed $> 0$).
- `OrderId`: Unique 64-bit unsigned identifier.
- `SequenceNumber`: Monotonically increasing 64-bit sequence identifier.
- `Side` (`buy`, `sell`) and `OrderType` (`limit`, `market`).

### 2. Order Commands & Lifecycle
The engine processes three primary validated commands:
1. **`NewOrder`**:
   - Limit orders: validated to require a price; match immediately against resting contra liquidity in price-time priority; any unexecuted remainder rests on the book.
   - Market orders: validated without price; consume best available contra liquidity across levels; unexecuted remainder is cancelled (immediate-or-cancel) without posting.
2. **`CancelOrder`**:
   - Direct $O(1)$ cancellation of resting orders by `OrderId`.
3. **`ReplaceOrder`**:
   - In-place quantity reduction at the same price retains original time priority in the queue.
   - Price change or quantity increase forfeits time priority (moved to the queue tail with the replacement's sequence number).
   - Price modifications that cross the opposing spread trigger immediate execution against resting liquidity.

### 3. Data Structures & Lookup Indexing
- **Price Levels**: `std::map<Price, PriceLevel, std::greater<Price>>` for bids, and `std::map<Price, PriceLevel, std::less<Price>>` for asks.
- **Queue Representation**: Each `PriceLevel` maintains an intrusive/FIFO `std::list<RestingOrder>` and an aggregate `total_units` accumulator.
- **Order Locator Index**: `std::unordered_map<OrderId, OrderLocation>` stores the side, price, and stable list iterator for each resting order, enabling $O(1)$ cancellation and modifications without traversing price levels.
- **Level-2 Depth & Top-of-Book**: `top_bid()`, `top_ask()`, and `depth(max_levels)` query aggregated price levels directly in $O(K)$ time without scanning individual resting orders.

### 4. Financial & Structural Invariants
The core exposes `validate_invariants()` ensuring that between operations:
- **Spread Invariant**: `best_bid() < best_ask()` (crossed books cannot persist).
- **Conservation Invariant**: Sum of units in order queues equals the level's aggregated quantity and matches total resting volume.
- **Count Invariant**: `resting_order_count()` equals the order index size and total orders across all price levels.
- **Monotonicity & Non-Emptiness**: Maps maintain strict order; empty price levels are purged immediately.

## System Boundaries & Concurrency Model

```
load generator / replay client
            |
            v
  TCP gateway -> binary decoder -> bounded SPSC queue
                                      |
                                      v
                         single writer: matching engine
                                      |
                    +-----------------+-----------------+
                    v                                   v
         market-data publisher (SPSC)        append-only event logger
                    |                                   |
                    v                                   v
          subscribers / snapshot API              replay verifier
```

- **Matching Core**: Remains strictly single-threaded, synchronous, and side-effect free.
- **Transport & Gateway**: Encapsulates Boost.Asio networking and binary codec at the edges.
- **Inter-thread Boundary**: Single-Producer Single-Consumer (SPSC) lock-free bounded rings decouple networking and market data fan-out from the matching engine.

## Phase 2: Property, Differential, and Fuzz Testing Architecture

The matching core is validated by a multi-layered testing harness guaranteeing correctness, invariant preservation, and resistance to adversarial inputs:

### 1. Reference Order Book Oracle (`tests/reference_order_book.hpp`)
An independent, deliberately naive implementation of a limit order book. Instead of intrusive pointer indexes, it represents resting liquidity in flat sequences, sorting orders directly by the fundamental definitions of price priority and timestamp sequencing.

### 2. Differential Testing (`tests/differential_test.cpp`)
Drives identical randomized order flows concurrently into both `OrderBook` and `ReferenceOrderBook`, asserting 100% equivalence on:
- Execution prices, quantities, and counterparty order IDs.
- Order submission, cancellation, and cancel-replace status and residual volumes.
- Best bid/ask quotes and top-of-book levels.
- Full Level-2 aggregated depth snapshots.

### 3. Property-Based Testing & Volume Conservation (`tests/property_test.cpp`)
Simulates heavy order book churn across tens of thousands of operations, validating double-auction conservation on every single state transition:
$$\text{Submitted Units} = \text{Executed Units} + \text{Cancelled Units} + \text{Resting Units}$$
Concurrently asserts strict spread non-crossing ($\text{best\_bid} < \text{best\_ask}$) and resting order count invariants.

### 4. Hostile Input & Boundary Fuzzing (`tests/fuzz_test.cpp`)
Validates engine robustness against:
- Extremal numeric boundary limits (`Price(1)`, $\text{Price}(\text{INT64\_MAX})$, $\text{Quantity}(1)$, huge volume requests).
- Invalid command payload handling (missing prices, unsolicited prices, non-existent order cancellations).
- Deep single-price queue churn (1,000 orders in a single price level, arbitrary middle-of-queue node erasures).
- Multi-level aggressive liquidity sweeps across dozens of contiguous price levels.

## Phase 3: Binary Protocol and Local Gateway

Phase 3 starts with a frozen v1 wire contract so framing, versioning, and error codes are independent of sockets.

### 1. Protocol contract (`docs/protocol.md`, `protocol.hpp`)
- 8-byte little-endian header: magic `0x584C`, version, message type, flags, payload length.
- Fixed-size inbound commands (`new_order`, `cancel_order`, `replace_order`) and outbound events (acks, rejects, executions, protocol errors).
- Bounded payload length (1024) and reserved-bit rejection so hostile length claims cannot allocate.
- Header and payload codecs are bounds-checked and reject invalid fixed-size fields before commands reach the engine.

### 2. Local TCP gateway (`tcp_gateway.hpp`, `tcp_gateway.cpp`)
- Boost.Asio accepts local TCP clients and decodes v1 frames into the bounded inbound SPSC queue.
- The matching service remains the single writer for the order book and emits acknowledgements, rejects, and execution notifications through the outbound queue.
- Per-session sequences must increase strictly; queue overload and protocol errors produce an error frame and close the session after the frame is flushed.
- The gateway drains outbound events on its io_context thread, preserving socket write ordering while keeping socket I/O out of the matching core.

### 3. Gateway verification and local operation
- Scripted TCP integration coverage exercises new orders, acknowledgements, executions, cancellation, retained and lost replace priority, spread-crossing replacements, malformed frames, oversized lengths, sequence violations, and inbound-queue overload disconnects.
- The `low_latency_exchange --gateway [port]` executable starts the local server (default port `9000`) and runs the matching service as the queue consumer.
- The `sanitize` CMake preset and the CI `sanitize-gateway` job run the gateway suite under AddressSanitizer and UndefinedBehaviorSanitizer.

## Phase 4: Market-data pipeline

### Ownership and backpressure decision

The matching-engine thread is the only producer of market-data events. One publisher thread is the only consumer of the `MarketDataQueue`, a fixed-capacity (1024 event) SPSC ring. This ownership is deliberate: no socket, subscriber, or publisher work can block the single writer of the order book.

When the ring is full, `MarketDataFeed` drops the new market-data event, increments `dropped_events`, and records queue occupancy. It never spins, waits, allocates, or retries on the matching path. A drop marks the feed as needing a snapshot, so the next event successfully placed on the queue is a recovery snapshot rather than an incremental update.

### Event model and subscriber recovery

Each `MarketDataEvent` has a monotonic `FeedSequence`, an event kind, and fixed-size arrays for the best five bid and ask levels. Both snapshots and depth updates contain the full bounded depth image: updates therefore include the current best prices while allowing a client to reconstruct the complete published depth without variable-size allocation.

The first event is a snapshot. Subsequent commands emit sequenced depth updates, with periodic snapshots every 64 successful updates. `MarketDataBook` accepts a depth update only when its feed sequence is contiguous. A gap makes it unsynchronized until it receives the next snapshot, which resets the book to a known state.

`MarketDataMetrics` reports published events, dropped events, current queue occupancy, and high-water occupancy. The market-data test suite proves that a non-consuming publisher cannot stall matching, and that a snapshot followed by sequenced updates reconstructs the engine's published depth.

## Phase 5: Event capture and deterministic replay

Accepted commands leave the matching service through a second bounded SPSC queue, separate from market data. The engine is its only producer; a recorder thread is its only consumer. The recorder serializes each command as a fixed, checksummed log record, while the core remains free of filesystem handles and disk I/O.

Capture is deliberately best-effort under overload: queue-full increments a drop metric rather than blocking the matching thread. A run with any capture drop exits non-zero, making the log visibly unsuitable for recovery rather than silently presenting a partial history as authoritative.

`low_latency_exchange_replay` reads the log sequentially, validates every header, fixed payload size, checksum, and command encoding, then applies commands to a new `OrderBook`. The final state digest covers the ordered bid and ask levels plus FIFO order state. A matching digest between the live book and replay demonstrates deterministic recovery for the recorded command sequence. The precise binary contract and failure categories are in [event_log.md](event_log.md).

## Phase 6: Observability and performance work

`MatchingEngineService` owns fixed-memory telemetry on the same single-writer
thread that owns the book: accepted/rejected commands, accepted/rejected
cancels, executions, protocol bytes, inbound/outbound queue high-water marks,
and command-enqueue-to-terminal-response latency buckets. The metrics are
snapshotted after the engine stops; they deliberately do not introduce a lock or
shared mutable monitoring state into the matching path.

`low_latency_exchange_benchmark` uses a seedable synthetic flow of resting,
crossing, and cancel commands. Core mode times direct matching across configurable
independent books. Gateway mode separately times a loopback TCP client from
write through terminal response. The harness reports throughput and logarithmic
p50/p99/p99.9 latency upper bounds, and documents the exact seed and shape used.

The first measurement identified the gateway's 1 ms periodic outbound drain as
the end-to-end latency floor. The interval is now 50 microseconds; this improves
loopback acknowledgement latency without modifying matching. The measured
before/after evidence and its idle-wakeup tradeoff are recorded in
[benchmarks.md](benchmarks.md).

## Next Steps
- **Phase 7**: Add the portfolio-oriented architecture diagram, terminal demo,
  threat/failure-mode summary, and release checklist.

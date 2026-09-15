# Architecture

## Current Baseline: Phase 1 Deterministic Matching Core

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
- Header encode/decode is bounds-checked; payload codecs and the TCP gateway remain subsequent components.

## Next Steps
- **Phase 3 remaining**: bounds-checked payload encode/decode, then a local Boost.Asio TCP gateway with ordered enqueue and explicit overload policy.
- **Phase 4**: Market-data publisher pipeline with bounded lock-free SPSC queues and Level-2 snapshot recovery.

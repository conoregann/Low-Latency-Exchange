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

## Next Steps (Phases 2 & 3)
- Phase 2: Property-based testing over random command streams, differential testing against a simplified reference book, and fuzzing.
- Phase 3: Binary protocol framing, length-checked encoding/decoding, and local TCP gateway.

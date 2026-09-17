# Active Execution Plan

## State

The deterministic order book, v1 binary codecs, bounded SPSC queues, and local
Boost.Asio TCP gateway exist. The gateway validates headers and payloads,
enforces per-session sequence monotonicity, and flushes protocol errors before
closing. Phase 3 is complete: scripted TCP coverage verifies cancel, replace,
and overload behavior, and the local gateway has a documented executable
workflow. Phase 4 is complete: the engine publishes bounded, recoverable
five-level market-data updates without allowing a slow publisher to stall
matching. Phase 5 is complete: accepted commands are captured outside the
matching path in a bounded queue, written as framed checksummed records, and
replayed into a fresh deterministic book.

Guardrails: [AGENTS.md](../AGENTS.md), [architecture](../docs/architecture.md),
and the normative [protocol](../docs/protocol.md).

## Phase 3: local gateway complete

- [x] Add a scripted TCP cancel flow: submit a resting order, cancel it, verify
  `cancel_accepted`, then verify the order no longer trades -> verify:
  `ctest --preset debug -R low_latency_exchange.gateway`
- [x] Add scripted TCP replace coverage for retained priority, priority loss,
  and a replacement that crosses the spread -> verify:
  `ctest --preset debug -R low_latency_exchange.gateway`
- [x] Specify and test bounded inbound-queue overload: fill the queue without
  an engine consumer, send one frame, receive `session_overloaded`, and observe
  disconnect without a hang or unbounded allocation -> verify:
  `ctest --preset debug -R low_latency_exchange.gateway`
- [x] Run the gateway suite under ASan/UBSan after the new paths are added ->
  verify: `ctest --preset sanitize -R low_latency_exchange.gateway`
- [x] Document the scripted local gateway invocation once an application entry
  point exists -> verify:
  `rg -n "gateway|TCP" README.md docs/architecture.md`

## Phase 4: market-data pipeline complete

- [x] Record an ADR-level ownership and backpressure decision: engine is the
  sole producer; one publisher is the sole consumer; queue-full policy is
  measured and cannot stall matching -> verify:
  `rg -n "producer|consumer|backpressure|queue" docs/architecture.md`
- [x] Define fixed-size incremental best-price/depth and snapshot event types,
  including a monotonically increasing feed sequence -> verify:
  `cmake --build --preset debug --parallel`
- [x] Add an engine-to-publisher bounded SPSC queue with explicit capacity and
  no hot-path allocation -> verify:
  `ctest --preset debug -R low_latency_exchange.market_data`
- [x] Add publisher tests proving a slow subscriber cannot stall matching and a
  snapshot plus sequenced deltas reconstructs book state -> verify:
  `ctest --preset debug -R "low_latency_exchange\.(market_data|gateway|spsc_queue)"`
- [x] Run property, differential, protocol, queue, and publisher-adjacent tests
  after integration -> verify: `ctest --preset debug`

## Phase 5: event capture and replay complete

- [x] Define a versioned fixed-frame event-log contract with payload bounds and
  checksums -> verify: `rg -n "checksum|Record format|truncated" docs/event_log.md`
- [x] Capture only accepted commands through a bounded engine-to-recorder SPSC
  queue so disk I/O cannot stall matching -> verify:
  `ctest --preset debug -R low_latency_exchange.event_log`
- [x] Add a replay executable that builds a fresh order book and reports its
  deterministic final-state digest -> verify:
  `./out/build/debug/low_latency_exchange_replay`
- [x] Test live/replay digest equality plus checksum-corruption, truncation, and
  capture-overload failure behavior -> verify:
  `ctest --preset debug -R low_latency_exchange.event_log`
- [x] Run replay and gateway critical paths under ASan/UBSan -> verify:
  `ctest --preset sanitize -R "low_latency_exchange\\.(gateway|event_log)"`

## Phase 6: observability and performance complete

- [x] Expose engine-owned counters for outcomes, executions, bytes, queue
  high-water marks, and fixed-memory command-to-ack latency buckets -> verify:
  `ctest --preset debug -R low_latency_exchange.observability`
- [x] Build a seedable workload generator with configurable book shape, cancel
  rate, crossing rate, and independent core symbols -> verify:
  `ctest --preset debug -R low_latency_exchange.benchmark`
- [x] Measure matching core separately from loopback TCP gateway latency and
  document reproducible Release commands -> verify:
  `rg -n "--mode core|--mode gateway|p99.9" docs/benchmarks.md`
- [x] Record before/after evidence for the outbound-drain scheduling change ->
  verify: `rg -n "1 ms|50 us|Evidence-backed" docs/benchmarks.md`
- [x] Preserve correctness after the instrumentation and scheduling change ->
  verify: `ctest --preset debug`

## Completion discipline

Check off an item only after its verification command succeeds from a clean
build. Keep each completed phase in a separate, reviewable commit series unless
an interface change requires an atomic migration.

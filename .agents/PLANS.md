# Active Execution Plan

## State

The deterministic order book, v1 binary codecs, bounded SPSC queues, and local
Boost.Asio TCP gateway exist. The gateway validates headers and payloads,
enforces per-session sequence monotonicity, and flushes protocol errors before
closing. Phase 3 remains active until cancel/replace and overload behavior are
fully covered through TCP. Phase 4 has not started.

Guardrails: [AGENTS.md](../AGENTS.md), [architecture](../docs/architecture.md),
and the normative [protocol](../docs/protocol.md).

## Phase 3: finish local gateway

- [ ] Add a scripted TCP cancel flow: submit a resting order, cancel it, verify
  `cancel_accepted`, then verify the order no longer trades -> verify:
  `ctest --test-dir out/build/debug --output-on-failure -R low_latency_exchange.gateway`
- [ ] Add scripted TCP replace coverage for retained priority, priority loss,
  and a replacement that crosses the spread -> verify:
  `ctest --test-dir out/build/debug --output-on-failure -R low_latency_exchange.gateway`
- [ ] Specify and test bounded inbound-queue overload: fill the queue without
  an engine consumer, send one frame, receive `session_overloaded`, and observe
  disconnect without a hang or unbounded allocation -> verify:
  `ctest --test-dir out/build/debug --output-on-failure -R low_latency_exchange.gateway`
- [ ] Run the gateway suite under ASan/UBSan after the new paths are added ->
  verify: `ctest --test-dir out/build/sanitize --output-on-failure -R low_latency_exchange.gateway`
- [ ] Document the scripted local gateway invocation once an application entry
  point exists; do not claim it before then -> verify:
  `rg -n "gateway|TCP" README.md docs/architecture.md`

## Phase 4: prepare market-data pipeline

- [ ] Record an ADR-level ownership and backpressure decision: engine is the
  sole producer; one publisher is the sole consumer; queue-full policy is
  measured and cannot stall matching -> verify:
  `rg -n "producer|consumer|backpressure|queue" docs/architecture.md`
- [ ] Define fixed-size incremental best-price/depth and snapshot event types,
  including a monotonically increasing feed sequence -> verify:
  `cmake --build --preset debug --parallel`
- [ ] Add an engine-to-publisher bounded SPSC queue with explicit capacity and
  no hot-path allocation -> verify:
  `ctest --test-dir out/build/debug --output-on-failure -R low_latency_exchange.spsc_queue`
- [ ] Add publisher tests proving a slow subscriber cannot stall matching and a
  snapshot plus sequenced deltas reconstructs book state -> verify:
  `ctest --test-dir out/build/debug --output-on-failure -R "low_latency_exchange\.(gateway|spsc_queue)"`
- [ ] Run property, differential, protocol, queue, and publisher-adjacent tests
  after integration -> verify: `ctest --preset debug`

## Completion discipline

Check off an item only after its verification command succeeds from a clean
build. Keep Phase 4 changes in a separate commit series from Phase 3 gateway
completion unless an interface change requires an atomic migration.

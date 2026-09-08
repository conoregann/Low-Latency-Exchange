# PulseBook: C++ Low-Latency Exchange and Market-Data Platform

## Project decision

Build **PulseBook**, a production-minded, in-memory limit-order-book exchange simulator in modern C++. It accepts binary order messages, matches orders deterministically, publishes market-data events, records/replays sessions, and reports latency distributions under load.

This is deliberately more substantial than a concurrent TCP server. The centre of the project is the matching engine and its correctness/performance constraints; networking is a controlled boundary around it. The result demonstrates systems programming, careful data ownership, concurrency design, measurement discipline, and market-microstructure literacy - all directly relevant to Citadel Securities.

## Why this is the right next project

The current portfolio already proves full-stack delivery, cloud-backed services, distributed data, and introductory C++/Boost.Asio networking. The largest missing signal is performance-sensitive native systems work beyond a request/response server. PulseBook closes that gap through:

- C++20 ownership, RAII, data layout, integer/fixed-point modelling, and cache-aware structures.
- Deterministic price-time-priority matching and auditable financial invariants.
- A single-writer matching core plus bounded lock-free queues with an explicit concurrency model.
- Binary wire parsing, sequencing, market-data fan-out, durable event capture, and replay.
- Reproducible benchmarks reporting p50/p99/p99.9 latency and throughput.
- Sanitizer, unit, property, differential, fuzz, and load testing evidence.

## Scope and boundaries

PulseBook simulates one or more instruments in a single process initially, then exposes a local TCP gateway. It is a research/learning system only: no brokerage integration, real money, investment advice, or claims of exchange-grade compliance.

### End-state capabilities

1. Submit, cancel, and replace limit/market orders with explicit sequence numbers.
2. Maintain price-time priority and publish executions, acknowledgements, rejects, and book updates.
3. Serve level-1/level-2 snapshots and incremental market-data messages.
4. Persist an append-only binary event log and deterministically replay it into the same final book.
5. Drive realistic synthetic order flow and produce a repeatable performance report.

### Architecture

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

Keep the matching core free of sockets, disk I/O, logging, and locks. It must be deterministic and testable from an in-process command stream. Isolate all I/O at the edges.

## Proposed repository layout

    include/pulsebook/        public value types and component interfaces
    src/core/                 order book, matching, sequencing, invariants
    src/transport/            binary codec and TCP gateway
    src/feed/                 book-update aggregation and publishing
    src/persistence/          event-log writer and replay reader
    apps/gateway/             local executable
    apps/loadgen/             deterministic workload generator
    apps/replay/              log inspection and replay verifier
    benchmarks/               microbenchmarks and scenario benchmarks
    tests/                    unit, property, integration, and regression tests
    docs/                     protocol, architecture decisions, benchmark reports

## Engineering decisions to make visible

- Model Price, Quantity, OrderId, and Sequence as strong integer types; never use floating point for prices.
- Preallocate hot-path storage and use explicit memory resources/pools where profiling justifies it.
- Represent the book with a price-indexed/ordered level structure and intrusive FIFO at each price. Record the data-structure choice and benchmark it against at least one alternative.
- Start with a single-writer engine. Use SPSC bounded rings only between stages; document queue ownership and back-pressure policy. Do not add threads merely to look concurrent.
- Define a versioned, length-checked little-endian binary protocol. Reject malformed and out-of-sequence input safely.
- Measure warmed-up, CPU-pinned (where available) runs and state machine/compiler/build settings.
- Use CMake presets; build Debug with AddressSanitizer/UndefinedBehaviorSanitizer and Release with warnings-as-errors.

## Implementation plan

### Phase 0 - Repository and quality baseline (1-2 days)

- Add CMake, clang-format, clang-tidy configuration, strict warnings, and reproducible presets.
- Add a small dependency strategy (for example FetchContent for GoogleTest/Google Benchmark, pinned by revision) and a GitHub Actions build/test workflow.
- Add docs/architecture.md, docs/protocol.md, and initial architecture-decision records.

**Done when:** macOS/Linux Debug and Release builds work from documented commands; a sanitizer test job runs; formatting and tests are enforced locally and in CI.

### Phase 1 - Correct deterministic matching core (4-6 days)

- Implement strong domain types, order validation, and an order state machine.
- Implement add, cancel, and replace for a single instrument.
- Implement price-time priority: an incoming order consumes the best executable resting liquidity and emits one execution per match.
- Maintain top-of-book and depth snapshots without scanning all orders on every request.

**Done when:** examples cover partial fills, multiple fills, price improvement, FIFO within a price level, cancellation, replace semantics, and invalid commands. Invariants confirm no quantity is created or lost and no order appears twice.

### Phase 2 - Testing for failure modes, not just happy paths (2-3 days)

- Write property tests over random command streams: total executed quantity is conserved; crossed books cannot remain after matching; sequence/order IDs remain unique.
- Add a simple reference implementation used only in tests and differential-test the optimised book against it.
- Fuzz the command decoder and regression-test every failure found.

**Done when:** deterministic seeds reproduce failures, corpus cases are committed, and sanitizers pass.

### Phase 3 - Binary protocol and local gateway (3-4 days)

- Specify compact command/event headers, versioning, lengths, flags, and error codes.
- Implement bounds-checked encode/decode routines before adding sockets.
- Build a small Boost.Asio TCP gateway that assigns commands to the matching-core queue; preserve client order and make overload behavior explicit (bounded queue plus reject/disconnect policy).

**Done when:** a client can submit a scripted session through TCP and receive acknowledgements/executions; malformed packets and oversized lengths are rejected without a crash or allocation bomb.

### Phase 4 - Market-data pipeline (3-4 days)

- Publish incremental best-price/depth events and periodic snapshots from engine output.
- Implement a bounded SPSC ring between the engine and publisher, with metrics for queue occupancy and dropped/rejected work.
- Add subscriber recovery: a snapshot followed by sequenced incremental updates.

**Done when:** a slow subscriber cannot stall matching; a client can reconstruct the same book from a snapshot plus updates.

### Phase 5 - Event capture and deterministic replay (2-3 days)

- Append accepted commands and/or engine events to a binary log with framing and checksums.
- Build a replay executable that reconstructs the final book and checks a recorded digest.
- Add a deliberately interrupted/truncated-log test case.

**Done when:** the replayed state digest equals the live-run digest for a documented workload, and corrupt or truncated logs fail clearly.

### Phase 6 - Observability and performance work (4-5 days)

- Expose counters: accepted/rejected/cancelled orders, executions, queue high-water marks, bytes, and command-to-acknowledgement latency histograms.
- Build a deterministic load generator with configurable symbol count, order mix, cancel rate, and book-shape distribution.
- Benchmark the matching core separately from end-to-end gateway latency. Profile first, then make one or two evidence-backed optimisations, such as allocation reduction or improved price-level lookup.

**Done when:** docs/benchmarks.md contains reproducible commands, hardware/build details, workload parameters, throughput, p50/p99/p99.9 latency, and before/after evidence for each optimisation.

### Phase 7 - Portfolio polish (1-2 days)

- Create an architecture diagram, a 90-second terminal demo, a concise README, and a threat/failure-mode section.
- Explain every performance claim with command output and configuration; omit vanity throughput figures.
- Tag a stable release only after clean CI, sanitizer, replay, and load tests.

**Done when:** a recruiter can understand the system in two minutes and an engineer can build, test, run, and reproduce the benchmark from the repository alone.

## Suggested 5-week cadence

| Week | Focus | Visible checkpoint |
| --- | --- | --- |
| 1 | Phases 0-1 | Correct in-process matching engine and test suite |
| 2 | Phase 2 | Property/differential tests and decoder fuzzing |
| 3 | Phases 3-4 | TCP order flow plus resilient market-data feed |
| 4 | Phase 5 | Event log and deterministic replay verifier |
| 5 | Phases 6-7 | Reproducible benchmarks, documentation, demo, release |

## Acceptance metrics

Use measured values, not targets invented in advance. The portfolio release should include:

- 100% passing unit/property/integration tests and clean ASan/UBSan runs.
- A replay test proving live and replayed book digests match for a recorded scenario.
- A clearly documented p50/p99/p99.9 latency and throughput result for both core and end-to-end paths.
- A soak/load test that reaches a predeclared command volume without invariant violations, leaks, or unbounded queue growth.
- At least two documented design trade-offs and one profiler-driven optimisation.

## Resume entry after completion

Use only metrics the benchmark report proves. A strong replacement for the current C++ server line could read:

**PulseBook - Low-Latency Exchange & Market-Data Platform** | C++20, Boost.Asio, CMake, GoogleTest, Google Benchmark

- Engineered a deterministic, price-time-priority limit-order-book matching engine with a versioned binary order protocol, supporting add, cancel, replace, execution, and level-2 market-data events.
- Designed a single-writer hot path with bounded lock-free SPSC queues, preallocated order storage, and event-log replay; verified correctness using property, differential, fuzz, and sanitizer testing.
- Built a reproducible load harness reporting [measured throughput] and [measured p99.9 latency] under a documented synthetic order-flow workload; used profiling to reduce [measured bottleneck].

Do not keep the bracketed wording in the final résumé; replace it with verifiable measurements from docs/benchmarks.md.

## Learning order

1. Modern C++ ownership, RAII, value types, and testing.
2. Data-oriented order-book representation and algorithmic complexity.
3. Thread ownership, memory ordering, and bounded queues.
4. Binary parsing and hostile-input handling.
5. Profiling, latency percentiles, cache effects, and evidence-led optimisation.
6. Protocol evolution, event sourcing, replay, and operational failure modes.

This order matters: a correct deterministic engine is more impressive, and easier to optimise, than a prematurely multi-threaded one.


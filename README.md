# Low-Latency Exchange

Low-Latency Exchange is a C++20 learning project for building a deterministic, in-memory limit-order-book exchange and market-data platform. It is designed to demonstrate correct matching, clear concurrency boundaries, resilient binary protocol handling, replayability, and measurable performance.

The project is a simulator for engineering practice, not a real trading system or investment product. The intended architecture and delivery plan are in [ACTION_PLAN.md](ACTION_PLAN.md).

## Repository layout

- `include/low_latency_exchange/` - public C++ headers.
- `src/` - implementation of the matching and supporting components.
- `apps/` - local executables, starting with the command-line entry point.
- `tests/` - automated unit, property, integration, and regression tests.
- `docs/` - architecture, protocol, and benchmark documentation.
- `.github/workflows/` - continuous-integration workflows.

## Build and test

Requirements: CMake 3.25+, a C++20 compiler, Boost 1.70+ (Asio), and Unix Makefiles (included with the macOS Command Line Tools and common Linux build toolchains).

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the current executable with:

```sh
./out/build/debug/low_latency_exchange
```

Use `cmake --preset release` and `cmake --build --preset release` for an optimised build.

## Local TCP gateway

Start the version-1 binary gateway on the default local port (`9000`):

```sh
./out/build/debug/low_latency_exchange --gateway
```

Pass an explicit TCP port when running multiple local instances or a scripted client:

```sh
./out/build/debug/low_latency_exchange --gateway 9100
```

The process listens on IPv4 and shuts down cleanly on `Ctrl-C`. Client frames and the overload/disconnect policy are specified in [docs/protocol.md](docs/protocol.md). The gateway test suite runs scripted new, cancel, replace, malformed-frame, and bounded-queue overload sessions:

```sh
ctest --preset debug -R low_latency_exchange.gateway
```

Run the same integration coverage with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake --preset sanitize
cmake --build --preset sanitize --parallel
ctest --preset sanitize -R low_latency_exchange.gateway
```

## Market-data pipeline

The matching engine produces fixed-size five-level depth updates through a bounded SPSC queue. The local publisher consumes that queue independently of matching and maintains recoverable subscriber state. A queue-full condition is counted and drops only market-data work; it never blocks the matching engine. The next queued event after a drop is a complete snapshot, allowing a subscriber to recover from a sequence gap.

```sh
ctest --preset debug -R low_latency_exchange.market_data
```

## Event capture and deterministic replay

The optional Phase 5 recorder writes accepted commands to a framed, checksummed
append-only log. File I/O runs on a recorder thread behind a bounded queue, so
the matching engine does not wait for disk. Start a gateway with an explicit
port and log path:

```sh
./out/build/debug/low_latency_exchange --gateway 9000 /tmp/orders.lxlg
```

After the gateway exits cleanly, replay the file into a fresh order book:

```sh
./out/build/debug/low_latency_exchange_replay /tmp/orders.lxlg
```

Replay prints the accepted-command count and final state digest. It fails with
a specific error for a malformed, corrupt, truncated, or non-replayable log.
The log record contract and capture-overload policy are specified in
[docs/event_log.md](docs/event_log.md). Run the focused coverage with:

```sh
ctest --preset debug -R low_latency_exchange.event_log
```

## Benchmarks and observability

Phase 6 adds a deterministic synthetic workload generator and separate matching
and loopback-TCP gateway benchmarks. The engine exposes fixed-memory counters
for command outcomes, executions, wire bytes, queue high-water marks, and
command-to-terminal-response latency buckets.

```sh
cmake --preset release
cmake --build --preset release --parallel

./out/build/release/low_latency_exchange_benchmark --mode core \
  --commands 100000 --warmup 10000 --symbols 1 --levels 32 \
  --cancel-rate 20 --cross-rate 10 --seed 6006

./out/build/release/low_latency_exchange_benchmark --mode gateway \
  --commands 1000 --symbols 1 --levels 32 \
  --cancel-rate 20 --cross-rate 10 --seed 6006
```

The workload seed and parameters make runs repeatable. Gateway v1 does not
carry an instrument identifier, so gateway mode supports one symbol; core mode
can run independent books for multiple symbols. See [docs/benchmarks.md](docs/benchmarks.md)
for methodology, measured results, and the documented timer optimisation.

## Development rules

- Keep the matching core deterministic and independent of sockets, disk I/O, logging, and locks.
- Use fixed-point/integer representations for financial quantities; never floating-point prices.
- Add a test with every behavioural change and measure performance before claiming an optimisation.
- Keep commits small, buildable, and named for the change they introduce.

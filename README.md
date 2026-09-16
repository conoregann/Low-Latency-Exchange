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

## Development rules

- Keep the matching core deterministic and independent of sockets, disk I/O, logging, and locks.
- Use fixed-point/integer representations for financial quantities; never floating-point prices.
- Add a test with every behavioural change and measure performance before claiming an optimisation.
- Keep commits small, buildable, and named for the change they introduce.

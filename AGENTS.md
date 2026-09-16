# Low-Latency Exchange Agent Guide

## Operating posture

Think before coding. Read the affected interface, implementation, tests, and
`docs/protocol.md` before proposing a change. State assumptions when the task
does not settle an architectural choice.

Prefer the simplest design that preserves the established architecture. Make
surgical changes: do not refactor unrelated code, rename stable protocol
symbols, or widen a change scope without explicit approval. Work toward the
requested, testable outcome; stop when it is met.

## Non-negotiable system rules

- Use C++20 and keep warnings clean with warnings-as-errors enabled.
- Prices, quantities, order IDs, and sequence numbers are strong integer types.
  Never add floating-point prices, quantities, matching, or protocol fields.
- `OrderBook` is deterministic and single-writer. It owns price-time priority,
  book invariants, and matching state; it contains no threads, locks, sockets,
  disk I/O, logging, or raw wire parsing.
- The core must preserve uncrossed books, quantity conservation, unique order
  identity, and FIFO within a price level. Add or update a test whenever a
  behavior or invariant changes.
- Keep sockets and Boost.Asio at the transport boundary. Decode and validate a
  full frame before placing a domain command on a bounded queue.
- Preserve per-session input order and strictly increasing sequence numbers.
  Reject malformed, oversized, out-of-order, or overloaded input through the
  protocol contract; never crash or allocate from an untrusted length.
- Hot paths must not allocate. Do not add `new`, `delete`, heap-growing
  containers, formatting, exceptions, virtual dispatch, locks, or blocking I/O
  to matching, queue, or per-event paths. If an allocation is unavoidable,
  isolate it at an edge and document why.
- Bounded queues require explicit ownership, capacity, and overflow behavior.
  Do not turn an SPSC queue into MPSC/MPMC use or add a consumer/producer
  without an architectural decision and tests.

## Layout boundaries

| Path | Responsibility |
| --- | --- |
| `include/low_latency_exchange/` | Public domain, protocol, queue, and transport interfaces. |
| `src/order_book.cpp` | Single-writer matching implementation only. |
| `src/protocol.cpp` | Bounds-checked v1 codec implementation. |
| `src/tcp_gateway.cpp` | Boost.Asio transport, session validation, and queue handoff. |
| `tests/` | Unit, property, differential, fuzz, and TCP integration coverage. |
| `docs/` | Normative architecture and protocol decisions. |

The wire contract in `docs/protocol.md` is normative. Update it before or with
any intentional compatibility change; do not silently reinterpret v1 bytes.

## Commands

```sh
# Debug build and complete test suite
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

# Targeted protocol, gateway, and fuzz checks
ctest --test-dir out/build/debug --output-on-failure \
  -R "low_latency_exchange\.(protocol|gateway|fuzz)"

# ASan + UBSan build and suite
cmake -S . -B out/build/sanitize -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build out/build/sanitize --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir out/build/sanitize --output-on-failure

# Fuzz regression target under the sanitizer build
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir out/build/sanitize --output-on-failure \
  -R "low_latency_exchange\.fuzz"
```

On macOS, use the repository preset or pass the active Xcode SDK as
`-DCMAKE_OSX_SYSROOT=...` if the compiler cannot find a compatible SDK.

## Before handing off

Run the narrowest relevant tests, then the full suite for core, protocol, queue,
or transport changes. Inspect `git diff --check` and preserve unrelated working
tree changes. See [.agents/PLANS.md](.agents/PLANS.md) for active work and:

- [.agents/skills/run-sanitizers/SKILL.md](.agents/skills/run-sanitizers/SKILL.md)
- [.agents/skills/verify-wire-protocol/SKILL.md](.agents/skills/verify-wire-protocol/SKILL.md)

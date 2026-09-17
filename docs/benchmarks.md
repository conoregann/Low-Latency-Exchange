# Benchmark methodology and baseline

## Scope

The benchmark executable separates two different questions:

1. **Core matching** measures a deterministic command stream applied directly to
   independent `OrderBook` instances. It excludes queues, sockets, serialization,
   logging, and disk I/O.
2. **Gateway loopback** measures synchronous client-write to terminal-response
   latency through the local TCP gateway, inbound/outbound queues, matching
   service, protocol codec, and loopback socket.

The two figures must not be compared directly. The core number is a matching
baseline; the gateway number includes transport and scheduling work deliberately
outside the matching core.

## Reproduction

```sh
cmake --preset release
cmake --build --preset release --parallel

./out/build/release/low_latency_exchange_benchmark \
  --mode core --commands 100000 --warmup 10000 --symbols 1 --levels 32 \
  --cancel-rate 20 --cross-rate 10 --seed 6006

./out/build/release/low_latency_exchange_benchmark \
  --mode gateway --commands 1000 --symbols 1 --levels 32 \
  --cancel-rate 20 --cross-rate 10 --seed 6006
```

The generator is deterministic: the seed, command count, book-level count,
crossing percentage, and cancel percentage completely define its stream. Core
mode accepts multiple independent symbols to model shard-local books. Gateway
mode currently requires one symbol because protocol v1 has no instrument field.

For stable comparisons, repeat the command several times on an otherwise idle
machine. This portable harness does not pin CPUs on macOS; report the machine,
compiler, and command line alongside any claimed number.

## Recorded baseline

Recorded 2026-09-17 on macOS with Apple Clang 21.0.0, CMake `release` preset,
and the commands above. The sandbox did not permit querying the CPU model, so
it is intentionally not inferred here. These are local development measurements,
not exchange-grade claims.

| Mode | Commands/s | p50 | p99 | p99.9 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Core matching | 4,908,367 | <= 64 ns | <= 512 ns | <= 2,048 ns | 483,459 ns |
| Loopback TCP gateway | 5,280 | <= 131,072 ns | <= 262,144 ns | <= 262,144 ns | 440,750 ns |

Core latency percentiles are logarithmic histogram upper bounds; the maximum is
the exact largest observation. This avoids per-command measurement allocation
in the benchmark loop while making the resolution explicit.

The gateway run processed 974 accepted commands, 26 rejected cancels, and 119
executions. It recorded 43,224 inbound bytes, 37,712 outbound bytes, inbound
queue high-water of 1, and outbound queue high-water of 5.

## Evidence-backed optimisation

The pre-change gateway used a fixed 1 ms outbound-queue timer. The initial
measurement showed acknowledgement latency clustered at that cadence, so the
timer—not matching—was setting the end-to-end tail latency floor. The timer is
now 50 microseconds.

| Gateway setting | Commands/s | p50 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: |
| 1 ms outbound drain | 958 | <= 524,288 ns | <= 1,048,576 ns | <= 1,048,576 ns |
| 50 us outbound drain | 5,280 | <= 131,072 ns | <= 262,144 ns | <= 262,144 ns |

This reduces scheduling delay without altering the single-writer matching path.
The explicit tradeoff is more frequent wakeups when the gateway is idle. A later
production-oriented phase should replace periodic polling with a safe wakeup
strategy and profile its CPU cost before making another latency claim.

## Counters and interpretation

`MatchingEngineService::metrics()` exposes accepted/rejected commands,
accepted/rejected cancels, executions, inbound/outbound protocol bytes, queue
high-water marks, and a command-enqueue-to-terminal-response latency histogram.
The counters are owned by the matching-engine thread and are read after it stops
in the benchmark; they are not a cross-thread monitoring API. This preserves
the hot-path ownership model and avoids introducing locks into matching.

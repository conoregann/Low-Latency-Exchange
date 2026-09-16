---
name: verify-wire-protocol
description: Verify Low-Latency Exchange v1 packet, codec, frame, and gateway changes against the normative binary wire protocol.
---

# Verify wire protocol

Use this skill for any change to a message type, payload layout, codec, frame
validation, error path, or TCP session behavior. `docs/protocol.md` is the v1
source of truth; `protocol.hpp` and `protocol.cpp` must implement it exactly.

## Review sequence

1. Read the relevant sections of `docs/protocol.md`, then inspect the matching
   constants, codecs, and tests in `include/low_latency_exchange/protocol.hpp`,
   `src/protocol.cpp`, and `tests/protocol_test.cpp`.
2. Classify the change.
   - Existing v1 behavior: preserve byte layout and error-code meanings.
   - Intentional protocol evolution: update `docs/protocol.md` and introduce a
     new version or message type; never silently reinterpret a v1 field.
3. Check every changed frame against this contract:
   - Header is exactly 8 bytes: little-endian `magic` (`0x584C`), v1 version,
     type, flags, and payload length.
   - All multi-byte fields are explicitly little-endian; no struct casts or
     layout-dependent `memcpy` is permitted.
   - Known v1 payloads have their exact fixed size. Reject size mismatches with
     `payload_length_invalid`; reject claims above 1024 before allocating.
   - Reserved flag bits and payload bytes are zero. Validate identifiers,
     quantities, prices, side, and order type before creating domain objects.
   - Inbound types are only `new_order`, `cancel_order`, and `replace_order`;
     outbound acknowledgements, rejects, executions, and errors use the
     documented payload types and `more` semantics.
   - Use the documented error codes. A protocol error is emitted before the
     session closes; malformed frames never reach `OrderBook`.
   - Per-connection sequences are strictly increasing and queue overload maps
     to `session_overloaded` with the documented disconnect policy.
4. Add boundary tests for every changed branch: short/wrong-length payload,
   unknown type, reserved data, invalid field, and the successful round trip.

## Verification

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --test-dir out/build/debug --output-on-failure \
  -R "low_latency_exchange\.(protocol|gateway|fuzz)"
```

For codec or frame-parser changes, also run the sanitizer skill. Hand off the
specific protocol sections checked, test command output, and any compatibility
decision.

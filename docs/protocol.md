# Binary Wire Protocol v1

Low-Latency Exchange speaks a versioned, length-checked, little-endian binary protocol over a byte stream (initially TCP). This document is the wire contract for protocol version **1**. Encode/decode code must match these layouts exactly.

The matching engine never sees raw bytes. The codec translates frames into domain commands (`NewOrder`, `CancelOrder`, `ReplaceOrder`) and engine results into events. Invalid frames are rejected at the codec/gateway boundary.

## Framing

Every message is `header || payload`.

- Integers are **little-endian**.
- Multi-byte fields are not aligned beyond their natural widths; decoders must not `memcpy` structs onto the wire.
- `payload_length` is the size of the payload only. The on-wire size of a message is `8 + payload_length`.
- Receivers **must** read the 8-byte header first, then exactly `payload_length` payload bytes.
- If `payload_length` exceeds `max_payload_length` (1024), the peer is protocol-hostile: do not allocate the claimed payload. Emit `payload_too_large` and close the session.
- Version 1 payloads are **fixed length**. A known type with the wrong length is `payload_length_invalid`.
- Reserved bytes and reserved flag bits **must** be zero. Non-zero reserved fields are `invalid_field`.
- There is no compression, encryption, or checksum in v1. Integrity is a later persistence concern.

### Header (8 bytes)

| Offset | Size | Type | Field | Notes |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | `u16` | `magic` | Must be `0x584C` (`'L'`, `'X'` on the wire). |
| 2 | 1 | `u8` | `version` | Must be `1`. |
| 3 | 1 | `u8` | `type` | `MessageType`. |
| 4 | 2 | `u16` | `flags` | Bitset; unknown bits must be zero. |
| 6 | 2 | `u16` | `payload_length` | Bytes following the header. |

Magic `0x584C` is stored little-endian, so the first two bytes on the wire are `4C 58`.

### Flags

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `more` | More events follow for the same inbound command (for example several executions, then an ack). |
| 1–15 | reserved | Must be 0 in v1. |

A command that produces N executions plus one terminal ack/reject sets `more` on every event except the last.

## Message types

Inbound (client → gateway) values occupy `1..15`. Outbound (gateway → client) values occupy `16..31`. Unknown types are `unknown_message_type`.

| Value | Name | Direction | Payload length |
| ---: | --- | --- | ---: |
| 1 | `new_order` | inbound | 40 |
| 2 | `cancel_order` | inbound | 16 |
| 3 | `replace_order` | inbound | 32 |
| 16 | `order_accepted` | outbound | 24 |
| 17 | `order_rejected` | outbound | 24 |
| 18 | `execution` | outbound | 40 |
| 19 | `cancel_accepted` | outbound | 24 |
| 20 | `cancel_rejected` | outbound | 24 |
| 21 | `replace_accepted` | outbound | 24 |
| 22 | `replace_rejected` | outbound | 24 |
| 23 | `protocol_error` | outbound | 8 |

## Domain field encoding

Identifiers and sizes match the in-process domain types:

| Field | Wire type | Domain rule |
| --- | --- | --- |
| `sequence` | `u64` | Must be `> 0` (maps to `SequenceNumber`). |
| `order_id` | `u64` | Must be `> 0` (maps to `OrderId`). |
| `quantity` | `u64` | Must be `> 0` (maps to `Quantity`). |
| `price_ticks` | `i64` | Must be `> 0` when a price is present. `0` means “no price” (market new-order only). Negative values are `invalid_field`. |
| `side` | `u8` | `0` = buy, `1` = sell. Other values are `invalid_field`. |
| `order_type` | `u8` | `0` = limit, `1` = market. Other values are `invalid_field`. |

Limit new-orders require `price_ticks > 0`. Market new-orders require `price_ticks == 0`. These map to `NewOrderValidationError` and become `limit_order_missing_price` / `market_order_has_price`.

## Inbound payloads

### `new_order` (40 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` |
| 16 | 8 | `u64` | `quantity` |
| 24 | 8 | `i64` | `price_ticks` |
| 32 | 1 | `u8` | `side` |
| 33 | 1 | `u8` | `order_type` |
| 34 | 6 | `u8[6]` | `reserved` (zero) |

### `cancel_order` (16 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` |

### `replace_order` (32 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` |
| 16 | 8 | `u64` | `new_quantity` |
| 24 | 8 | `i64` | `new_price_ticks` (must be `> 0`) |

## Outbound payloads

`sequence` on outbound messages is the inbound command sequence that produced the event.

### `order_accepted` / `replace_accepted` (24 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` |
| 16 | 8 | `u64` | `remaining_quantity` (`0` if none rests) |

`remaining_quantity == 0` is valid on the wire even though `Quantity` is non-zero in the engine: it means “no residual resting size”.

### `order_rejected` / `cancel_rejected` / `replace_rejected` (24 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` (`0` if the id could not be parsed) |
| 16 | 2 | `u16` | `error_code` |
| 18 | 6 | `u8[6]` | `reserved` (zero) |

### `execution` (40 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `resting_order_id` |
| 16 | 8 | `u64` | `incoming_order_id` |
| 24 | 8 | `i64` | `price_ticks` |
| 32 | 8 | `u64` | `quantity` |

One `execution` event is emitted per match, matching the engine’s one-execution-per-fill rule.

### `cancel_accepted` (24 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 8 | `u64` | `sequence` |
| 8 | 8 | `u64` | `order_id` |
| 16 | 8 | `u64` | `cancelled_quantity` |

### `protocol_error` (8 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 2 | `u16` | `error_code` |
| 2 | 1 | `u8` | `offending_type` (`0` if the type byte was unreadable) |
| 3 | 5 | `u8[5]` | `reserved` (zero) |

After `protocol_error`, the session is closed. Do not continue parsing the stream: length claims may be hostile.

## Error codes

Values are stable. `0` is not used on the wire.

### Framing and codec (`1–99`)

| Code | Name | When |
| ---: | --- | --- |
| 1 | `malformed_frame` | Header truncated or not decodable as bytes. |
| 2 | `unsupported_version` | `version != 1`. |
| 3 | `unknown_message_type` | `type` is not in the v1 table. |
| 4 | `payload_length_invalid` | Length disagrees with the fixed size for `type`. |
| 5 | `payload_too_large` | `payload_length > 1024`. |
| 6 | `invalid_field` | Reserved bits/bytes non-zero, illegal enum, negative price, or zero identifier/quantity where forbidden. |
| 7 | `limit_order_missing_price` | Limit `new_order` with `price_ticks == 0`. |
| 8 | `market_order_has_price` | Market `new_order` with `price_ticks != 0`. |

### Matching (`100–199`)

| Code | Name | Engine source |
| ---: | --- | --- |
| 100 | `invalid_order` | `OrderRejectReason::invalid_order` (should be rare if codec already mapped 7/8). |
| 101 | `duplicate_order_id` | `OrderRejectReason::duplicate_order_id`. |
| 102 | `order_not_found` | Cancel/replace of an unknown id. |

### Session (`200–299`, reserved for the gateway)

| Code | Name | When |
| ---: | --- | --- |
| 200 | `session_overloaded` | Bounded inbound queue is full; reject or disconnect per gateway policy. |
| 201 | `sequence_out_of_order` | Client sequence did not increase as required by the session. |

Gateway sequence policy (to be enforced when the TCP session exists): inbound `sequence` values are unique and strictly increasing per connection.

## Session behaviour (normative for later gateway work)

1. Preserve client send order: decode, enqueue, and apply commands FIFO per connection.
2. Overload: the inbound queue is bounded. When it cannot accept the next decoded command, respond with `protocol_error` / reject `session_overloaded` and disconnect; never block the matching thread on socket I/O.
3. A single inbound command may produce multiple outbound events. `more` marks the non-terminal events.
4. Malformed or oversized frames never reach the matching engine.

## Versioning

- `version` is a single byte in the header. v1 implementations reject any other value with `unsupported_version`.
- New optional fields require a new version or a new message type. v1 parsers must not ignore trailing payload bytes.
- Changing magic, header size, endianness, or the meaning of an existing error code requires a new protocol version.

## Implementation mapping

C++ constants, enumerations, payload sizes, and header encode/decode live in `include/low_latency_exchange/protocol.hpp`. Payload codecs and the TCP gateway are subsequent Phase 3 components.

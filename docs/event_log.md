# Event Capture Log, Version 1

## Purpose and scope

The event log is an append-only audit input for deterministic recovery. It records
only commands that the matching engine accepted. A replay starts with an empty
`OrderBook`, applies each recorded command in order, and reports a deterministic
digest of the resulting resting state.

This is a local recovery format, not the TCP wire protocol. Its command payloads
reuse the validated v1 protocol encodings so a command has one canonical binary
representation at both boundaries.

## Record format

The file is a concatenation of independently framed records. All multi-byte
integers are little-endian.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | Magic `0x474C584C` (`LXLG`) |
| 4 | 1 | Format version (`1`) |
| 5 | 1 | Inbound command type: new, cancel, or replace |
| 6 | 2 | Fixed payload length |
| 8 | 8 | FNV-1a 64 checksum |
| 16 | N | Validated protocol command payload |

The checksum covers the record version, command type, two-byte payload length,
and payload. It does not cover the magic number or the stored checksum itself.
The payload length must exactly match the v1 fixed length for its command type;
there is no variable-length record or allocation based on a log-provided size.

## Capture ownership and backpressure

The matching-engine thread is the sole producer for a 4,096-entry SPSC capture
queue. A dedicated recorder thread is its sole consumer and performs all file
I/O. Consequently, opening, appending, flushing, and a slow disk cannot block
the matching engine or enter `OrderBook`.

If the queue is full, the engine drops the capture record and increments a
metric; it never waits or retries. The gateway reports a non-zero exit status
when capture drops occurred, because a partial log must not be treated as a
recovery source. Operators should increase recorder capacity or reduce load
before relying on a cleanly terminated log.

## Replay validity and failure modes

Replay fails closed at the first bad record and reports one of: I/O failure,
truncated header, malformed header, truncated payload, checksum mismatch,
invalid command, or a command rejected by the fresh order book. A rejected
replay command signals that the file is not a complete, ordered accepted-command
history for this engine version.

The state digest is a stable FNV-1a hash over book side, ordered price levels,
aggregate quantities, FIFO resting orders, and total resting count. It is a
diagnostic equivalence check, not a cryptographic integrity guarantee.

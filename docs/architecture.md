# Architecture

## Current baseline

The repository starts with one small C++20 library (`pulsebook_core`), a command-line executable, and a smoke test. This is intentionally modest: it establishes a reproducible build/test path before the matching domain is designed.

## Intended boundaries

The future matching engine owns order-book state and runs as a deterministic, single-writer core. Transport, market-data publishing, and event logging remain outside that core and communicate through explicit command/event boundaries. This prevents socket or subscriber behaviour from changing matching semantics.

## Near-term design work

Before adding the engine implementation, specify the value types, command state machine, price-time-priority invariants, and event model. See [ACTION_PLAN.md](../ACTION_PLAN.md) for the staged plan and acceptance criteria.

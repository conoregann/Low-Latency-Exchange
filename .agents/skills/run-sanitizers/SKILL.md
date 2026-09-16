---
name: run-sanitizers
description: Build and run Low-Latency Exchange tests with AddressSanitizer and UndefinedBehaviorSanitizer after core, queue, protocol, or gateway changes.
---

# Run sanitizers

Use this skill after changes to matching, queues, protocol codecs, gateway
sessions, or memory ownership. It validates memory and undefined-behavior
failures; it does not replace functional tests or performance measurement.

## Procedure

1. Start from the repository root. Preserve unrelated working-tree changes.
2. Configure the repository's Debug sanitizer preset:

   ```sh
   cmake --preset sanitize
   cmake --build --preset sanitize --parallel
   ```

   The preset configures a compatible SDK on macOS and enables ASan and UBSan.

3. Run the unit, property, differential, protocol, queue, gateway, and fuzz
   tests under strict sanitizer behavior:

   ```sh
   ctest --preset sanitize
   ```

4. Re-run the highest-risk targets individually when a failure needs isolation:

   ```sh
   ctest --preset sanitize \
     -R "low_latency_exchange\.(differential|fuzz|gateway)"
   ```

   The preset requests fail-fast ASan/UBSan diagnostics. It does not force
   `detect_leaks=1`, which AppleClang does not support on this project host.

## Triage

- Treat any ASan/UBSan report as a failure. Keep the smallest reproducing test,
  fix the root cause, then rerun the full sanitizer suite.
- Do not suppress a report, disable a sanitizer, or lower queue contention to
  hide a failure.
- For a gateway-only issue, verify the malformed-frame and queue-overload paths
  as well as the happy path.
- Report the exact configure command, test command, and final pass/fail result.

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
2. Configure a fresh Debug sanitizer build:

   ```sh
   cmake -S . -B out/build/sanitize -G "Unix Makefiles" \
     -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
     -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
   cmake --build out/build/sanitize --parallel
   ```

   On macOS, pass a compatible `-DCMAKE_OSX_SYSROOT=...` when required by the
   active compiler.

3. Run the unit, property, differential, protocol, queue, gateway, and fuzz
   tests under strict sanitizer behavior:

   ```sh
   ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
   UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
   ctest --test-dir out/build/sanitize --output-on-failure
   ```

4. Re-run the highest-risk targets individually when a failure needs isolation:

   ```sh
   ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
   UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
   ctest --test-dir out/build/sanitize --output-on-failure \
     -R "low_latency_exchange\.(differential|fuzz|gateway)"
   ```

## Triage

- Treat any ASan/UBSan report as a failure. Keep the smallest reproducing test,
  fix the root cause, then rerun the full sanitizer suite.
- Do not suppress a report, disable a sanitizer, or lower queue contention to
  hide a failure.
- For a gateway-only issue, verify the malformed-frame and queue-overload paths
  as well as the happy path.
- Report the exact configure command, test command, and final pass/fail result.

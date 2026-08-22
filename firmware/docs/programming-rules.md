# Programming rules

House rules for this codebase. They exist because each one was earned.

## Architecture

1. **Logic lives in exactly one place.** The firmware decides everything
   about the lighting; the web page only displays what the module pushes and
   sends commands. Never re-implement firmware behavior in JavaScript. Unit
   tests exercise the same code the device runs.
2. **Simulated signals enter at the head of the pipeline.** Test/simulation
   inputs are injected as raw signals into `input_conditioner` (synthetic
   flasher wave for turns), so every downstream rule (debounce, blink
   tracking, brake holdoff) applies identically to real and simulated input.
   Never merge simulated state further down.
3. **Pure, host-testable cores.** Decision logic (`blinker.c`,
   `effect_eval.c`, `compositor.c`, `event_arbiter.c`) has zero ESP-IDF
   dependencies and compiles on the host. Every behavior rule gets a host
   test (`test/host/run_tests.sh`). ESP-specific code is a thin wrapper.
4. **Safety first, always.** Boot order lights the strip from compiled-in
   fallback effects before storage or network are touched. Any config,
   filesystem, or network failure degrades to working position/brake/turn
   lighting — never to darkness. The render task never parses JSON, never
   takes a mutex, and never blocks on flash; changes cross over via an
   ownership-transfer queue.
5. **Contracts are files, and they move with the code.**
   `docs/EFFECT_SPEC.md` (effect semantics) is normative, and
   `components/net_services/proto/ws_protocol.proto` *is* the protocol:
   nanopb regenerates the C bindings from it on every build, so the firmware
   cannot drift from it. The webpage decodes the same wire format by hand
   (no bundler there — `tools/build_webui.sh` only inlines and gzips), so a
   protocol change updates the .proto, the .options file and `webui/app.js`
   together.

## Hardware / concurrency

6. **All RMT channels are created from core 1.** The ESP32 RMT peripheral
   has one shared interrupt source; splitting its handlers across cores
   deadlocks during flash operations (interrupt-watchdog boot loop, learned
   the hard way). RMT ISRs are IRAM-safe (`CONFIG_RMT_ISR_IRAM_SAFE`) so the
   strip survives flash writes.
7. **Single-writer snapshots over locks.** Cross-task state is published
   with C11 atomics by one writer and read by one reader (input snapshot,
   frame mirror, stats). Blocking flash writes (NVS) happen only in
   low-priority task context, never in timers or the render loop.
8. **No power-save trickery.** The module runs off the bike's battery:
   `WIFI_PS_NONE`, `TCP_NODELAY` — latency beats microamps here.

## Style

9. **No magic numbers or magic strings.** Use `#define`/named constants for
   timeouts, thresholds, ids, field numbers. (Factory-effect JSON literals
   are the data itself and are exempt.)
10. **`const` locals.** Any local variable that is never reassigned after
    initialization is declared `const` (and `T *const` where it applies).
11. **Comments state constraints, not narration.** A comment earns its place
    by explaining what the code cannot say (why a core is pinned, why a
    buffer is static, what invariant a caller must hold).
12. **Keep the UI minimal.** Factory effects + palette + sections cover the
    product. Any "test/preview" affordance drives the page view AND the real
    strip through the same mechanism, never two separate paths.

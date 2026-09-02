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
3. **Pure, host-testable cores.** Decision logic (`blinker.cpp`,
   `effect_eval.cpp`, `event_arbiter.cpp`, `config_rules.cpp`,
   `config_proto.cpp`) has zero ESP-IDF dependencies and compiles on the
   host; `test/host/run_tests.sh` covers the blinker, the effect evaluator,
   the arbiter and the config codec. A behaviour rule that lives in one of
   those files gets a host test. ESP-specific code is a thin wrapper.
4. **Safety first, always — and fail dark, never wrong.** The strips are
   latched black before anything else runs, and the boot order lights them
   from compiled-in fallback effects before storage or network are touched.
   A config that cannot be read runs the compiled defaults; a strip whose
   hardware fails, or a render task that hangs, goes dark (the watchdog
   reboots it) rather than improvise a signal. Never add a retry or fallback
   whose purpose is "keep something lit". The render task never takes a
   mutex and never blocks on flash; changes cross over via an
   ownership-transfer queue.
5. **Contracts are files, and they move with the code.**
   `docs/EFFECT_SPEC.md` (effect semantics) is normative, and
   `components/protocol/proto/ws_protocol.proto` *is* the protocol:
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
   strip survives flash writes. **Its memory is a hard budget**: 8 blocks of
   64 symbols, contiguous per channel. The status LED holds one and the two
   strips split the rest (`RMT_MEM_BLOCK_SYMBOLS` in `led_driver.cpp`, with a
   `static_assert`). Over-asking does not degrade — the last strip created
   gets `ESP_ERR_NOT_FOUND` and stays dark.
7. **The stored config is protobuf, and it is guarded three ways.**
   `SysConfig` goes into NVS as the *same* protobuf encoding the API speaks
   (key `syscfgpb`, namespace `motolight`), prefixed by a CRC32 over it —
   one codec in `config_store/config_proto.cpp`, so wire and flash cannot
   drift apart. `load()` accepts it only if the CRC matches, the message
   decodes, **and** `validate()` passes; anything else boots the compiled
   defaults with the purple status LED. Adding a field to the .proto is
   therefore additive: an older build skips what it does not know, a newer
   one defaults it, and nobody has to re-enter their configuration. Removing
   or repurposing a field number is the breaking move — reserve it instead.
   The learned flasher period lives in its own key (`blinkms`).
8. **Single-writer snapshots over locks.** Cross-task state is published
   with C11 atomics by one writer and read by one reader (input snapshot,
   frame mirror, stats). Blocking flash writes (NVS) happen only in
   low-priority task context, never in timers or the render loop.
9. **No power-save trickery.** The module runs off the bike's battery:
   `WIFI_PS_NONE`, `TCP_NODELAY` — latency beats microamps here.

## Style

10. **No magic numbers or magic strings.** Use named `constexpr` constants
   (or `#define` where a macro is unavoidable) for timeouts, thresholds,
   ids, field numbers. The factory-effect step tables are the data itself
   and are exempt.
11. **`const` locals.** Any local variable that is never reassigned after
    initialization is declared `const` (and `T *const` where it applies).
12. **Comments state constraints, not narration.** A comment earns its place
    by explaining what the code cannot say (why a core is pinned, why a
    buffer is static, what invariant a caller must hold).
13. **Keep the UI minimal.** Factory effects + palette + sections cover the
    product. Any "test/preview" affordance drives the page view AND the real
    strip through the same mechanism, never two separate paths.

# CLAUDE.md

Firmware for a custom ESP32 board that drives auxiliary turn signals and a
brake light on a motorcycle top box, from the bike's own 12 V signals.

## The rule everything else follows

This is **supplementary** lighting — the bike carries its own legal lamps.
So the code that runs while riding stays simple and verifiable, and it fails
**dark**, never wrong: a strip showing nothing misleads nobody, a strip
showing the wrong signal does. Never add recovery, retries or fallback
patterns whose purpose is "keep something lit".

Latitude for richer behaviour (editing, previews, simulation, diagnostics)
exists only behind the config WiFi, which is off at boot.

## Where things are

| Path | What |
|---|---|
| `firmware/main/` | boot sequence — read it before changing startup order |
| `firmware/components/` | one concern per component; `fx`, `event_arbiter`, `input_conditioner` are host-testable |
| `firmware/components/tasks/include/tasks.hpp` | every task's name, stack, priority and core |
| `firmware/webui/` | `index.html` + `style.css` + `app.js`, inlined into one asset at build time |
| `firmware/docs/programming-rules.md` | the non-negotiables (numbered rules) |
| `firmware/docs/coding-style.md` | naming, Allman braces, `const char* p` |
| `firmware/docs/EFFECT_SPEC.md` | normative effect semantics |
| `user_manual/` | the end-user manual and its screenshots |

## Commands

```bash
firmware/test/host/run_tests.sh          # pure-logic tests, no ESP-IDF needed
firmware/tools/check_webui.py webui      # dangling ids / undefined symbols
firmware/tools/build_webui.sh            # inline + gzip the page asset
idf.py build && idf.py -p /dev/ttyACM0 flash
```

After editing anything in `firmware/webui/`, run `build_webui.sh` and commit
the regenerated `components/net_services/webui_dist/index.html.gz` — CI fails
if it is stale.

Changing `sys_config.h` means bumping `CFG_VERSION`: the stored config is the
raw struct, so the layout *is* the format.

## Before merging into master — regenerate the user manual

The manual's screenshots are taken from a **running module**, so they go
stale silently. Regenerate them on the bench before the merge, with this
prompt:

```
Regenerate the user manual before merging this branch into master.

1. Build and flash the branch to the bench module, then bring the config
   WiFi up ("wifi on" on the serial console) and note its IP.
2. Put the module in a representative state: a strip with a few sections
   covering left turn, brake and right turn, and — for the Simulate shot
   only — override on with a turn signal running, so the screenshot shows
   the animation live.
3. Run firmware/tools/capture_screenshots.sh <ip>, then LOOK at every
   image in user_manual/img/: each tab fully framed, no cut-off card, no
   empty black filler, and no personal data visible (the Setup capture
   deliberately stops before the WiFi card, which shows the home SSID).
4. Clear the simulation afterwards — override off and every forced signal
   off — so the module is left following its real inputs.
5. Read user_manual/README.md against what the screenshots now show and
   update every part the branch changed: tabs, controls, wording of
   options, effect list, status LED codes, limits, troubleshooting rows.
   The manual is written for a rider, not a developer — no component
   names, no source paths.
6. Check that every image referenced by the manual exists, and commit the
   screenshots together with the text.
```

## Committing

Imperative subject line, a body that says *why*, and:

```
Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
```

Keep unrelated changes in separate commits — a mechanical sweep (renames,
formatting) always gets its own, so it never hides a behaviour change.

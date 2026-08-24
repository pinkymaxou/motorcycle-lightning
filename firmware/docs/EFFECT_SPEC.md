# Effect Specification (normative)

This document is the single source of truth for effect semantics, implemented
once in `components/fx/effect_eval.cpp`. Effects are defined as native
constexpr step tables in `components/fx/factory_effects.cpp` — there is no
JSON anywhere; the web UI receives only the module's rendered frames (see
`components/net_services/proto/ws_protocol.proto`). If a client-side preview is ever reintroduced, it
must implement this spec exactly.

## Model

An **effect** is a timeline of 1..16 **steps** played sequentially. Each step
has a duration (ms), an easing curve, one spatial **primitive**, and start→end
values for its two colors and up to four parameters. The evaluator is a pure
function:

```
evaluate(effect, t_ms, zone_len, mirror) -> RGBA[zone_len]
```

### Position

Pixel `i` of a section of `zone_len` pixels has position
`pos = (i + 0.5) / zone_len` ∈ (0,1). If `mirror` is set, `pos := 1 − pos`.
Effects are always authored section-relative; one effect fits any section length.

### Time, steps, looping

- `total = Σ dur`; cumulative ends `end[k]`.
- Step selection at time `t`: smallest `k` with `t < end[k]` (the last step
  also serves `t == total−1` after wrapping).
- Within a step: `u = (t − start) / dur`, clamped to [0,1]; `e = ease(u)`.
- `loop_from = n ≥ 0`: for `t ≥ total`, `t := loop_at + (t − loop_at) mod
  (total − loop_at)` where `loop_at = end[n−1]` (0 for n = 0).
- `loop_from = −1` (one-shot): for `t ≥ total`, `t := total − 1` (last frame
  held).
- On an event activation edge the timeline restarts at `t = 0`. Turn-signal
  sub-effects restart on every debounced signal phase edge (they follow the
  bike's flasher).

### Interpolation

Per frame (not per pixel): `c1 = lerp(c1_start, c1_end, e)`, same for `c2`,
and `p[j] = ps[j] + (pe[j] − ps[j])·e`. Color lerp is per-channel linear on
the stored 8-bit RGBA values, rounded to nearest.

### Easing

| name | e(u) |
|---|---|
| linear | u |
| hold | 0 (start values held) |
| inQuad | u² |
| outQuad | 1 − (1−u)² |
| inOutQuad | u<0.5 ? 2u² : 1 − 2(1−u)² |
| outCubic | 1 − (1−u)³ |
| inOutSine | 0.5 − 0.5·cos(πu) |

### Primitives

All produce RGBA per pixel; `mix(a,b,k)` is the per-channel linear blend with
`k` clamped to [0,1]; `min_soft = 1 / zone_len`; `transparent = (0,0,0,0)`.
Parameter names map to slots at compile time; defaults in parentheses.

**fill** — `start`(0), `end`(1):
`pos ∈ [start, end] ? c1 : transparent`

**wipe** — `edge`(0), `soft`(0.08); mode `low|high|center_out|edges_in`:
coverage `s` = pos | 1−pos | |pos−0.5|·2 | 1−|pos−0.5|·2 respectively;
`k = clamp01((edge − s) / max(soft, min_soft))`; `out = mix(c2, c1, k)`.
*Convention: to fully cover the zone at the end of a sweep, animate `edge`
to `1 + soft` (the trailing soft edge must clear the last pixel).*

**scan** — `pos`(0.5), `width`(0.2), `soft`(0.05):
`d = |pixel_pos − pos|`; `k = clamp01((width/2 − d) / max(soft, min_soft))`;
`out = mix(c2, c1, k)`.

**gradient** — `offset`(0), `scale`(1); mode `clamp|repeat`:
`s = pos·scale − offset`; clamp → `clamp01(s)`, repeat → `frac(s)`;
`out = mix(c1, c2, s)`.

**chase** — `phase`(0), `period`(0.15), `duty`(0.5), `soft`(0):
`u = frac(pos / max(period, 1e−4) − phase)`;
`soft ≤ 0`: `k = u < duty ? 1 : 0`; else `k = clamp01(min(u, duty−u) / soft)`;
`out = mix(c2, c1, k)`.

### Compositing (device)

A strip is an ordered list of up to 8 **sections** laid end to end in wiring
order; the strip's LED count is the sum of their lengths. Sections never
overlap, so each one composites independently over its own range.

Within a section the paint order is **idle, aux, brake, turn**, with a single
blend rule: source-over alpha onto an opaque black RGB frame. Alpha decides
visibility, the section decides territory. An unassigned event paints nothing.

- `mirror` is the section's declared direction, not something derived from
  where the section sits — a section reversed in the config reverses every
  layer it paints.
- A section blinks only if its own turn source (none / left / right) is in
  blink mode. While it blinks, its **brake layer is skipped entirely**, so it
  alternates its position and turn colors only.
- Turn sub-effect timelines are **normalized to the flasher**: layer time is
  scaled by `total_ms / (period_ms / 2)`, so the effect's full duration plays
  over exactly one signal phase whatever the flasher rate. Other layers run in
  real time.
- Hazard (both signals blinking) resolves one master channel per strip — the
  one that entered blink mode first, tie to the left — and every section
  follows its phase, so sections and strips cannot drift apart.
- Hazard may also **replace** the turn effects: the config carries one shared
  `hazard_on` / `hazard_off` pair, and while both signals blink they stand in
  for every section's `turn_on` / `turn_off`. Each phase falls back on its own
  when its id is empty. Hazard is a vehicle state, not a direction, so the
  override is shared rather than per section, and it is time-scaled to the
  flasher exactly like the turn effects it replaces.
- The brake effect's intro (steps before `loop_from`) replays only if the
  brake was released for the configured holdoff; a quicker re-application
  starts the timeline at the loop segment.
- While the brake input is physically active, a post-composite red floor
  (R ≥ 64) applies to every section that has a brake effect assigned and is
  not currently blinking. The **Off** effect is excluded: assigning it is an
  explicit "dark here", and the configuration owns that decision. `none` is
  also excluded, since nothing was assigned at all.

### Output (device only)

`out = gamma_lut[c]` with a gamma-2.2 LUT, applied
after compositing. The webpage receives the pre-gamma composited frame and
draws it as-is — an sRGB display applies ~2.2 itself, so the two match.

### Authoring

Factory effects are `constexpr` step tables (`EffectDef` in
`factory_effects.cpp`): per step a primitive, easing, duration, start/end
colors (palette reference or literal RGBA) and start/end parameter slots.
Limits: 16 steps per effect, durations 10 ms - 60 s.

Palette references resolve at build time to **opaque** premultiplied colors
(`rgb x a/255`, alpha forced to 255): the palette's alpha channel is a
brightness control, and the same palette color renders identically no matter
how many layers paint it. The palette itself is a fixed enum — position,
brake, turn, white — whose values are user-editable.

# motorcycle-lightning

Motorcycle turn signal and brake effects on a WS2812B strip, driven by an
ESP32 (M5Stamp Pico) on a custom opto-isolated interface PCB (see `pcb/`).

## What it does

- Reads the bike's 12V lighting signals (left/right turn, brake, aux) through
  optocouplers — turn inputs pulse at the flasher rate; the firmware learns
  the flasher period, persists it, and leaves "blink mode" 1.5× past the
  expected next flash.
- Renders zones on one strip: `[left turn | center | right turn]`. Turn zones
  alternate amber (or red) / low-red **in sync with the real flasher** — the
  sweep animation is normalized to the flasher half-period. Everything else
  shows the position light (dim red) or the brake light; the brake never
  paints inside an actively blinking turn zone. The brake strobe intro
  replays only after the brake was released for a configurable delay
  (default 25 s).
- Hosts a config webpage (SoftAP `MotoLights` at http://192.168.4.1, and on
  the home network in APSTA mode): assign factory effects per event, tune the
  named color palette (turn color amber/red/custom), set zones/LED count, and
  test everything — the page shows the module's real frames streamed over a
  WebSocket at ~30 FPS. **The whole protocol (WebSocket and REST) is
  protobuf** (`firmware/components/net_services/proto/ws_protocol.proto`); there is no JSON anywhere.
  Simulated signals are injected at the head of the one input pipeline, so
  the simulation behaves exactly like the real inputs (including the brake
  holdoff), and an override mode ignores the physical inputs while testing.

All lighting decisions live in the firmware only; the page displays frames.
Safety floor: factory effects are compiled into the firmware, any config or
storage failure degrades to working position/brake/turn lighting.

## Build (ESP-IDF 6.1)

```sh
cd firmware
cp main/wifi_creds.h.example main/wifi_creds.h   # optional: seeds the home WiFi
                                                 # on first boot (gitignored)
source ~/esp/esp-idf-6.1/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # via the 6-pin PROG header
```

PROG header wiring to a 3.3V USB-serial adapter: 3V3, GND, adapter TX→G3,
RX→G1, RTS→EN, DTR→G0. A CP210x shows up as `/dev/ttyUSB0`, a CH9102 as
`/dev/ttyACM0`; under WSL both need `usbipd attach` first.

The home network is configured from the webpage (Setup tab: SSID, password,
STA active) and stored in NVS — `wifi_creds.h` only seeds an empty config on
first boot.

**The config WiFi is off at boot** — riding needs no radio. Press the module
button (G39) to bring the SoftAP (and the home-network STA, if configured)
up, press it again to shut it down. The on-module status LED blinks at 2 Hz:
**green/blue = WiFi on** (configuration), **plain green = WiFi off** (normal
riding), orange = network error, purple = default config.

After flashing, the page is unreachable until you press the button once.

Host unit tests (no ESP-IDF needed): `firmware/test/host/run_tests.sh`

After editing `firmware/webui/index.html`, run `firmware/tools/build_webui.sh`
and rebuild.

## Layout

- `firmware/main/` — boot sequence (safety-first: lighting runs before storage/network)
- `firmware/components/fx/` — effect model, evaluator, compositor, native factory effects
- `firmware/components/input_conditioner/` — 1 kHz sampling, debounce, blinker tracker,
  brake-strobe holdoff, simulated-signal injection
- `firmware/components/event_arbiter/` — inputs + config → layer stack (zones, priorities)
- `firmware/components/render_core/` — ~75 FPS render task (core 1) + control queue
- `firmware/components/net_services/` — SoftAP+STA, REST API, WebSocket protobuf push,
  embedded web app
- `firmware/webui/` — the single-file config page (display only, no lighting logic)
- `firmware/docs/EFFECT_SPEC.md` — normative effect semantics
- `firmware/components/net_services/proto/ws_protocol.proto` — WebSocket message contract
- `pcb/` — schematic, layout, EasyEDA project

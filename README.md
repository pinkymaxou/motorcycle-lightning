# motorcycle-lightning

[![build](https://github.com/pinkymaxou/motorcycle-lightning/actions/workflows/build.yml/badge.svg)](https://github.com/pinkymaxou/motorcycle-lightning/actions/workflows/build.yml)

Motorcycle turn signal and brake effects on a WS2812B strip, driven by an
ESP32 (M5Stamp Pico) on a custom opto-isolated interface PCB (see `pcb/`).

## What it does

- Reads the bike's 12V lighting signals (left/right turn, brake, aux) through
  optocouplers — turn inputs pulse at the flasher rate; the firmware learns
  the flasher period, persists it, and leaves "blink mode" 1.5× past the
  expected next flash.
- Each strip is built as an ordered list of **sections** (up to 8, any
  length): a section has its own animation direction, its own turn source
  (left / right / none) and its own effect per event, so a bar can be laid out
  however the bike needs it. A section blinks **in sync with the real
  flasher** — the sweep is normalized to the flasher half-period — and while
  it blinks its brake effect is skipped, so it only alternates position and
  turn colors. The brake strobe intro replays only after the brake was
  released for a configurable delay (default 25 s).
- Hosts a config webpage (SoftAP `MotoLights` at http://192.168.4.1, and on
  the home network in APSTA mode): assign factory effects per event, tune the
  named color palette (turn color amber/red/custom), build the sections, and
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

Firmware updates go over WiFi from the System tab (or
`curl -X POST --data-binary @firmware/build/motorcycle_lightning.bin
http://<module>/api/ota`): the image lands in the spare OTA slot and the
module rolls back on its own if it fails to boot.

Host unit tests (no ESP-IDF needed): `firmware/test/host/run_tests.sh`

After editing anything in `firmware/webui/`, run `firmware/tools/build_webui.sh`
and rebuild. It checks the page for dangling ids and undefined symbols, then
inlines `style.css` and `app.js` into `index.html` and gzips the result: the
sources stay split for editing, the module still serves one request.

## Layout

- `firmware/main/` — boot sequence (safety-first: lighting runs before storage/network)
- `firmware/components/fx/` — effect model, evaluator, compositor, native factory effects
- `firmware/components/input_conditioner/` — 1 kHz sampling, debounce, blinker tracker,
  brake-strobe holdoff, simulated-signal injection
- `firmware/components/event_arbiter/` — inputs + config → layer stack (sections, priorities)
- `firmware/components/render_core/` — ~75 FPS render task (core 1) + control queue
- `firmware/components/net_services/` — SoftAP+STA, REST API, WebSocket protobuf push,
  embedded web app
- `firmware/webui/` — the config page: `index.html`, `style.css`, `app.js`
  (display only, no lighting logic), inlined into one asset at build time
- `firmware/docs/EFFECT_SPEC.md` — normative effect semantics
- `firmware/components/net_services/proto/ws_protocol.proto` — WebSocket message contract
- `pcb/` — schematic, layout, EasyEDA project

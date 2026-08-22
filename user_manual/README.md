# MotoLights — user manual

Auxiliary turn signals and brake light for a motorcycle top box, driven from
the bike's own 12 V lighting signals.

The module reads LEFT, RIGHT, BRAKE and an AUX input through optocouplers and
drives up to two WS2812B-family strips. It learns your flasher's rhythm, so
the animations run in step with the bike instead of guessing. A configuration
page lets you describe the strip and see exactly what it is doing, live.

> **This is supplementary lighting.** Your motorcycle keeps its own legal
> lamps and they remain the ones that matter. The module is built to fail
> **dark** rather than wrong: a strip that shows nothing misleads nobody, a
> strip that shows the wrong signal does. Never rely on it as your only turn
> signal or brake light.

---

## 1. Wiring

Each input is opto-isolated and **active low**: applying 12 V to it turns the
signal ON. The strips need 5 V; the board level-shifts the data lines.

![Pinout tab](img/03-pinout.png)

The Pinout tab of the config page always shows the pin map of the firmware
you are actually running, so check it there rather than trusting a printout.

**Which end of the strip?** LED 1 is the end where the data connector is. If
your bar is mounted with the connector on the right, say so in Setup
(*reversed data direction*) and everything — animations, previews, the live
view — follows.

---

## 2. First power-up

The status LED on the module tells you where you are:

| Status LED | Meaning |
|---|---|
| solid blue | booting |
| green blink, 2 Hz | running normally, config WiFi **off** |
| green / blue alternating, 2 Hz | running, config WiFi **on** |
| purple blink | running on factory defaults — the stored config was rejected |
| orange blink | network error |

**The config WiFi is off at boot, on purpose.** Riding needs no radio, and a
module that does not transmit uses less power and has less to go wrong. Press
the module button once to bring it up; press again to shut it down.

---

## 3. Getting to the page

1. Press the module button — the status LED starts alternating green/blue.
2. Join the WiFi network **MotoLights** from your phone or laptop.
3. Open **http://192.168.4.1**.

If you gave the module your home WiFi in Setup, it also joins that network
when the config WiFi is up, and the page is reachable at the address shown in
the System tab. The SoftAP always stays available either way, so you can
never lock yourself out.

The page has four tabs. Everything you change lives in **Setup** and is only
applied when you press **Save** — the Save button grows a dot when you have
unsaved changes, and leaving the tab asks first.

---

## 4. Simulate — see what the module is doing

![Simulate tab](img/01-simulate.png)

The strip drawing is not a mock-up: it is the module's real frames, pushed
about 75 times a second. What you see here is exactly what the LEDs show.

- **LEFT / HAZARD / RIGHT / BRAKE / AUX** inject a simulated signal into the
  module's normal pipeline — the lighting logic is the same one used on the
  road, never a copy running in your browser.
- **Override real inputs** makes the module ignore the physical inputs while
  you test on the bench. It expires by itself after 60 seconds, so a
  simulation can never follow you onto the road.
- **Real inputs** shows the physical state of the four inputs, with the
  learned flasher period next to it.
- The **debug timeline** keeps the last 20 seconds: the four inputs on top,
  then one lane per section showing its real average colour.

---

## 5. Setup — describing your strip

![Setup tab](img/02-setup.png)

### Sections

A strip is an ordered list of up to **8 sections**, laid end to end starting
at the connector. Each section has its own length, direction and role, and
the strip's total is simply their sum (300 LEDs maximum).

For each section:

| Column | What it does |
|---|---|
| **LEDs** | how many pixels this section covers |
| **Direction** | which way its animations run — *Reverse* flips them |
| **Role** | a preset, or **Custom…** to set each event yourself |

**Roles** are shortcuts that fill in the effects below:

- **Turn left** / **Turn right** — position light, brake, and the turn
  animation driven by that side's signal
- **Brake + position** — no blinker, lights up on the brake
- **Position only** — always on, ignores brake and turn
- **Custom…** — opens the five selectors: *Turn source*, *Idle / position*,
  *Brake*, *Turn ON*, *Turn off phase*, *Aux*

Any event can be set to **— none —**, which means "paint nothing here". Note
that **— none —** and the *Off (dark)* effect are not the same thing: *Off*
is an effect that actively paints black, and a section with any brake effect
assigned still gets the red safety floor while you are braking. If you want a
section to ignore the brake completely, choose **— none —**.

Use **↑ ↓** to reorder sections — the order *is* the wiring order — and
**✕** to delete one. Rows alternate shading, and the preview above shades
every other section, so two neighbouring sections of the same role stay
apart.

### Effects available

| Effect | What it looks like |
|---|---|
| Position light | steady dim red |
| Brake solid | full red |
| Brake 3× flash | three quick flashes, then solid |
| Turn ON (solid) | the whole section in the turn colour |
| Turn ON sweep | the turn colour sweeps across the section |
| Turn off-phase (low red) | what a blinking section shows between flashes |
| Full white | steady white |
| Off (dark) | paints black |

### Strip hardware

**LED model** (WS2812/WS2812B, SK6812, WS2811, WS2816) and **colour order**
(GRB, RGB, GRBW, RGBW) are per strip — one physical controller, one LED type.
If your colours come out swapped (red showing as green), the colour order is
what to change.

### Shared settings

These come from the bike itself, so they apply to every strip:

- **Turn signal colour** — amber, red, or a custom colour from the palette
- **Brake strobe holdoff** — the brake's flash intro only replays if the
  brake was released for at least this long, so stop-and-go traffic does not
  turn into a strobe show. `0` replays every time.

### Colours

Four semantic colours — position, brake, turn signal, white — that the
effects refer to. Editing one changes every effect that uses it. The
brightness slider dims the colour itself, and it is the **only** brightness
control: there is no separate per-strip level to fight with.

### WiFi — home network

Optional. Enter your network's SSID and password to have the module join it
whenever the config WiFi is up; it is only there to make the page easier to
reach from a laptop. The password is write-only — leave it blank to keep the
current one. The module never brings up any radio on its own at boot.

### Save / Restore defaults

**Save** writes everything to the module's flash and applies it immediately.
**Restore defaults** puts back the factory layout (a 40-LED bar: 12 left turn,
16 brake, 12 right turn) and clears your settings, including the WiFi ones.

---

## 6. System — what the module reports

![System tab](img/04-system.png)

Firmware build, chip, addresses, uptime, free memory — and **Unexpected
resets**, which is the one worth a glance. The module keeps the last 8
resets it did not ask for (a crash or a watchdog reboot) in flash. Losing
power is not a crash and is never counted, so anything other than *no
unexpected reset* here is worth reporting.

---

## 7. How it behaves on the road

**Turn signals.** The module measures your flasher's period the first time it
sees it blink and remembers it across reboots. A sweep is scaled to that
period, so it always finishes in exactly one flash whatever the rate. When
the signal stops, the module waits 20 % longer than the learned period before
leaving blink mode — it never cuts a flash short.

**Hazards.** With both signals blinking, both sides lock to the one that
started first, so the two ends of the bar — and both strips — stay in step
instead of drifting apart.

**Braking.** While the brake input is on, every section that has a brake
effect gets a red floor, so it can never be darker than a visible red. A
section that is currently blinking skips its brake layer entirely: a turn
signal outranks the brake light on that piece of strip, which is what a
driver behind you needs to see.

**Failures.** The strips are latched black in the first milliseconds of boot,
before anything else runs. If the lighting task ever stops, a watchdog reboots
the module — and it comes back dark rather than frozen on half a frame.

---

## 8. Troubleshooting

| Symptom | What it means |
|---|---|
| Strip completely dark, module otherwise alive | no section defined for that strip, or a wiring problem — check the Setup total (`0 / 300 LEDs`) |
| Purple blinking status LED | the stored configuration was rejected and factory defaults are running — open Setup, check it, and Save |
| Page unreachable | config WiFi is off — press the module button; the LED must alternate green/blue |
| Colours wrong (red shows green) | wrong **colour order** for your strip |
| Animation runs the wrong way | flip the section's **Direction**, or the strip's **reversed data direction** if the whole bar is mirrored |
| Blinking out of step with the bike | let it blink a few times so the period gets learned; the System tab shows the learned value |
| `Unexpected resets` is not zero | the module crashed or was rebooted by its watchdog — note the reason and report it |

A serial console is available at 115200 baud with a few commands: `wifi`,
`wifi on`, `wifi off`, `crashlog`, `crashlog clear`, `reboot`.

---

## 9. Limits

| | |
|---|---|
| Strips | 2 independent outputs |
| Sections per strip | 8 |
| LEDs per strip | 300 |
| Frame rate | ~75 Hz, refreshed continuously |
| LED families | WS2812/WS2812B, SK6812, WS2811, WS2816 |
| Inputs | LEFT, RIGHT, BRAKE, AUX — 12 V, opto-isolated, active low |

---

*Screenshots are taken from a running module with
`firmware/tools/capture_screenshots.sh <module-ip>`.*

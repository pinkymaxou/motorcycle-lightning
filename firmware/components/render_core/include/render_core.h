/* Render core: the ~75 FPS render task (core 1) + the control queue through
 * which config changes cross over (ownership transfer — the render loop
 * never parses anything and never takes a mutex). */
#pragma once

#include "esp_err.h"
#include "sys_config.h"
#include "cond_state.h"

namespace RenderCore
{

/* Starts the render task with the compiled-in hard-fallback effect set —
 * lighting is functional from this call on, before any storage is touched.
 * strip_gpios: one WS2812B data pin per strip (initialized from the render
 * task so the RMT interrupts allocate on its core). */
esp_err_t start();

/* Build the full effect set for cfg (factory effects, palette-resolved) and
 * swap it into the render task. Safe to call from boot or httpd context.
 * Unknown ids fall back per-effect and are reported in warnings(). */
esp_err_t applyConfig(const SysConfig& cfg);

/* Simulated signals (webpage buttons) — forwarded to the head of the input
 * pipeline (InputConditioner). */
void forceEvent(CondEvent ev, bool active);
bool eventForced(CondEvent ev);
void setOverride(bool active);
bool overrideActive();

void getStats(uint32_t* fps_x10, uint32_t* frame_us_max);

/* Warnings from the last applyConfig (empty string if none). */
const char* warnings();

/* Copy of the most recent composited frame (logical RGB, pre-gamma) for the
 * webpage's live strip view. Returns the LED count copied into out. */
uint16_t getFrame(StripId strip, uint8_t* out_rgb, uint16_t max_leds);

} // namespace RenderCore

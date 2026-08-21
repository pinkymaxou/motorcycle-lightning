/* GPIO sampling + blinker tracking (1 kHz esp_timer). Publishes a lock-free
 * snapshot for the render task. */
#pragma once

#include "esp_err.h"
#include "cond_state.h"

namespace InputConditioner
{

struct InputPins
{
    int left, right, brake, aux;   /* GPIO numbers, active-low inputs */
};

/* stored_period_ms: persisted flasher period (0 = never learned). */
esp_err_t init(const InputPins &pins, uint32_t stored_period_ms,
               uint8_t exit_x10);

void get(CondState *out);

/* True once when a newly learned period should be persisted; clears the flag
 * and returns the value. Call from a low-priority context (NVS write). */
bool takeDirtyPeriod(uint32_t *period_ms);

/* Required brake-release time before the brake effect's intro replays.
 * Safe to call any time (config changes). */
void setBrakeHoldoff(uint32_t ms);

/* Blink-mode exit factor x10 (12 = period + 20% grace). Live-applicable. */
void setExitFactor(uint8_t x10);

/* Simulated signals (webpage): injected as RAW inputs at the head of the one
 * and only pipeline — forced turns pulse like a real flasher, forced brake
 * goes through the same debounce/holdoff logic. Auto-clear after 60 s. */
void forceEvent(CondEvent ev, bool active);
bool eventForced(CondEvent ev);

/* Override: while active the REAL inputs are ignored (forced ones still
 * apply). Auto-expires after 60 s; keep-alive by re-calling. */
void setOverride(bool active);
bool overrideActive();

} // namespace InputConditioner

/* On-module button (G39, input-only, external pull-up: pressed = low).
 *
 * A press toggles the config WiFi. Holding it for HOLD_FACTORY_MS asks for a
 * factory reset — long on purpose: it throws away every setting, and it is
 * the only way back in if the access point was given a password nobody
 * remembers. */
#pragma once

#include "esp_err.h"

namespace UiButton
{

using Callback = void (*)();

constexpr uint32_t HOLD_FACTORY_MS = 15000;

/* on_press fires when the button is released after a short press; on_hold
 * fires once, while it is still held, at HOLD_FACTORY_MS. */
esp_err_t init(int gpio, Callback on_press, Callback on_hold);

} // namespace UiButton

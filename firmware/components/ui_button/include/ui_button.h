/* On-module button (G39, input-only, external pull-up: pressed = low).
 * v1: callback just logs; later it will toggle NetServices on/off. */
#pragma once

#include "esp_err.h"

namespace UiButton
{

using Callback = void (*)();

esp_err_t init(int gpio, Callback on_press);

} // namespace UiButton

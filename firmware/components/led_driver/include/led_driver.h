/* WS2812B strip output (led_strip / RMT backend) with per-strip brightness,
 * hardware type and gamma correction at the output stage. The PCB carries
 * STRIP_COUNT independent outputs; each is addressed by index. */
#pragma once

#include <cstdint>
#include "esp_err.h"
#include "sys_config.h"

namespace LedDriver
{

/* Call from the render task so the RMT interrupts allocate on its core.
 * gpios[] holds one data pin per strip. */
esp_err_t init(const int *gpios, int count, uint16_t max_leds);

void setBrightness(StripId strip, uint8_t brightness);
void setReversed(StripId strip, bool reversed);

/* Select one strip's family and wire order. Recreates that strip's RMT
 * device when the type actually changes, so it MUST be called from the
 * render task (all RMT channels belong to core 1). */
esp_err_t setLedType(StripId strip, LedModel model, ColorOrder order);

/* Push a logical RGB frame (rgb[count*3]) to one strip. Applies direction,
 * brightness and gamma. Blocks until the previous refresh is done. */
esp_err_t write(StripId strip, const uint8_t *rgb, uint16_t count);

} // namespace LedDriver

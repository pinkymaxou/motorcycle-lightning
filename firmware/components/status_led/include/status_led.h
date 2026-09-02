/* On-module SK6812 status pixel (G27). */
#pragma once

#include "esp_err.h"

namespace StatusLed
{

enum class State : uint8_t
{
    Boot = 0,        /* solid blue */
    RunningWifi,     /* green/blue at 2 Hz: running, config WiFi active */
    Running,         /* green blink at 2 Hz: running, WiFi off */
    NetError,        /* orange blink */
    ConfigFallback   /* purple blink */
};

/* Hold one colour, ignoring the blink pattern, until the module reboots.
 * Used to acknowledge a factory reset before the NVS is erased. */
void solid(uint8_t r, uint8_t g, uint8_t b);

esp_err_t init(int gpio);
void set(State state);

} // namespace StatusLed

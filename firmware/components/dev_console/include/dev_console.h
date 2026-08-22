/* Serial console on the programming UART: one line, one command. Exists so
 * the module can be driven from a terminal (or a script) without reaching
 * for the on-module button — handy while the config WiFi is off by default. */
#pragma once

#include "esp_err.h"

namespace DevConsole
{

/* Called from the console task for every complete line received. */
using LineHandler = void (*)(const char* line);

esp_err_t start(LineHandler handler);

} // namespace DevConsole

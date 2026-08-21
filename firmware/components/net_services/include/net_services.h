/* SoftAP + HTTP config service. start/stop are idempotent — stop/start is the
 * hook the module button will use later to toggle the config webpage. */
#pragma once

#include "esp_err.h"
#include "sys_config.h"

namespace NetServices
{

/* live_cfg points at the application's authoritative config; the HTTP API
 * reads and mutates it (single mutator: the httpd task). STA settings come
 * from the config (sta_ssid / sta_pass / sta_active). */
esp_err_t start(SysConfig *live_cfg);
esp_err_t stop();
bool running();
int staCount();

/* Apply new STA settings live (join/leave the home network). */
esp_err_t reconfigureSta(const char *ssid, const char *pass, bool active);

/* Firmware pin assignments shown on the System page. The table must stay
 * valid for the program's lifetime (main/board_pins.h is the source). */
struct PinDef
{
    const char *name;
    int gpio;
    const char *desc;   /* what the pin does */
};

void setPinout(const PinDef *pins, int count);

} // namespace NetServices

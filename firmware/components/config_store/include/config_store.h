/* Persistence: system config as a versioned NVS blob. */
#pragma once

#include "esp_err.h"
#include "sys_config.h"

namespace ConfigStore
{

/* Compiled-in defaults — the device must be fully functional from these. */
void defaults(SysConfig *cfg);

esp_err_t init();                       /* open the NVS handle */
esp_err_t load(SysConfig *cfg);         /* validates; error = use defaults */
esp_err_t save(const SysConfig *cfg);

/* Learned flasher period, stored separately (written rarely). 0 = none. */
uint32_t  loadBlinkPeriod();
esp_err_t saveBlinkPeriod(uint32_t period_ms);

/* Sanity validation: ranges, enums, and NUL-terminated strings (a corrupt
 * NVS blob of the right size must never yield unterminated strings). */
bool validate(const SysConfig *cfg);

} // namespace ConfigStore

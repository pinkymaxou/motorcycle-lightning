/* Persistence and the config's wire form. Both are the same protobuf
 * encoding: a field added to the .proto is skipped by an older build and
 * defaulted by a newer one, so the stored configuration survives a schema
 * change instead of being thrown away. */
#pragma once

#include "esp_err.h"
#include "sys_config.h"

namespace ConfigStore
{

/* Compiled-in defaults — the device must be fully functional from these. */
void defaults(SysConfig* cfg);

esp_err_t init();                       /* open the NVS handle */
esp_err_t load(SysConfig* cfg);         /* validates; error = use defaults */
esp_err_t save(const SysConfig* cfg);

/* Learned flasher period, stored separately (written rarely). 0 = none. */
uint32_t  loadBlinkPeriod();
esp_err_t saveBlinkPeriod(uint32_t period_ms);

/* Sanity validation: ranges, enums, and NUL-terminated strings (a corrupt
 * blob must never yield unterminated strings). */
bool validate(const SysConfig* cfg);

/* The WiFi password is write-only over the wire: the page is told that one
 * is set, never what it is. Storage obviously needs the real thing. */
enum class Secrets : uint8_t
{
    Omit = 0,
    Include
};

/* Returns the encoded length, 0 on failure. */
size_t encode(const SysConfig& cfg, uint8_t* out, size_t cap, Secrets secrets);
bool decode(const uint8_t* data, size_t len, SysConfig* cfg);


} // namespace ConfigStore

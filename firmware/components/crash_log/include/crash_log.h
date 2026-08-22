/* Persistent record of the resets nobody asked for.
 *
 * A crash on the road leaves no trace once the module reboots, and the two
 * failures that matter here — a panic and a hung render task — both end in a
 * silent restart. This keeps the last CRASH_LOG_ENTRIES of them in NVS.
 * Losing power is not a crash and is never recorded. */
#pragma once

#include <cstdint>
#include "esp_err.h"

namespace CrashLog
{

constexpr int CRASH_LOG_ENTRIES = 8;

/* Reads the log and appends this boot's reset reason if it was unexpected.
 * Call once, after nvs_flash_init(). Diagnostics only: any failure is
 * swallowed, nothing here may ever hold up the lighting. */
void init();

/* One line for the boot log, the console and the System page, e.g.
 * "3 unexpected resets, last: task watchdog" or "no unexpected reset". */
const char* summary();

/* Oldest first. Returns how many names were written. */
int snapshot(const char* names[], int max);

esp_err_t clear();

} // namespace CrashLog

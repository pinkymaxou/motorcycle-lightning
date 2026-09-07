/* Ride counters, kept across a power cut.
 *
 * The bike cuts this module's power abruptly, several times a ride. The
 * counters live in two flash sectors used in alternation: the sector holding
 * the newest valid data is never the one being erased, so the worst a cut can
 * cost is the last interval. Everything that decides what is valid is in
 * stats_record / stats_store, which the host tests drive.
 *
 * Diagnostics only. If any of it fails, it stops and says so on the page —
 * it may never hold up the lighting. */
#pragma once

#include <cstdint>

#include "blinker.h"
#include "stats_record.h"

namespace Stats
{

using StatsRecord::Counters;

/* Time is counted in ticks of this, the unit the flush cadence was chosen
 * for: four writes a minute. */
constexpr uint32_t TICK_SECONDS = StatsRecord::TICK_SECONDS;

/* Reads what flash holds and counts this boot. Call after the render task is
 * running: this touches flash and must not spend the boot budget. Safe to
 * call when the partition is missing — the counters then live in RAM only. */
void init();

/* Call from the housekeeping loop. `wifi_on` says whether the config WiFi is
 * up, `quiet` whether no signal is currently running — an erase is only ever
 * started when it is. */
void tick(bool wifi_on, bool quiet);

/* Everything counted, including what has not reached flash yet. */
void get(Counters* out);

/* False when the counters are not being written to flash, with `why` set to a
 * short reason for the page. */
bool stored(const char** why);

/* Zeroes the counters by committing a fresh generation, so a power cut in the
 * middle can never bring the old ones back. */
bool reset();

/* One line for the console and the boot log. */
const char* summary();

/* Flushes now, whatever the cadence — on the way to a reboot. */
void flush();

/* Wipes the partition outright, for the button factory reset: nvs_flash_erase
 * does not touch a raw partition, and the manual promises the button erases
 * everything the module remembers. */
void eraseAll();

} // namespace Stats

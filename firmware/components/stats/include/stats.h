/* Ride counters, kept across a power cut.
 *
 * The bike cuts this module's power abruptly, several times a ride. The
 * counters live in two flash sectors used in alternation: the sector holding
 * the newest valid data is never the one being erased, so the worst a cut can
 * cost is the last interval. Everything that decides what is valid is in
 * stats_record / stats_store, which the host tests drive.
 *
 * Why a raw partition and not NVS, which the config and the crash log already
 * use: NVS gives the same tear safety and wear levelling, but it decides for
 * itself when to garbage-collect, and a sector erase stalls both cores for up
 * to 400 ms — thirty frames with a turn signal frozen mid-sweep. Here the
 * erase is ours to schedule, and it only runs when nothing is being signalled.
 * That is the whole reason this component exists; the price is a partition
 * that only a cable flash can add.
 *
 * Ownership: one task — the housekeeping loop — owns the counters and does
 * every flash operation. Other tasks read a published snapshot and ask for a
 * reset or a flush through a request the owner serves on its next pass, the
 * same way the button asks for a factory reset. There is no lock.
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

/* How long a caller on another task is willing to wait for the owner to serve
 * its request. The owner passes every HOUSEKEEPING_PERIOD_MS. */
constexpr uint32_t RESET_WAIT_MS = 3000;
constexpr uint32_t FLUSH_WAIT_MS = 1000;

/* ---- owner task only: the housekeeping loop ---------------------------- */

/* Reads what flash holds and counts this boot. Call after the render task is
 * running: this touches flash and must not spend the boot budget. Safe to
 * call when the partition is missing — the counters then live in RAM only. */
void init();

/* Call every housekeeping pass. `wifi_on` says whether the config WiFi is up,
 * `quiet` whether no signal is currently running — an erase is only ever
 * started when it is. Serves pending requests from other tasks. */
void tick(bool wifi_on, bool quiet);

/* For the button factory reset, on the way to a restart. Tear-safe: commits a
 * fresh generation holding zeros, then erases the old sector, so a cut in
 * between still leaves zeros as the newest data. `nvs_flash_erase` does not
 * touch a raw partition, and the manual promises the button erases everything
 * the module remembers. */
void factoryWipe();

/* ---- any task ---------------------------------------------------------- */

/* A consistent snapshot of everything counted, including what has not reached
 * flash yet. */
void get(Counters* out);

/* nullptr while the counters are being saved; otherwise a short reason for the
 * page — no partition, flash gave up, or a full sector still waiting for a
 * quiet moment to make room. */
const char* notStoredWhy();

/* Zeroes the counters by committing a fresh generation, so a power cut in the
 * middle can never bring the old ones back. Waits up to `wait_ms` for the
 * owner to do it; the owner only will while nothing is being signalled, since
 * it erases. On false, `why` says what stood in the way. */
bool reset(uint32_t wait_ms, const char** why);

/* Writes the counters now, whatever the cadence — on the way to a reboot.
 * Waits up to `wait_ms` for the owner; from the owner it is immediate. */
void flush(uint32_t wait_ms);

/* One line for the console and the boot log. */
const char* summary();

} // namespace Stats

/* The A/B sector state machine — pure logic, no ESP-IDF.
 *
 * Kept out of the flash adapter so the host tests can drive every power-cut
 * path: a tear inside a record, a tear inside an erase, a tear at the
 * roll-over commit point, and a tear during a reset. The adapter below it
 * only reads, writes and erases.
 *
 * The one invariant everything rests on, and the reason a power cut can never
 * cost more than one interval: the sector holding the newest valid data is
 * never the sector being erased.
 */
#pragma once

#include "stats_record.h"

namespace StatsStore
{

using StatsRecord::Counters;

/* Flash underneath. Every one returns false on failure; none may throw or
 * abort — a diagnostic must never be able to hold up the lighting. */
struct Ops
{
    bool (*readSlot)(void* ctx, int sector, int index, uint8_t* out);
    bool (*writeSlot)(void* ctx, int sector, int index, const uint8_t* in);
    bool (*eraseSector)(void* ctx, int sector);
    void* ctx;
};

/* Two consecutive flash failures retire the store rather than go on writing
 * into flash that is no longer taking programs. */
constexpr int FAILURES_ALLOWED = 2;

struct Store
{
    Ops      ops;
    bool     persisting;    /* false once the store has given up for good */
    int      sector;        /* the sector being appended to */
    uint32_t generation;
    int      next_slot;
    bool     spare_erased;  /* the other sector is blank and ready to receive */
    int      failures;      /* consecutive flash failures */
    Counters counters;      /* what flash last confirmed */
};

/* Reads both sectors and settles on the newest coherent state. Returns false
 * if nothing could be established and nothing could be bootstrapped, which
 * leaves persisting false and the counters at zero. */
bool open(Store* s, const Ops& ops);

/* Appends one record. Returns false without losing anything when the sector
 * is full and the spare has not been erased yet — the caller keeps counting
 * in RAM and tries again once a quiet moment has allowed the erase. */
bool append(Store* s, const Counters& c);

/* True when the next append will need the spare, so the caller knows to look
 * for a quiet moment to erase it in. */
bool needsSpare(const Store* s);

/* Erases the other sector. Refuses, always, to erase the sector holding the
 * newest data. Call only at a moment when stalling the lighting is harmless. */
bool prepareSpare(Store* s);

/* Zeroes the counters by committing a fresh generation, so that a power cut
 * anywhere inside a reset either leaves the old counters or the new zeros,
 * and can never resurrect what was cleared. Erases as it needs to: a reset is
 * an explicit action taken with the module parked, never while riding. */
bool reset(Store* s);

} // namespace StatsStore

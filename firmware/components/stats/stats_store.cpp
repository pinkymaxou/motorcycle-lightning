#include "stats_store.h"

#include <cstring>

namespace StatsStore
{

using namespace StatsRecord;

namespace
{

/* Bridges one sector into the sector scanner, which addresses slots without
 * knowing which sector it is walking. */
struct SectorView
{
    const Ops* ops;
    int        sector;
};

bool readThroughView(void* ctx, const int index, uint8_t* out)
{
    SectorView* v = static_cast<SectorView*>(ctx);
    return v->ops->readSlot(v->ops->ctx, v->sector, index, out);
}

int otherSector(const int sector)
{
    return (SECTOR_COUNT - 1) - sector;
}

/* One more flash operation has failed. Past the allowance the store stops for
 * good: it is a diagnostic, and a diagnostic that keeps hammering a failing
 * sector is worse than one that stops and says so. */
void noteFailure(Store* s)
{
    s->failures++;
    if (s->failures >= FAILURES_ALLOWED)
    {
        s->persisting = false;
    }
}

/* Commits a new generation into the already-erased spare by writing its
 * header, and only then moves the write position there. Until the first
 * record lands, the previous sector still holds every counter — which is
 * exactly what a power cut in the middle of this leaves behind. */
bool commitNextGeneration(Store* s)
{
    if (!s->spare_erased)
    {
        return false;
    }
    if (s->generation >= GENERATION_MAX)
    {
        s->persisting = false;   /* ~2000 years in; stop rather than wrap */
        return false;
    }

    const int spare = otherSector(s->sector);
    /* The invariant, as a runtime check rather than an assert: this build
     * compiles asserts out. Never erase or claim the sector that holds the
     * newest data. */
    if (spare == s->sector)
    {
        s->persisting = false;
        return false;
    }

    const uint32_t next_generation = s->generation + 1;
    uint8_t slot[SLOT_BYTES];
    encodeHeader(slot, next_generation);
    if (!s->ops.writeSlot(s->ops.ctx, spare, HEADER_SLOT, slot))
    {
        noteFailure(s);
        return false;
    }

    uint32_t written = 0;
    if (!s->ops.readSlot(s->ops.ctx, spare, HEADER_SLOT, slot) ||
        !headerValid(slot, &written) || written != next_generation)
    {
        noteFailure(s);
        return false;
    }

    s->failures = 0;
    s->sector = spare;
    s->generation = next_generation;
    s->next_slot = FIRST_RECORD_SLOT;
    s->spare_erased = false;
    return true;
}

} // namespace

bool open(Store* s, const Ops& ops)
{
    std::memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->next_slot = FIRST_RECORD_SLOT;

    ScanResult scan[SECTOR_COUNT];
    for (int i = 0; i < SECTOR_COUNT; i++)
    {
        SectorView view = { &s->ops, i };
        scanSector(readThroughView, &view, &scan[i]);
    }

    int winner = -1;
    for (int i = 0; i < SECTOR_COUNT; i++)
    {
        if (!scan[i].header_ok)
        {
            continue;
        }
        if (winner < 0 || scan[i].generation > scan[winner].generation)
        {
            winner = i;
        }
    }

    if (winner < 0)
    {
        /* Nothing readable anywhere — a virgin module, or a partition wiped
         * by a factory reset. Start at generation 0 with no room, so the
         * first append takes the ordinary roll-over path and no special
         * bootstrap exists to go wrong. */
        s->sector = 0;
        s->generation = 0;
        s->next_slot = SLOTS_PER_SECTOR;
        s->spare_erased = scan[otherSector(0)].blank;
        s->persisting = true;
        return true;
    }

    const int other = otherSector(winner);
    s->sector = winner;
    s->generation = scan[winner].generation;
    s->next_slot = scan[winner].next_slot;
    s->spare_erased = scan[other].blank;
    s->persisting = true;

    if (scan[winner].have_record)
    {
        s->counters = scan[winner].counters;
    }
    else if (scan[other].have_record)
    {
        /* A header with no record is what a power cut at the roll-over commit
         * point leaves. The previous sector was never touched, so resume from
         * it: at most one interval is lost. */
        s->counters = scan[other].counters;
    }
    return true;
}

bool needsSpare(const Store* s)
{
    return s->persisting && !s->spare_erased;
}

bool prepareSpare(Store* s)
{
    if (!s->persisting)
    {
        return false;
    }
    if (s->spare_erased)
    {
        return true;
    }

    const int spare = otherSector(s->sector);
    if (spare == s->sector)
    {
        s->persisting = false;
        return false;
    }
    if (!s->ops.eraseSector(s->ops.ctx, spare))
    {
        noteFailure(s);
        return false;
    }
    s->failures = 0;
    s->spare_erased = true;
    return true;
}

bool append(Store* s, const Counters& c)
{
    if (!s->persisting)
    {
        return false;
    }

    if (s->next_slot >= SLOTS_PER_SECTOR && !commitNextGeneration(s))
    {
        /* Either the spare is not erased yet or the commit failed. Nothing is
         * lost: the caller goes on counting in RAM and tries again. */
        return false;
    }

    uint8_t slot[SLOT_BYTES];
    while (s->next_slot < SLOTS_PER_SECTOR)
    {
        if (!s->ops.readSlot(s->ops.ctx, s->sector, s->next_slot, slot))
        {
            noteFailure(s);
            return false;
        }
        if (slotErased(slot))
        {
            break;
        }
        s->next_slot++;   /* a torn slot is never programmed a second time */
    }
    if (s->next_slot >= SLOTS_PER_SECTOR)
    {
        return false;     /* the next call rolls over */
    }

    encodeRecord(slot, s->generation, c);
    if (!s->ops.writeSlot(s->ops.ctx, s->sector, s->next_slot, slot))
    {
        s->next_slot++;
        noteFailure(s);
        return false;
    }

    /* Read back: this build has no flash write verification of its own, and a
     * sector that has stopped taking programs must be found out here rather
     * than at the next boot. */
    Counters back;
    if (!s->ops.readSlot(s->ops.ctx, s->sector, s->next_slot, slot) ||
        !recordValid(slot, s->generation, &back) ||
        0 != std::memcmp(&back, &c, sizeof(c)))
    {
        s->next_slot++;
        noteFailure(s);
        return false;
    }

    s->failures = 0;
    s->counters = c;
    s->next_slot++;
    return true;
}

bool reset(Store* s)
{
    if (!s->persisting)
    {
        return false;
    }
    /* A reset is done with the module parked and the config WiFi up, so it may
     * erase straight away instead of waiting for a quiet moment. */
    if (!prepareSpare(s))
    {
        return false;
    }
    if (!commitNextGeneration(s))
    {
        return false;
    }

    const Counters zeroed = {};
    if (!append(s, zeroed))
    {
        return false;
    }
    /* The superseded sector still holds the old counters and is erased later,
     * at a quiet moment. Until then it is simply an older generation, which
     * never wins. */
    return true;
}

} // namespace StatsStore

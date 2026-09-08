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
    const SectorView* const v = static_cast<const SectorView*>(ctx);
    return v->ops->readSlot(v->ops->ctx, v->sector, index, out);
}

/* The sector that may be erased: the one NOT holding the newest data. The
 * invariant everything rests on, as a runtime check rather than an assert,
 * because this build compiles asserts out. A sector index outside the two
 * can only come from corrupted state, and then nothing may be erased. */
bool spareOf(Store* s, int* spare)
{
    if (s->sector < 0 || s->sector >= SECTOR_COUNT)
    {
        s->persisting = false;
        return false;
    }
    *spare = (SECTOR_COUNT - 1) - s->sector;
    return true;
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

    int spare = 0;
    if (!spareOf(s, &spare))
    {
        return false;
    }

    const uint32_t next_generation = s->generation + 1;
    uint8_t slot[SLOT_BYTES];
    encodeHeader(slot, next_generation);
    /* From here the header slot may hold bits: whatever happens, the spare is
     * no longer blank and must be erased again before another attempt. */
    s->spare_erased = false;
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
    return true;
}

} // namespace

void open(Store* s, const Ops& ops)
{
    std::memset(s, 0, sizeof(*s));
    s->ops = ops;

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
        return;
    }

    const int other = otherSector(winner);
    s->sector = winner;
    s->generation = scan[winner].generation;
    s->next_slot = scan[winner].next_slot;
    s->spare_erased = scan[other].blank;
    s->persisting = true;

    /* Only the sector the counters actually come from can have rejections
     * worth counting: the other one is history, already accounted for. */
    uint32_t rejected = scan[winner].rejected;
    if (scan[winner].have_record)
    {
        s->counters = scan[winner].counters;
        if (rejected > 0)
        {
            /* Leave no room here: the next write must open a fresh
             * generation, so the rejected record is never anyone's
             * predecessor again. */
            s->next_slot = SLOTS_PER_SECTOR;
        }
    }
    else if (scan[other].have_record)
    {
        /* A header with no record is what a power cut at the roll-over commit
         * point leaves. The previous sector was never touched, so resume from
         * it: at most one interval is lost. This sector is already the fresh
         * generation, so nothing more needs forcing. */
        s->counters = scan[other].counters;
        rejected += scan[other].rejected;
    }
    s->counters.anomalies += rejected;
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

    int spare = 0;
    if (!spareOf(s, &spare))
    {
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

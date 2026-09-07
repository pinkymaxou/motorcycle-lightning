#include "stats_record.h"

#include <cstring>

namespace StatsRecord
{

static_assert(sizeof(Counters) == COUNTER_WORDS * 4,
              "Counters must be a packed run of 32-bit words");
static_assert(SLOT_WORDS == 1 + 1 + COUNTER_WORDS + 1 + 1,
              "record layout must fill the slot exactly: magic, generation, "
              "counters, reserved, crc");

namespace
{

constexpr uint32_t CRC32_POLY = 0xEDB88320;
/* Record layout, in words: magic, generation, the counters, reserved, crc. */
constexpr uint32_t GENERATION_WORD = 1;
constexpr uint32_t COUNTERS_WORD = 2;
constexpr uint32_t CRC_WORD = SLOT_WORDS - 1;
constexpr uint32_t CRC_COVERAGE_BYTES = CRC_WORD * 4;   /* 60 */
/* Header layout: magic, format version, generation, reserved, crc. */
constexpr uint32_t FORMAT_WORD = 1;
constexpr uint32_t HEADER_GENERATION_WORD = 2;

/* Word accessors, little-endian on both the device and the host so a record
 * written by one is read the same way by the other. */
uint32_t getWord(const uint8_t* slot, const uint32_t index)
{
    const uint8_t* const p = slot + index * 4;
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void putWord(uint8_t* slot, const uint32_t index, const uint32_t value)
{
    uint8_t* const p = slot + index * 4;
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

void seal(uint8_t* slot)
{
    putWord(slot, CRC_WORD, crc32(slot, CRC_COVERAGE_BYTES));
}

bool sealIntact(const uint8_t* slot)
{
    return crc32(slot, CRC_COVERAGE_BYTES) == getWord(slot, CRC_WORD);
}

/* An increase of `grown` is only believable if it fits in `seconds` at
 * `per_second`. Zero elapsed seconds still allows one tick's worth, because a
 * flush forced by a reboot or by the WiFi going down can land before the next
 * tick has been counted. */
bool fits(const uint32_t grown, const uint32_t seconds, const uint32_t per_second)
{
    return grown <= (seconds + TICK_SECONDS) * per_second;
}

} // namespace

uint32_t crc32(const uint8_t* data, const size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            const uint32_t mask = (0 != (crc & 1)) ? CRC32_POLY : 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

bool slotErased(const uint8_t* slot)
{
    for (uint32_t i = 0; i < SLOT_WORDS; i++)
    {
        if (ERASED_WORD != getWord(slot, i))
        {
            return false;
        }
    }
    return true;
}

bool headerValid(const uint8_t* slot, uint32_t* generation)
{
    if (HEADER_MAGIC != getWord(slot, 0))
    {
        return false;
    }
    if (FORMAT_VERSION != getWord(slot, FORMAT_WORD))
    {
        return false;
    }
    const uint32_t gen = getWord(slot, HEADER_GENERATION_WORD);
    if (0 == gen || gen > GENERATION_MAX)
    {
        return false;
    }
    if (!sealIntact(slot))
    {
        return false;
    }
    *generation = gen;
    return true;
}

void encodeHeader(uint8_t* slot, const uint32_t generation)
{
    std::memset(slot, 0, SLOT_BYTES);
    putWord(slot, 0, HEADER_MAGIC);
    putWord(slot, FORMAT_WORD, FORMAT_VERSION);
    putWord(slot, HEADER_GENERATION_WORD, generation);
    seal(slot);
}

bool recordValid(const uint8_t* slot, const uint32_t generation, Counters* out)
{
    if (RECORD_MAGIC != getWord(slot, 0))
    {
        return false;
    }
    /* A record carries its sector's generation, so one left behind by an
     * interrupted erase cannot be read as belonging to the current one. */
    if (generation != getWord(slot, GENERATION_WORD))
    {
        return false;
    }
    if (!sealIntact(slot))
    {
        return false;
    }
    if (nullptr != out)
    {
        uint32_t words[COUNTER_WORDS];
        for (uint32_t i = 0; i < COUNTER_WORDS; i++)
        {
            words[i] = getWord(slot, COUNTERS_WORD + i);
        }
        std::memcpy(out, words, sizeof(*out));
    }
    return true;
}

void encodeRecord(uint8_t* slot, const uint32_t generation, const Counters& c)
{
    std::memset(slot, 0, SLOT_BYTES);
    putWord(slot, 0, RECORD_MAGIC);
    putWord(slot, GENERATION_WORD, generation);
    uint32_t words[COUNTER_WORDS];
    std::memcpy(words, &c, sizeof(c));
    for (uint32_t i = 0; i < COUNTER_WORDS; i++)
    {
        putWord(slot, COUNTERS_WORD + i, words[i]);
    }
    seal(slot);
}

bool plausible(const Counters& older, const Counters& newer)
{
    uint32_t a[COUNTER_WORDS];
    uint32_t b[COUNTER_WORDS];
    std::memcpy(a, &older, sizeof(a));
    std::memcpy(b, &newer, sizeof(b));

    /* Nothing the module counts can ever run backwards. NOR flash only fails
     * upwards, so this alone catches most of what a torn erase can do. */
    for (uint32_t i = 0; i < COUNTER_WORDS; i++)
    {
        if (b[i] < a[i])
        {
            return false;
        }
    }

    /* The clock is the yardstick for everything else, so it is bounded first:
     * a clock word that rotted toward all-ones would otherwise make any jump
     * in the other counters look reasonable. */
    const uint32_t gap = newer.powered_15s - older.powered_15s;
    if (gap > MAX_GAP_TICKS)
    {
        return false;
    }
    if (newer.boots - older.boots > MAX_BOOTS_PER_RECORD)
    {
        return false;
    }
    if (newer.anomalies - older.anomalies > MAX_ANOMALIES_PER_RECORD)
    {
        return false;
    }

    /* Every event counter is then tied to that clock. */
    const uint32_t elapsed_s = gap * TICK_SECONDS;
    if (newer.wifi_15s > newer.powered_15s)
    {
        return false;
    }
    if (!fits(newer.brake - older.brake, elapsed_s, MAX_BRAKE_PER_S))
    {
        return false;
    }
    if (!fits(newer.aux - older.aux, elapsed_s, MAX_AUX_PER_S))
    {
        return false;
    }
    if (!fits(newer.left_uses - older.left_uses, elapsed_s, MAX_USES_PER_S) ||
        !fits(newer.right_uses - older.right_uses, elapsed_s, MAX_USES_PER_S) ||
        !fits(newer.hazard_uses - older.hazard_uses, elapsed_s, MAX_USES_PER_S))
    {
        return false;
    }
    if (!fits(newer.left_flashes - older.left_flashes, elapsed_s, MAX_FLASHES_PER_S) ||
        !fits(newer.right_flashes - older.right_flashes, elapsed_s, MAX_FLASHES_PER_S) ||
        !fits(newer.hazard_flashes - older.hazard_flashes, elapsed_s, MAX_FLASHES_PER_S))
    {
        return false;
    }
    return true;
}

void scanSector(const SlotReader read, void* ctx, ScanResult* out)
{
    std::memset(out, 0, sizeof(*out));
    out->next_slot = FIRST_RECORD_SLOT;

    uint8_t slot[SLOT_BYTES];
    bool header_erased = false;
    if (read(ctx, HEADER_SLOT, slot))
    {
        header_erased = slotErased(slot);
        out->header_ok = headerValid(slot, &out->generation);
    }

    /* Backwards to the last slot that is not blank. Everything above it is
     * blank by construction, so that is where the next record goes — and a
     * hole further down, which a marginal erase can leave, is never written
     * into. A torn slot is occupied, not free: it is skipped over, never
     * reused. */
    int last_used = -1;
    for (int i = SLOTS_PER_SECTOR - 1; i >= FIRST_RECORD_SLOT; i--)
    {
        if (!read(ctx, i, slot))
        {
            continue;
        }
        if (!slotErased(slot))
        {
            last_used = i;
            break;
        }
    }
    out->blank = header_erased && (last_used < 0);
    if (!out->header_ok)
    {
        /* No proof the erase ever completed: nothing here may be trusted, and
         * nothing may be written until the sector is erased again. */
        return;
    }
    if (last_used < 0)
    {
        return;   /* erased and headed: empty, ready at the first record slot */
    }
    out->next_slot = last_used + 1;

    /* The newest record that is both valid and coherent with the one before
     * it. A candidate that fails plausibility is dropped and its predecessor
     * becomes the candidate. */
    Counters candidate;
    int candidate_slot = -1;
    for (int i = last_used; i >= FIRST_RECORD_SLOT; i--)
    {
        Counters here;
        if (!read(ctx, i, slot) || !recordValid(slot, out->generation, &here))
        {
            continue;
        }
        if (candidate_slot < 0)
        {
            candidate = here;
            candidate_slot = i;
            continue;
        }
        if (plausible(here, candidate))
        {
            break;   /* the pair agrees: the candidate stands */
        }
        candidate = here;   /* it did not: fall back and keep looking */
        candidate_slot = i;
        out->rejected++;
    }
    if (candidate_slot >= 0)
    {
        out->have_record = true;
        out->counters = candidate;
    }
}

} // namespace StatsRecord

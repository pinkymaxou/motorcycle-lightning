/* Host unit tests for the ride-counter store.
 *
 * The flash underneath is simulated with real NOR semantics — erase sets all
 * bits, a program can only clear them — so a torn write in this test behaves
 * the way a torn write behaves on the bike: the slot reads back as a
 * bit-superset of what was meant, never as a clean prefix. */
#include <cstdio>
#include <cstring>

#include "stats_record.h"
#include "stats_store.h"

using namespace StatsRecord;

namespace
{

int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) \
    { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

struct Flash
{
    uint8_t mem[SECTOR_COUNT][SECTOR_BYTES];
    /* Writes left before power is cut, -1 for a healthy supply. */
    int  writes_to_tear;
    /* How many bytes of the interrupted write actually land. */
    int  tear_bytes;
    /* Erases left before power is cut, -1 for a healthy supply. */
    int  erases_to_tear;
    bool dead;
};

void flashErase(Flash* f)
{
    std::memset(f->mem, 0xFF, sizeof(f->mem));
    f->writes_to_tear = -1;
    f->erases_to_tear = -1;
    f->tear_bytes = 0;
    f->dead = false;
}

bool readSlot(void* ctx, const int sector, const int index, uint8_t* out)
{
    Flash* f = static_cast<Flash*>(ctx);
    if (f->dead)
    {
        return false;
    }
    std::memcpy(out, &f->mem[sector][index * SLOT_BYTES], SLOT_BYTES);
    return true;
}

bool writeSlot(void* ctx, const int sector, const int index, const uint8_t* in)
{
    Flash* f = static_cast<Flash*>(ctx);
    if (f->dead)
    {
        return false;
    }
    int landed = SLOT_BYTES;
    bool cut = false;
    if (0 == f->writes_to_tear)
    {
        landed = f->tear_bytes;
        cut = true;
    }
    else if (f->writes_to_tear > 0)
    {
        f->writes_to_tear--;
    }
    uint8_t* dst = &f->mem[sector][index * SLOT_BYTES];
    for (int i = 0; i < landed; i++)
    {
        dst[i] &= in[i];   /* programming only clears bits */
    }
    if (cut)
    {
        f->writes_to_tear = -1;   /* the cut fires once */
        f->dead = true;           /* the module has lost power */
        return false;
    }
    return true;
}

bool eraseSector(void* ctx, const int sector)
{
    Flash* f = static_cast<Flash*>(ctx);
    if (f->dead)
    {
        return false;
    }
    if (0 == f->erases_to_tear)
    {
        /* An interrupted erase leaves the sector indeterminate: some slots
         * blank, others still holding what was there. */
        std::memset(&f->mem[sector][0], 0xFF, SECTOR_BYTES / 2);
        f->erases_to_tear = -1;
        f->dead = true;
        return false;
    }
    if (f->erases_to_tear > 0)
    {
        f->erases_to_tear--;
    }
    std::memset(&f->mem[sector][0], 0xFF, SECTOR_BYTES);
    return true;
}

StatsStore::Ops opsFor(Flash* f)
{
    StatsStore::Ops ops = {};
    ops.readSlot = readSlot;
    ops.writeSlot = writeSlot;
    ops.eraseSector = eraseSector;
    ops.ctx = f;
    return ops;
}

/* One flush: bumps the clock by a tick and a few events, the way the module
 * does. */
Counters advance(const Counters& c, const uint32_t brakes = 1)
{
    Counters n = c;
    n.powered_15s++;
    n.brake += brakes;
    return n;
}

/* Runs the store the way the housekeeping loop does: erase the spare when it
 * is wanted, then append. */
bool flush(StatsStore::Store* s, const Counters& c)
{
    if (StatsStore::needsSpare(s))
    {
        StatsStore::prepareSpare(s);
    }
    return StatsStore::append(s, c);
}

void testCrcAndBlankFlash()
{
    uint8_t blank[SLOT_BYTES];
    std::memset(blank, 0xFF, sizeof(blank));

    /* Pinned: the CRC of blank flash is not the value blank flash holds in
     * its CRC word, so an erased slot can never validate on its own. */
    CHECK(0xF48CF14D == crc32(blank, SLOT_BYTES - 4));
    CHECK(crc32(blank, SLOT_BYTES - 4) != 0xFFFFFFFF);

    uint32_t generation = 0;
    CHECK(!headerValid(blank, &generation));
    CHECK(!recordValid(blank, 1, nullptr));
    CHECK(slotErased(blank));

    uint8_t slot[SLOT_BYTES];
    encodeHeader(slot, 7);
    CHECK(headerValid(slot, &generation));
    CHECK(7 == generation);
    CHECK(!slotErased(slot));

    Counters c = {};
    c.brake = 42;
    c.powered_15s = 9;
    Counters back = {};
    encodeRecord(slot, 7, c);
    CHECK(recordValid(slot, 7, &back));
    CHECK(42 == back.brake && 9 == back.powered_15s);
    /* A record only counts inside the generation it was written for. */
    CHECK(!recordValid(slot, 8, nullptr));
}

void testTornSlotIsOccupiedNotFree()
{
    Counters c = {};
    c.brake = 5;
    uint8_t whole[SLOT_BYTES];
    encodeRecord(whole, 1, c);

    /* Torn with only the first word programmed. */
    uint8_t head[SLOT_BYTES];
    std::memset(head, 0xFF, sizeof(head));
    for (int i = 0; i < 4; i++)
    {
        head[i] &= whole[i];
    }
    CHECK(!slotErased(head));
    CHECK(!recordValid(head, 1, nullptr));

    /* Torn the dangerous way round: the ordering word still reads blank while
     * later words have bits cleared. Testing only the first word would call
     * this free and program it a second time. */
    uint8_t tail[SLOT_BYTES];
    std::memset(tail, 0xFF, sizeof(tail));
    for (size_t i = 4; i < SLOT_BYTES; i++)
    {
        tail[i] &= whole[i];
    }
    CHECK(!slotErased(tail));
    CHECK(!recordValid(tail, 1, nullptr));
}

void testPlausibility()
{
    Counters a = {};
    a.powered_15s = 100;
    a.brake = 10;
    a.left_flashes = 40;
    a.wifi_15s = 5;

    Counters b = advance(a);
    CHECK(plausible(a, b));

    /* A counter that ran backwards. */
    Counters back = b;
    back.brake = 9;
    CHECK(!plausible(a, back));

    /* A jump no amount of riding could produce in one interval. */
    Counters jump = b;
    jump.brake = a.brake + 500;
    CHECK(!plausible(a, jump));

    /* Corruption pushing bits up, which is the only way NOR fails. */
    Counters rot = b;
    rot.left_flashes = 0xFFFFFFFF;
    CHECK(!plausible(a, rot));

    /* WiFi time can never exceed powered time. */
    Counters impossible = b;
    impossible.wifi_15s = impossible.powered_15s + 1;
    CHECK(!plausible(a, impossible));

    /* A flush skipped for fifty intervals, then resumed: the events fit in
     * the elapsed time, so this must be accepted. */
    Counters skipped = a;
    skipped.powered_15s += 50;
    skipped.brake += 50;
    skipped.left_flashes += 200;
    CHECK(plausible(a, skipped));

    /* A forced flush with no time elapsed still allows one tick's events. */
    Counters forced = a;
    forced.brake += 1;
    CHECK(plausible(a, forced));
}

void testEmptyThenFillsAndRollsOver()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));
    CHECK(s.persisting);

    Counters c = {};
    /* Exactly fills the first sector, then rolls into the other. */
    for (int i = 0; i < RECORDS_PER_SECTOR; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    const int first_sector = s.sector;
    const uint32_t first_generation = s.generation;

    c = advance(c);
    CHECK(flush(&s, c));
    CHECK(s.sector != first_sector);
    CHECK(s.generation == first_generation + 1);

    /* And the sector just left still holds its records: it is not erased
     * until a quiet moment allows it. */
    uint8_t slot[SLOT_BYTES];
    CHECK(readSlot(&f, first_sector, HEADER_SLOT, slot));
    uint32_t generation = 0;
    CHECK(headerValid(slot, &generation));
    CHECK(generation == first_generation);

    /* Reopening lands on the newer generation with every count intact. */
    StatsStore::Store again;
    CHECK(StatsStore::open(&again, opsFor(&f)));
    CHECK(again.counters.brake == c.brake);
    CHECK(again.counters.powered_15s == c.powered_15s);
    CHECK(again.generation == s.generation);
}

void testTornRecordCostsOneIntervalAtMost()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < 10; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    const Counters survived = c;

    /* Power cut half way through the eleventh record. */
    f.writes_to_tear = 0;
    f.tear_bytes = SLOT_BYTES / 2;
    c = advance(c);
    CHECK(!flush(&s, c));

    f.dead = false;   /* the ignition comes back on */
    StatsStore::Store after;
    CHECK(StatsStore::open(&after, opsFor(&f)));
    CHECK(after.persisting);
    CHECK(after.counters.brake == survived.brake);
    CHECK(after.counters.powered_15s == survived.powered_15s);

    /* The torn slot is never programmed a second time. */
    const int torn_slot = after.next_slot - 1;
    uint8_t slot[SLOT_BYTES];
    CHECK(readSlot(&f, after.sector, torn_slot, slot));
    CHECK(!slotErased(slot));
    CHECK(!recordValid(slot, after.generation, nullptr));

    c = advance(c);
    CHECK(flush(&after, c));
    CHECK(after.counters.brake == c.brake);
}

void testTornEraseNeverTouchesTheLiveSector()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < RECORDS_PER_SECTOR; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    /* The sector has just filled and rolled over, so the superseded one is
     * still waiting to be erased. */
    c = advance(c);
    CHECK(flush(&s, c));
    CHECK(StatsStore::needsSpare(&s));
    const int live_sector = s.sector;
    const uint32_t live_generation = s.generation;

    /* Power cut in the middle of that erase — the longest window in the
     * design, and the one the alternation exists for. The record still lands,
     * because the sector being written to is not the one being erased. */
    f.erases_to_tear = 0;
    c = advance(c);
    CHECK(!StatsStore::prepareSpare(&s));

    f.dead = false;
    StatsStore::Store after;
    CHECK(StatsStore::open(&after, opsFor(&f)));
    CHECK(after.persisting);
    CHECK(after.sector == live_sector);
    CHECK(after.generation == live_generation);
    CHECK(after.counters.brake == c.brake - 1);
    CHECK(after.counters.powered_15s == c.powered_15s - 1);

    /* The half-erased sector cannot win: its header went with the erase. */
    uint8_t slot[SLOT_BYTES];
    uint32_t generation = 0;
    CHECK(readSlot(&f, (SECTOR_COUNT - 1) - live_sector, HEADER_SLOT, slot));
    CHECK(!headerValid(slot, &generation));

    /* And the module recovers by erasing it properly. */
    CHECK(StatsStore::needsSpare(&after));
    CHECK(StatsStore::prepareSpare(&after));
    c = advance(c);
    CHECK(flush(&after, c));
    CHECK(after.counters.brake == c.brake);
}

void testTornAtTheRollOverCommitPoint()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < RECORDS_PER_SECTOR; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    const Counters survived = c;
    const int live_sector = s.sector;

    /* The spare is erased and headed, then power dies before its first record
     * lands: a sector with a valid header, a higher generation, and nothing
     * in it. */
    CHECK(StatsStore::prepareSpare(&s));
    f.writes_to_tear = 1;          /* the header goes in, the record does not */
    f.tear_bytes = SLOT_BYTES / 3;
    c = advance(c);
    CHECK(!StatsStore::append(&s, c));

    f.dead = false;
    StatsStore::Store after;
    CHECK(StatsStore::open(&after, opsFor(&f)));
    CHECK(after.persisting);
    CHECK(after.sector != live_sector);          /* the new generation is current */
    CHECK(after.counters.brake == survived.brake);  /* but the data came from the old one */
    CHECK(after.counters.powered_15s == survived.powered_15s);

    c = advance(c);
    CHECK(flush(&after, c));
    CHECK(after.counters.brake == c.brake);
}

void testHigherGenerationWinsOverAStaleSector()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < RECORDS_PER_SECTOR + 5; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    /* Both sectors now carry valid headers and records; the newer generation
     * has fewer records but must still win. */
    StatsStore::Store after;
    CHECK(StatsStore::open(&after, opsFor(&f)));
    CHECK(after.counters.brake == c.brake);
    CHECK(after.generation == s.generation);
}

void testResetCannotResurrect()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < 20; i++)
    {
        c = advance(c);
        CHECK(flush(&s, c));
    }
    CHECK(4102 != c.brake);

    /* A reset that completes, then the ignition is killed straight away. */
    CHECK(StatsStore::reset(&s));
    StatsStore::Store after;
    CHECK(StatsStore::open(&after, opsFor(&f)));
    CHECK(0 == after.counters.brake);
    CHECK(0 == after.counters.powered_15s);

    /* A reset cut before its zeroed record commits: the old counters stand,
     * which is the reset not taking — never a resurrection of something the
     * rider had already cleared. */
    Flash g;
    flashErase(&g);
    StatsStore::Store t;
    CHECK(StatsStore::open(&t, opsFor(&g)));
    Counters d = {};
    for (int i = 0; i < 20; i++)
    {
        d = advance(d);
        CHECK(flush(&t, d));
    }
    g.writes_to_tear = 1;         /* the new header lands, the zeros do not */
    g.tear_bytes = SLOT_BYTES / 3;
    CHECK(!StatsStore::reset(&t));

    g.dead = false;
    StatsStore::Store cut;
    CHECK(StatsStore::open(&cut, opsFor(&g)));
    CHECK(cut.counters.brake == d.brake);

    /* Retrying the reset on the module that came back still works. */
    CHECK(StatsStore::reset(&cut));
    StatsStore::Store settled;
    CHECK(StatsStore::open(&settled, opsFor(&g)));
    CHECK(0 == settled.counters.brake);
}

void testNeverErasesTheSectorHoldingTheData()
{
    Flash f;
    flashErase(&f);
    StatsStore::Store s;
    CHECK(StatsStore::open(&s, opsFor(&f)));

    Counters c = {};
    for (int i = 0; i < 200; i++)
    {
        c = advance(c);
        flush(&s, c);
        /* At every step the sector being written to still holds its header,
         * and its own records are never the ones erased. */
        uint8_t slot[SLOT_BYTES];
        uint32_t generation = 0;
        CHECK(readSlot(&f, s.sector, HEADER_SLOT, slot));
        CHECK(headerValid(slot, &generation));
        CHECK(generation == s.generation);
    }
    CHECK(s.counters.brake == c.brake);
}

void testAPowerCutEveryFewRecords()
{
    /* The realistic case: a short ride, ignition off, repeat. Nothing may
     * ever come back wrong, and never more than one interval behind. */
    Flash f;
    flashErase(&f);
    Counters c = {};
    Counters confirmed = {};

    for (int ride = 0; ride < 40; ride++)
    {
        StatsStore::Store s;
        CHECK(StatsStore::open(&s, opsFor(&f)));
        CHECK(s.counters.brake == confirmed.brake);
        c = s.counters;

        for (int i = 0; i < 7; i++)
        {
            c = advance(c);
            if (flush(&s, c))
            {
                confirmed = c;
            }
        }
        /* The ignition is cut part way through the next write. */
        f.writes_to_tear = 0;
        f.tear_bytes = 17 + (ride % 40);
        c = advance(c);
        flush(&s, c);
        f.dead = false;
    }
    CHECK(confirmed.brake > 200);
}

} // namespace

int main()
{
    testCrcAndBlankFlash();
    testTornSlotIsOccupiedNotFree();
    testPlausibility();
    testEmptyThenFillsAndRollsOver();
    testTornRecordCostsOneIntervalAtMost();
    testTornEraseNeverTouchesTheLiveSector();
    testTornAtTheRollOverCommitPoint();
    testHigherGenerationWinsOverAStaleSector();
    testResetCannotResurrect();
    testNeverErasesTheSectorHoldingTheData();
    testAPowerCutEveryFewRecords();

    if (0 != g_fail)
    {
        std::printf("stats tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("stats tests: all passed\n");
    return 0;
}

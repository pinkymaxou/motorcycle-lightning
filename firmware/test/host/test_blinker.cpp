/* Host unit tests for the blinker tracker state machine. */
#include <cstdio>

#include "blinker.h"

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

/* Advance the system tick-by-tick with fixed raw levels. */
uint32_t runMs(Blink::BlinkSystem* s, uint32_t now, const uint32_t ms,
               const bool l, const bool r, const bool b, const bool a)
{
    for (uint32_t i = 0; i < ms; i++)
    {
        now++;
        Blink::tick(s, l, r, b, a, now);
    }
    return now;
}

/* Simulate n flasher cycles on the left channel: on_ms ON, off_ms OFF. */
uint32_t pulseLeft(Blink::BlinkSystem* s, uint32_t now, const int n,
                   const uint32_t on_ms, const uint32_t off_ms)
{
    for (int i = 0; i < n; i++)
    {
        now = runMs(s, now, on_ms, true, false, false, false);
        now = runMs(s, now, off_ms, false, false, false, false);
    }
    return now;
}

void testDebounceGlitch()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    /* 3 ms glitch: below the 5-sample threshold, must not enter blink mode */
    now = runMs(&s, now, 3, true, false, false, false);
    now = runMs(&s, now, 20, false, false, false, false);
    CHECK(!s.left.blink_mode);
    CHECK(!s.left.debounced);
}

void testEnterAndPhase()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    now = runMs(&s, now, 10, true, false, false, false);
    CHECK(s.left.blink_mode);          /* first flash enters blink mode */
    CHECK(s.left.debounced);           /* ON phase visible */
    CHECK(!s.right.blink_mode);
    CHECK(Blink::PERIOD_DEFAULT_MS == s.period_ms); /* not learned yet */
    CHECK(s.left.last_phase_edge_ms > 1000 && s.left.last_phase_edge_ms <= now);
}

void testLearnAndPersistFlag()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    /* 700 ms period: 350 on / 350 off, 4 cycles = 3 measured intervals */
    now = pulseLeft(&s, now, 4, 350, 350);
    CHECK(s.learned);
    CHECK(s.period_dirty);
    CHECK(s.period_ms > 660 && s.period_ms < 740);
    CHECK(s.left.blink_mode);
    (void)now;
}

void testExitAfterGrace()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 15); /* stored period 700, 1.5x -> exit at 1050 ms */
    uint32_t now = 1000;
    now = runMs(&s, now, 100, true, false, false, false);  /* one flash */
    now = runMs(&s, now, 900, false, false, false, false);
    CHECK(s.left.blink_mode);   /* 900 < 1050 since last ON edge... */
    now = runMs(&s, now, 300, false, false, false, false);
    CHECK(!s.left.blink_mode);  /* ...but 1200 > 1050: exited */
}

void testNeverExitWhileOn()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 15);
    uint32_t now = 1000;
    /* signal stuck ON for 5 s: stay in blink mode the whole time */
    now = runMs(&s, now, 5000, true, false, false, false);
    CHECK(s.left.blink_mode);
    CHECK(s.left.debounced);
    /* releases: exits after the grace window */
    now = runMs(&s, now, 1100, false, false, false, false);
    CHECK(!s.left.blink_mode);
}

void testStoredPeriodUsed()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 500, 15);
    CHECK(s.learned);
    CHECK(500 == s.period_ms);
    CHECK(!s.period_dirty);
    /* default exit factor: period + 20% grace */
    Blink::init(&s, 500, 0);
    CHECK(Blink::EXIT_X10_DEFAULT == s.exit_x10);
    CHECK(12 == s.exit_x10);
}

void testRelearnFastFlash()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 15);
    s.period_dirty = false;
    uint32_t now = 1000;
    /* bulb-out fast flash: 350 ms period */
    now = pulseLeft(&s, now, 5, 175, 175);
    CHECK(s.period_ms > 320 && s.period_ms < 380);
    CHECK(s.period_dirty); /* >10% change -> persist again */
    (void)now;
}

/* A flasher slower than the estimate x exit factor drops out of blink mode
 * before its next ON edge; it must still be measured, or the module never
 * recovers from a wrong stored period (factory default on a slow bike, or a
 * hyperflash learned before the bulb was replaced). */
void testRelearnSlowFlash()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 750, 12);          /* factory: 750 ms, +20 % grace */
    s.period_dirty = false;
    uint32_t now = 1000;
    /* legal 60 flashes/min: 1000 ms period, beyond 750 x 1.2 = 900 */
    now = pulseLeft(&s, now, 6, 500, 500);
    CHECK(s.period_ms > 950 && s.period_ms < 1050);
    CHECK(s.period_dirty);
    /* once learned, the channel rides through the off phase in blink mode */
    now = runMs(&s, now, 500, true, false, false, false);
    now = runMs(&s, now, 450, false, false, false, false);
    CHECK(s.left.blink_mode);
    (void)now;
}

void testSmallJitterNoRepersist()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 15);
    uint32_t now = 1000;
    /* 720 ms measured vs 700 stored: within 10%, keep stored value */
    now = pulseLeft(&s, now, 4, 360, 360);
    CHECK(!s.period_dirty);
    CHECK(700 == s.period_ms);
    (void)now;
}

void testHazardBothChannels()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    for (int i = 0; i < 4; i++)
    {
        now = runMs(&s, now, 350, true, true, false, false);
        now = runMs(&s, now, 350, false, false, false, false);
    }
    CHECK(s.left.blink_mode && s.right.blink_mode);
    CHECK(s.learned);
    CHECK(s.period_ms > 660 && s.period_ms < 740);
    now = runMs(&s, now, 1200, false, false, false, false);
    CHECK(!s.left.blink_mode && !s.right.blink_mode);
}

void testBrakeIntroHoldoff()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    /* first press ever: intro plays */
    now = runMs(&s, now, 500, false, false, true, false);
    CHECK(s.brake.debounced && s.brake_intro);
    /* quick pump: released 3 s only -> no intro */
    now = runMs(&s, now, 3000, false, false, false, false);
    now = runMs(&s, now, 500, false, false, true, false);
    CHECK(s.brake.debounced && !s.brake_intro);
    /* long release (>25 s) -> intro again */
    now = runMs(&s, now, 26000, false, false, false, false);
    now = runMs(&s, now, 500, false, false, true, false);
    CHECK(s.brake.debounced && s.brake_intro);
    (void)now;
}

void testBrakeDebounce()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 0, 15);
    uint32_t now = 1000;
    now = runMs(&s, now, 10, false, false, true, false);
    CHECK(s.brake.debounced);
    now = runMs(&s, now, 10, false, false, false, false);
    CHECK(!s.brake.debounced);
}


/* Runs `cycles` blink cycles at a 700 ms period. `right_lead_ms` lets the
 * right lamp's edge land a few milliseconds before the left one, the way two
 * lamps on one relay never quite agree. */
uint32_t blinkTrain(Blink::BlinkSystem* s, uint32_t now, const int cycles,
                    const bool left, const bool right,
                    const uint32_t right_lead_ms = 0)
{
    for (int i = 0; i < cycles; i++)
    {
        if (right_lead_ms > 0 && left && right)
        {
            now = runMs(s, now, right_lead_ms, false, true, false, false);
            now = runMs(s, now, 350 - right_lead_ms, true, true, false, false);
        }
        else
        {
            now = runMs(s, now, 350, left, right, false, false);
        }
        now = runMs(s, now, 350, false, false, false, false);
    }
    return now;
}

uint32_t settle(Blink::BlinkSystem* s, uint32_t now)
{
    return runMs(s, now, 1500, false, false, false, false);
}

void testTurnUseCreditedAsItHappens()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    /* The switch-on is held just long enough to see whether the other side
     * joins, then it is a left turn — before the episode is anywhere near
     * over, so a power cut mid-signal still has it. */
    now = runMs(&s, now, Blink::HAZARD_JOIN_MS + Blink::DEBOUNCE_SAMPLES + 5,
                true, false, false, false);
    CHECK(1 == s.counts.left_uses);
    CHECK(1 == s.counts.left_flashes);

    now = runMs(&s, now, 350 - Blink::HAZARD_JOIN_MS - Blink::DEBOUNCE_SAMPLES - 5,
                true, false, false, false);
    now = runMs(&s, now, 350, false, false, false, false);
    now = blinkTrain(&s, now, 2, true, false);
    now = settle(&s, now);
    CHECK(1 == s.counts.left_uses);
    CHECK(3 == s.counts.left_flashes);
    CHECK(0 == s.counts.right_uses);
    CHECK(0 == s.counts.hazard_uses);

    /* A second, separate episode is a second use. */
    now = blinkTrain(&s, now, 2, true, false);
    now = settle(&s, now);
    CHECK(2 == s.counts.left_uses);
    CHECK(5 == s.counts.left_flashes);
}

void testHazardFromRestCountsOnlyAsHazard()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    now = blinkTrain(&s, now, 4, true, true);
    now = settle(&s, now);

    CHECK(1 == s.counts.hazard_uses);
    CHECK(4 == s.counts.hazard_flashes);
    CHECK(0 == s.counts.left_uses);
    CHECK(0 == s.counts.right_uses);
    CHECK(0 == s.counts.left_flashes);
    CHECK(0 == s.counts.right_flashes);
}

void testHazardIsSymmetric()
{
    /* The right lamp's edge landing first must give the same tally as the
     * left one landing first. */
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    now = blinkTrain(&s, now, 4, true, true, 8);
    now = settle(&s, now);

    CHECK(1 == s.counts.hazard_uses);
    CHECK(4 == s.counts.hazard_flashes);
    CHECK(0 == s.counts.left_uses);
    CHECK(0 == s.counts.right_uses);
    CHECK(0 == s.counts.left_flashes);
    CHECK(0 == s.counts.right_flashes);
}

void testHazardIsCreditedTheMomentItForms()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    /* Nothing has settled yet — the ignition could be cut right here — and
     * the hazard is already on the books. */
    now = runMs(&s, now, 20, true, true, false, false);
    CHECK(1 == s.counts.hazard_uses);
    CHECK(1 == s.counts.hazard_flashes);
}

void testTurnThenHazardKeepsBoth()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    /* Signalling left, then the hazards go on without a pause: the left turn
     * that happened stays a left turn, and what follows is a hazard. */
    now = blinkTrain(&s, now, 2, true, false);
    now = blinkTrain(&s, now, 3, true, true);
    now = settle(&s, now);

    CHECK(1 == s.counts.left_uses);
    CHECK(2 == s.counts.left_flashes);
    CHECK(1 == s.counts.hazard_uses);
    CHECK(3 == s.counts.hazard_flashes);
    CHECK(0 == s.counts.right_uses);
    CHECK(0 == s.counts.right_flashes);

    /* And the mirror image tallies the same way on its own side. */
    Blink::BlinkSystem m;
    Blink::init(&m, 700, 12);
    now = 1000;
    now = blinkTrain(&m, now, 2, false, true);
    now = blinkTrain(&m, now, 3, true, true);
    now = settle(&m, now);
    CHECK(1 == m.counts.right_uses);
    CHECK(2 == m.counts.right_flashes);
    CHECK(1 == m.counts.hazard_uses);
    CHECK(3 == m.counts.hazard_flashes);
    CHECK(0 == m.counts.left_uses);
}

void testHazardOffAndOnAgainIsTwoHazards()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    /* Hazards for 3, then only the left keeps going for 2, then hazards again
     * for 2: seven flashes of the left lamp. Nothing may be counted twice and
     * nothing may vanish. The first left-only flash still falls inside the
     * right channel's exit grace, so the strip — and therefore the count —
     * still call that one a hazard flash. */
    now = blinkTrain(&s, now, 3, true, true);
    now = blinkTrain(&s, now, 2, true, false);
    now = blinkTrain(&s, now, 2, true, true);
    now = settle(&s, now);

    CHECK(2 == s.counts.hazard_uses);
    CHECK(6 == s.counts.hazard_flashes);
    CHECK(1 == s.counts.left_flashes);
    CHECK(7 == s.counts.hazard_flashes + s.counts.left_flashes);
    CHECK(0 == s.counts.left_uses);    /* the left never switched on by itself */
    CHECK(0 == s.counts.right_uses);
}

void testBrakeAndAuxCounted()
{
    Blink::BlinkSystem s;
    Blink::init(&s, 700, 12);
    uint32_t now = 1000;

    for (int i = 0; i < 3; i++)
    {
        now = runMs(&s, now, 200, false, false, true, false);
        now = runMs(&s, now, 200, false, false, false, false);
    }
    CHECK(3 == s.counts.brake);

    for (int i = 0; i < 2; i++)
    {
        now = runMs(&s, now, 200, false, false, false, true);
        now = runMs(&s, now, 200, false, false, false, false);
    }
    CHECK(2 == s.counts.aux);
    /* A press is one event, not one per sample it stays down. */
    now = runMs(&s, now, 5000, false, false, true, false);
    CHECK(4 == s.counts.brake);
}

} // namespace

int main()
{
    testDebounceGlitch();
    testEnterAndPhase();
    testLearnAndPersistFlag();
    testExitAfterGrace();
    testNeverExitWhileOn();
    testStoredPeriodUsed();
    testRelearnFastFlash();
    testRelearnSlowFlash();
    testSmallJitterNoRepersist();
    testHazardBothChannels();
    testBrakeIntroHoldoff();
    testBrakeDebounce();
    testTurnUseCreditedAsItHappens();
    testHazardFromRestCountsOnlyAsHazard();
    testHazardIsSymmetric();
    testHazardIsCreditedTheMomentItForms();
    testTurnThenHazardKeepsBoth();
    testHazardOffAndOnAgainIsTwoHazards();
    testBrakeAndAuxCounted();

    if (0 != g_fail)
    {
        std::printf("blinker tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("blinker tests: all passed\n");
    return 0;
}

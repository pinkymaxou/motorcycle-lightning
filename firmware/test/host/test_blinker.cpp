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
uint32_t runMs(Blink::BlinkSystem *s, uint32_t now, const uint32_t ms,
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
uint32_t pulseLeft(Blink::BlinkSystem *s, uint32_t now, const int n,
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
    testSmallJitterNoRepersist();
    testHazardBothChannels();
    testBrakeIntroHoldoff();
    testBrakeDebounce();

    if (0 != g_fail)
    {
        std::printf("blinker tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("blinker tests: all passed\n");
    return 0;
}

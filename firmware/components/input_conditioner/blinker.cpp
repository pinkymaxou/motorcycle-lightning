#include "blinker.h"

#include <cstring>

namespace Blink
{

namespace
{

/* Debounce: N consecutive samples of the opposite level to flip.
 * Returns +1 on OFF->ON edge, -1 on ON->OFF edge, 0 otherwise. */
int debounce(BlinkChannel *c, const bool raw)
{
    if (raw == c->debounced)
    {
        c->stable_cnt = 0;
        return 0;
    }
    if (++c->stable_cnt >= DEBOUNCE_SAMPLES)
    {
        c->stable_cnt = 0;
        c->debounced = raw;
        return raw ? 1 : -1;
    }
    return 0;
}

void learnPeriod(BlinkSystem *s, const uint32_t interval_ms)
{
    if (interval_ms < PERIOD_MIN_MS || interval_ms > PERIOD_MAX_MS)
    {
        s->consist = 0;
        s->pending_ms = 0;
        return;
    }
    if (0 != s->pending_ms)
    {
        const uint32_t tol = s->pending_ms / 5; /* ±20% */
        const uint32_t diff = interval_ms > s->pending_ms
                                  ? interval_ms - s->pending_ms
                                  : s->pending_ms - interval_ms;
        if (diff <= tol)
        {
            if (s->consist < 255)
            {
                s->consist++;
            }
            s->pending_ms = (s->pending_ms + interval_ms) / 2;
        }
        else
        {
            s->consist = 0;
            s->pending_ms = interval_ms;
            return;
        }
    }
    else
    {
        s->pending_ms = interval_ms;
        s->consist = 0;
        return;
    }

    if (s->consist >= 1) /* 2 consistent intervals measured */
    {
        const uint32_t cand = s->pending_ms;
        const uint32_t diff = cand > s->period_ms ? cand - s->period_ms
                                                  : s->period_ms - cand;
        /* Persist once, then only update on a real change (>10%). */
        if (!s->learned || diff > s->period_ms / 10)
        {
            s->period_ms = cand;
            s->learned = true;
            s->period_dirty = true;
        }
    }
}

void turnTick(BlinkSystem *s, BlinkChannel *c, const bool raw,
              const uint32_t now_ms)
{
    const int edge = debounce(c, raw);

    if (0 != edge)
    {
        c->last_phase_edge_ms = now_ms;
    }

    if (edge > 0)
    {
        if (c->blink_mode)
        {
            if (now_ms > c->last_on_edge_ms)
            {
                learnPeriod(s, now_ms - c->last_on_edge_ms);
            }
        }
        else
        {
            c->blink_start_ms = now_ms;  /* entering blink mode */
        }
        c->blink_mode = true;
        c->last_on_edge_ms = now_ms;
    }

    /* Exit: signal off AND past the expected next ON edge plus grace. */
    if (c->blink_mode && !c->debounced)
    {
        const uint32_t limit = (s->period_ms * s->exit_x10) / 10;
        if (now_ms - c->last_on_edge_ms > limit)
        {
            c->blink_mode = false;
        }
    }
}

} // namespace

void init(BlinkSystem *s, const uint32_t stored_period_ms, const uint8_t exit_x10)
{
    std::memset(s, 0, sizeof(*s));
    if (stored_period_ms >= PERIOD_MIN_MS && stored_period_ms <= PERIOD_MAX_MS)
    {
        s->period_ms = stored_period_ms;
        s->learned = true;
    }
    else
    {
        s->period_ms = PERIOD_DEFAULT_MS;
        s->learned = false;
    }
    s->exit_x10 = (0 != exit_x10) ? exit_x10 : EXIT_X10_DEFAULT;
    s->brake_holdoff_ms = BRAKE_HOLDOFF_MS;
}

void tick(BlinkSystem *s, const bool raw_left, const bool raw_right,
          const bool raw_brake, const bool raw_aux, const uint32_t now_ms)
{
    turnTick(s, &s->left, raw_left, now_ms);
    turnTick(s, &s->right, raw_right, now_ms);

    const int brake_edge = debounce(&s->brake, raw_brake);
    if (0 != brake_edge)
    {
        s->brake.last_phase_edge_ms = now_ms;
    }
    if (brake_edge > 0)
    {
        /* replay the intro only after a long enough release */
        s->brake_intro = !s->brake_seen ||
            (now_ms - s->brake_off_edge_ms >= s->brake_holdoff_ms);
        s->brake_seen = true;
    }
    else if (brake_edge < 0)
    {
        s->brake_off_edge_ms = now_ms;
    }

    if (0 != debounce(&s->aux, raw_aux))
    {
        s->aux.last_phase_edge_ms = now_ms;
    }
}

} // namespace Blink

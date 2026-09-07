#include "blinker.h"

#include <cstring>

namespace Blink
{

namespace
{

/* Debounce: N consecutive samples of the opposite level to flip.
 * Returns +1 on OFF->ON edge, -1 on ON->OFF edge, 0 otherwise. */
int debounce(BlinkChannel* c, const bool raw)
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

void learnPeriod(BlinkSystem* s, const uint32_t interval_ms)
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

/* Returns the debounced edge: +1 on OFF->ON, -1 on ON->OFF, 0 otherwise. */
int turnTick(BlinkSystem* s, BlinkChannel* c, const bool raw,
             const uint32_t now_ms)
{
    const int edge = debounce(c, raw);

    if (0 != edge)
    {
        c->last_phase_edge_ms = now_ms;
    }

    if (edge > 0)
    {
        /* Learn from every ON->ON interval, in or out of blink mode. A
         * flasher slower than period x exit leaves blink mode before its
         * next ON edge, so learning only inside the mode could never
         * measure it — the estimate would stay wrong forever, and the
         * section would fall back to brake between two flashes. The
         * PERIOD_MIN/MAX window in learnPeriod() rejects a real gap. */
        if (0 != c->last_on_edge_ms && now_ms > c->last_on_edge_ms)
        {
            learnPeriod(s, now_ms - c->last_on_edge_ms);
        }
        if (!c->blink_mode)
        {
            c->blink_start_ms = now_ms;  /* entering blink mode */
            c->episode_flashes = 0;
            c->episode_hazard = false;
        }
        c->blink_mode = true;
        c->episode_flashes++;
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

    return edge;
}

} // namespace

void init(BlinkSystem* s, const uint32_t stored_period_ms, const uint8_t exit_x10)
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

void tick(BlinkSystem* s, const bool raw_left, const bool raw_right,
          const bool raw_brake, const bool raw_aux, const uint32_t now_ms)
{
    const bool left_was_blinking = s->left.blink_mode;
    const bool right_was_blinking = s->right.blink_mode;
    turnTick(s, &s->left, raw_left, now_ms);
    turnTick(s, &s->right, raw_right, now_ms);

    /* Hazard is not a signal of its own here, it is both channels blinking at
     * once. While that lasts, both episodes are marked, so neither is ever
     * counted as a turn — including one that began as a plain turn signal
     * before the hazards were switched on. */
    const bool hazard_now = s->left.blink_mode && s->right.blink_mode;
    if (hazard_now)
    {
        s->left.episode_hazard = true;
        s->right.episode_hazard = true;
    }
    else if (s->hazard_active)
    {
        /* The pair has broken up: one hazard use, and its flashes taken from
         * the left channel so a synchronised pair counts once, not twice. */
        s->counts.hazard_uses++;
        s->counts.hazard_flashes += s->left.episode_flashes;
    }
    s->hazard_active = hazard_now;

    /* A turn episode is credited only once it has ended, and only if it was
     * never part of a hazard. */
    if (left_was_blinking && !s->left.blink_mode && !s->left.episode_hazard)
    {
        s->counts.left_uses++;
        s->counts.left_flashes += s->left.episode_flashes;
    }
    if (right_was_blinking && !s->right.blink_mode && !s->right.episode_hazard)
    {
        s->counts.right_uses++;
        s->counts.right_flashes += s->right.episode_flashes;
    }

    const int brake_edge = debounce(&s->brake, raw_brake);
    if (0 != brake_edge)
    {
        s->brake.last_phase_edge_ms = now_ms;
    }
    if (brake_edge > 0)
    {
        s->counts.brake++;
        /* replay the intro only after a long enough release */
        s->brake_intro = !s->brake_seen ||
            (now_ms - s->brake_off_edge_ms >= s->brake_holdoff_ms);
        s->brake_seen = true;
    }
    else if (brake_edge < 0)
    {
        s->brake_off_edge_ms = now_ms;
    }

    const int aux_edge = debounce(&s->aux, raw_aux);
    if (0 != aux_edge)
    {
        s->aux.last_phase_edge_ms = now_ms;
    }
    if (aux_edge > 0)
    {
        s->counts.aux++;
    }
}

} // namespace Blink

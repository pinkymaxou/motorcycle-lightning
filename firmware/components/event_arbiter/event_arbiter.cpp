#include "event_arbiter.h"

namespace EventArbiter
{

namespace
{

/* A turn sub-effect timeline is normalized onto the flasher half-period;
 * clamp so a bogus tiny period cannot explode the time scale. */
constexpr uint32_t MIN_PHASE_MS = 100;

/* Normalize a turn sub-effect's timeline onto the flasher half-period: the
 * effect's full duration plays over exactly one ON (or off) phase, so sweeps
 * stay in sync with the real blinker whatever its rate. */
void turnTimeScale(const Fx::FxEffect *fx, const CondState &in, Fx::FxLayer *l)
{
    uint32_t phase = in.period_ms / 2;
    if (phase < MIN_PHASE_MS)
    {
        phase = MIN_PHASE_MS;
    }
    if (0 == fx->total_ms)
    {
        l->t_num = l->t_den = 1;
    }
    else
    {
        l->t_num = fx->total_ms;
        l->t_den = phase;
    }
}

} // namespace

void zoneRange(const StripSet &set, const ZoneId zone,
               uint16_t *start, uint16_t *len)
{
    switch (zone)
    {
    case ZoneId::Left:
        *start = 0;
        *len = set.left_end;
        break;
    case ZoneId::Center:
        *start = set.left_end;
        *len = set.center_end - set.left_end;
        break;
    case ZoneId::Right:
        *start = set.center_end;
        *len = set.led_count - set.center_end;
        break;
    default:
        *start = 0;
        *len = set.led_count;
        break;
    }
}

int buildLayers(const CondState &in, const StripSet &set,
                Fx::FxLayer out[Fx::MAX_LAYERS])
{
    int n = 0;

    if (nullptr != set.idle)
    {
        Fx::FxLayer *const l = &out[n++];
        l->fx = set.idle;
        l->zone_start = 0;
        l->zone_len = set.led_count;
        l->mirror = false;
        l->t0_ms = 0;
        l->t_num = l->t_den = 1;
    }
    if (nullptr != set.aux && in.aux)
    {
        Fx::FxLayer *const l = &out[n++];
        l->fx = set.aux;
        zoneRange(set, set.aux_zone, &l->zone_start, &l->zone_len);
        l->mirror = false;
        l->t0_ms = in.aux_edge_ms;
        l->t_num = l->t_den = 1;
    }

    /* Zone geometry is fixed (low indices, centre, high indices); which
     * bike side each end belongs to is an installation detail. A zone keeps
     * its own sweep direction: the low-index zone always sweeps from its
     * high end (the centre of the bar) outwards. */
    const ZoneId low_zone = ZoneId::Left;
    const ZoneId high_zone = ZoneId::Right;
    const ZoneId left_zone = set.swap_sides ? high_zone : low_zone;
    const ZoneId right_zone = set.swap_sides ? low_zone : high_zone;
    const bool left_mirror = !set.swap_sides;
    const bool right_mirror = set.swap_sides;

    if (nullptr != set.brake && in.brake)
    {
        /* While a turn signal blinks, the brake layer must not paint inside
         * that turn zone at all — the zone alternates position/turn colors
         * only. Turn zones are the strip's prefix/suffix, so clipping keeps
         * the brake range contiguous. */
        uint16_t start, len;
        zoneRange(set, set.brake_zone, &start, &len);
        uint16_t lo = start;
        uint16_t hi = start + len;
        const bool low_blinks = set.swap_sides ? in.right_blink : in.left_blink;
        const bool high_blinks = set.swap_sides ? in.left_blink : in.right_blink;
        if (low_blinks && lo < set.left_end)
        {
            lo = set.left_end;
        }
        if (high_blinks && hi > set.center_end)
        {
            hi = set.center_end;
        }
        if (hi > lo)
        {
            Fx::FxLayer *const l = &out[n++];
            l->fx = set.brake;
            l->zone_start = lo;
            l->zone_len = hi - lo;
            l->mirror = false;
            /* Quick re-application of the brake (released < holdoff) skips
             * the effect's intro: shift t0 back so the timeline starts at
             * its loop/steady segment. */
            l->t0_ms = in.brake_intro ? in.brake_edge_ms
                                      : in.brake_edge_ms - set.brake->loop_at_ms;
            l->t_num = l->t_den = 1;
        }
    }

    /* Turn layers: phase-gated — the sub-effect follows the real signal and
     * restarts its timeline on every debounced phase edge.
     * Turn effects are authored inner->outer (position 0 = bike center side):
     * the right zone already runs inner->outer unmirrored (its low LED index
     * is the inner edge), the left zone must mirror to sweep outward.
     * Hazard (both blinking): both zones follow ONE master channel — the one
     * that was already blinking when hazard engaged (earlier blink-mode
     * entry; tie goes to the left) — so debounce jitter or a pre-existing
     * blinker can never desync the two zones. */
    const bool hazard = in.left_blink && in.right_blink;
    const bool master_left = !hazard ||
        static_cast<int32_t>(in.left_blink_start_ms - in.right_blink_start_ms) <= 0;
    bool on_l = in.left_on;
    bool on_r = in.right_on;
    uint32_t t0_l = in.left_phase_ms;
    uint32_t t0_r = in.right_phase_ms;
    if (hazard)
    {
        if (master_left)
        {
            on_r = on_l;
            t0_r = t0_l;
        }
        else
        {
            on_l = on_r;
            t0_l = t0_r;
        }
    }
    if (in.left_blink)
    {
        const Fx::FxEffect *const fx = on_l ? set.turn_on : set.turn_off;
        if (nullptr != fx)
        {
            Fx::FxLayer *const l = &out[n++];
            l->fx = fx;
            zoneRange(set, left_zone, &l->zone_start, &l->zone_len);
            l->mirror = left_mirror;
            l->t0_ms = t0_l;
            turnTimeScale(fx, in, l);
        }
    }
    if (in.right_blink)
    {
        const Fx::FxEffect *const fx = on_r ? set.turn_on : set.turn_off;
        if (nullptr != fx)
        {
            Fx::FxLayer *const l = &out[n++];
            l->fx = fx;
            zoneRange(set, right_zone, &l->zone_start, &l->zone_len);
            l->mirror = right_mirror;
            l->t0_ms = t0_r;
            turnTimeScale(fx, in, l);
        }
    }

    return n;
}

} // namespace EventArbiter

#include "event_arbiter.h"

namespace EventArbiter
{

namespace
{

/* Every section can paint at most idle, aux, brake and one turn layer. */
constexpr int LAYERS_PER_SECTION = 4;
static_assert(Fx::MAX_LAYERS >= CFG_MAX_SECTIONS * LAYERS_PER_SECTION,
              "Fx::MAX_LAYERS must cover every section's layer budget");

/* A turn sub-effect timeline is normalized onto the flasher half-period;
 * clamp so a bogus tiny period cannot explode the time scale. */
constexpr uint32_t MIN_PHASE_MS = 100;

/* Normalize a turn sub-effect's timeline onto the flasher half-period: the
 * effect's full duration plays over exactly one ON (or off) phase, so sweeps
 * stay in sync with the real blinker whatever its rate. */
void turnTimeScale(const Fx::FxEffect* fx, const CondState& in, Fx::FxLayer* l)
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

/* Add one layer covering a whole section; the section's declared direction
 * applies to everything painted there. */
Fx::FxLayer* pushLayer(const SectionSet& sec, const Fx::FxEffect* fx,
                       const uint32_t t0_ms, Fx::FxLayer* out, int* n)
{
    Fx::FxLayer* const l = &out[(*n)++];
    l->fx = fx;
    l->zone_start = sec.start;
    l->zone_len = sec.len;
    l->mirror = sec.reversed;
    l->t0_ms = t0_ms;
    l->t_num = l->t_den = 1;
    return l;
}

} // namespace

void layoutStrip(const StripConfig& sc, StripSet* out)
{
    *out = StripSet{};
    out->led_model = sc.led_model;
    out->color_order = sc.color_order;
    out->reversed = sc.reversed;

    const int count = (sc.n_sections < CFG_MAX_SECTIONS) ? sc.n_sections
                                                         : CFG_MAX_SECTIONS;
    uint16_t offset = 0;
    for (int i = 0; i < count; i++)
    {
        const SectionConfig& src = sc.sections[i];
        SectionSet& dst = out->sections[i];
        dst.start = offset;
        /* Truncate rather than overrun: the render buffers are sized for
         * CFG_MAX_LEDS and a corrupt config must not reach past them. */
        dst.len = (offset + src.led_count > CFG_MAX_LEDS)
                      ? static_cast<uint16_t>(CFG_MAX_LEDS - offset)
                      : src.led_count;
        dst.turn = src.turn;
        dst.reversed = src.reversed;
        offset += dst.len;
    }
    out->n_sections = static_cast<uint8_t>(count);
    out->led_count = offset;
}

bool sectionBlinking(const SectionSet& sec, const CondState& in)
{
    return (TurnSource::Left == sec.turn && in.left_blink) ||
           (TurnSource::Right == sec.turn && in.right_blink);
}

int buildLayers(const CondState& in, const StripSet& set,
                Fx::FxLayer out[Fx::MAX_LAYERS])
{
    /* Hazard (both signals blinking): every section follows ONE master
     * channel — the one that was already blinking when hazard engaged
     * (earlier blink-mode entry; tie goes to the left) — so debounce jitter
     * or a pre-existing blinker can never desync the sections. Resolved once
     * for the whole strip, before any section is painted. */
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

    int n = 0;
    for (int i = 0; i < set.n_sections; i++)
    {
        const SectionSet& sec = set.sections[i];
        if (0 == sec.len)
        {
            continue;           /* placeholder section */
        }
        const bool blinking = sectionBlinking(sec, in);

        if (nullptr != sec.idle)
        {
            pushLayer(sec, sec.idle, 0, out, &n);
        }
        if (nullptr != sec.aux && in.aux)
        {
            pushLayer(sec, sec.aux, in.aux_edge_ms, out, &n);
        }
        /* While this section's own turn signal blinks it alternates its
         * position/turn colors only — the brake must not paint over it. */
        if (nullptr != sec.brake && in.brake && !blinking)
        {
            /* Quick re-application of the brake (released < holdoff) skips
             * the effect's intro: shift t0 back so the timeline starts at
             * its loop/steady segment. */
            const uint32_t t0 = in.brake_intro
                                    ? in.brake_edge_ms
                                    : in.brake_edge_ms - sec.brake->loop_at_ms;
            pushLayer(sec, sec.brake, t0, out, &n);
        }
        if (blinking)
        {
            const bool left = (TurnSource::Left == sec.turn);
            const Fx::FxEffect* const fx =
                (left ? on_l : on_r) ? sec.turn_on : sec.turn_off;
            if (nullptr != fx)
            {
                Fx::FxLayer* const l =
                    pushLayer(sec, fx, left ? t0_l : t0_r, out, &n);
                turnTimeScale(fx, in, l);
            }
        }
    }

    return n;
}

} // namespace EventArbiter

/* Host unit tests for the event arbiter layer builder. */
#include <cstdio>
#include <cstring>

#include "event_arbiter.h"
#include "effect_eval.h"
#include "factory_effects.h"

namespace
{

using EventArbiter::StripSet;
using Fx::FxEffect;
using Fx::FxLayer;

int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) \
    { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

FxEffect g_on, g_off, g_idle, g_brake;

void makeFill(FxEffect *fx, const char *id)
{
    std::memset(fx, 0, sizeof(*fx));
    std::strncpy(fx->id, id, Fx::ID_LEN - 1);
    fx->n_steps = 1;
    fx->loop_from = 0;
    fx->steps[0].prim = Fx::Prim::Fill;
    fx->steps[0].ease = Fx::Ease::Hold;
    fx->steps[0].dur_ms = 1000;
    fx->steps[0].c1a = fx->steps[0].c1b = Fx::RgbaColor{ 255, 0, 0, 255 };
    fx->steps[0].ps[1] = fx->steps[0].pe[1] = 1.0f;
    Fx::finalize(fx);
}

StripSet makeSet()
{
    StripSet set = {};
    makeFill(&g_on, "on");
    makeFill(&g_off, "off");
    makeFill(&g_idle, "idle");
    makeFill(&g_brake, "brake");
    set.idle = &g_idle;
    set.brake = &g_brake;
    set.turn_on = &g_on;
    set.turn_off = &g_off;
    set.led_count = 40;
    set.left_end = 12;
    set.center_end = 28;
    set.brake_zone = ZoneId::Full;
    return set;
}

/* Hazard: both zones must follow the channel that was blinking FIRST. */
void testHazardSyncsOnEarlierChannel()
{
    const StripSet set = makeSet();
    CondState in = {};
    in.period_ms = 750;
    in.left_blink = in.right_blink = true;

    /* left was already blinking, hazard added the right later */
    in.left_blink_start_ms = 500;
    in.right_blink_start_ms = 4000;
    in.left_on = true;
    in.right_on = false;            /* right channel lags by a debounce tick */
    in.left_phase_ms = 1000;
    in.right_phase_ms = 1013;

    FxLayer layers[Fx::MAX_LAYERS];
    int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(3 == n); /* idle + turn L + turn R */
    CHECK(layers[n - 2].fx == &g_on && layers[n - 1].fx == &g_on);
    CHECK(1000 == layers[n - 2].t0_ms && 1000 == layers[n - 1].t0_ms);

    /* RIGHT was already blinking: both zones follow the right channel */
    in.left_blink_start_ms = 4000;
    in.right_blink_start_ms = 500;
    in.left_on = false;
    in.right_on = true;
    in.left_phase_ms = 2013;
    in.right_phase_ms = 2000;
    n = EventArbiter::buildLayers(in, set, layers);
    CHECK(3 == n);
    CHECK(layers[n - 2].fx == &g_on && layers[n - 1].fx == &g_on);
    CHECK(2000 == layers[n - 2].t0_ms && 2000 == layers[n - 1].t0_ms);

    /* simultaneous start: tie goes to the left channel */
    in.left_blink_start_ms = in.right_blink_start_ms = 500;
    in.left_on = true;
    in.right_on = false;
    in.left_phase_ms = 3000;
    in.right_phase_ms = 3006;
    n = EventArbiter::buildLayers(in, set, layers);
    CHECK(3 == n);
    CHECK(3000 == layers[n - 2].t0_ms && 3000 == layers[n - 1].t0_ms);
}

/* A single active turn signal keeps its own phase/timing. */
void testSingleTurnUsesOwnChannel()
{
    const StripSet set = makeSet();
    CondState in = {};
    in.period_ms = 750;
    in.right_blink = true;
    in.right_on = false;
    in.right_phase_ms = 2000;
    in.left_on = true;              /* stale left data must be ignored */
    in.left_phase_ms = 500;

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(2 == n); /* idle + turn R */
    const FxLayer *const tr = &layers[n - 1];
    CHECK(tr->fx == &g_off);
    CHECK(2000 == tr->t0_ms);
}

/* Brake clipped out of actively blinking turn zones. */
void testBrakeClippedByTurns()
{
    const StripSet set = makeSet();
    CondState in = {};
    in.period_ms = 750;
    in.brake = true;
    in.brake_intro = true;
    in.left_blink = true;

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(3 == n); /* idle + brake + turn L */
    const FxLayer *const br = &layers[1];
    CHECK(br->fx == &g_brake);
    CHECK(12 == br->zone_start);            /* left zone excluded */
    CHECK(40 == br->zone_start + br->zone_len);
}

/* Factory effects build natively (no JSON) and resolve palette colors. */
void testFactoryBuild()
{
    Fx::FxPalette pal;
    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        pal.colors[i] = Fx::defaultColor(static_cast<Fx::FxColor>(i));
    }
    FxEffect fx;
    CHECK(Fx::factoryBuild("f_turn_sweep", pal, &fx));
    CHECK(1 == fx.n_steps);
    CHECK(-1 == fx.loop_from);
    CHECK(Fx::Prim::Wipe == fx.steps[0].prim);
    /* c1 resolved to amber, opaque */
    CHECK(0xFF == fx.steps[0].c1a.r && 0x5A == fx.steps[0].c1a.g);
    CHECK(255 == fx.steps[0].c1a.a);
    CHECK(Fx::factoryBuild("f_brake_flash", pal, &fx));
    CHECK(7 == fx.n_steps && 6 == fx.loop_from);
    CHECK(!Fx::factoryBuild("nope", pal, &fx));
    CHECK(nullptr != Fx::fallback(Fx::FallbackRole::Brake));
}

} // namespace

int main()
{
    testHazardSyncsOnEarlierChannel();
    testSingleTurnUsesOwnChannel();
    testBrakeClippedByTurns();
    testFactoryBuild();

    if (0 != g_fail)
    {
        std::printf("arbiter tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("arbiter tests: all passed\n");
    return 0;
}

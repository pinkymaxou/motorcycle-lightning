/* Host unit tests for the event arbiter layer builder. */
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "event_arbiter.h"
#include "effect_eval.h"
#include "factory_effects.h"

namespace
{

using EventArbiter::SectionSet;
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

FxEffect g_on, g_off, g_idle, g_brake, g_aux;

void makeFill(FxEffect* fx, const char* id)
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

/* The factory layout, through the real geometry path. */
StripConfig makeConfig()
{
    StripConfig sc = {};
    sc.n_sections = CFG_DEFAULT_SECTION_COUNT;
    for (int i = 0; i < CFG_DEFAULT_SECTION_COUNT; i++)
    {
        const DefaultSection& def = CFG_DEFAULT_SECTIONS[i];
        SectionConfig& sec = sc.sections[i];
        sec.led_count = def.led_count;
        sec.reversed = def.reversed;
        sec.turn = def.turn;
        std::strcpy(sec.fx_idle, "idle");
        std::strcpy(sec.fx_brake, "brake");
        std::strcpy(sec.fx_turn_on, "on");
        std::strcpy(sec.fx_turn_off, "off");
    }
    return sc;
}

StripSet makeSet()
{
    makeFill(&g_on, "on");
    makeFill(&g_off, "off");
    makeFill(&g_idle, "idle");
    makeFill(&g_brake, "brake");
    makeFill(&g_aux, "aux");

    const StripConfig sc = makeConfig();
    StripSet set;
    EventArbiter::layoutStrip(sc, &set);
    for (int i = 0; i < set.n_sections; i++)
    {
        SectionSet& sec = set.sections[i];
        sec.idle = &g_idle;
        sec.brake = &g_brake;
        sec.brake_floor = true;      /* a real brake effect, not Off */
        if (TurnSource::None != sec.turn)
        {
            sec.turn_on = &g_on;
            sec.turn_off = &g_off;
        }
    }
    return set;
}

/* Find the layer painting a given section with a given effect, or nullptr. */
const FxLayer* layerOf(const FxLayer* layers, const int n,
                       const SectionSet& sec, const FxEffect* fx)
{
    for (int i = 0; i < n; i++)
    {
        if (layers[i].fx == fx && layers[i].zone_start == sec.start)
        {
            return &layers[i];
        }
    }
    return nullptr;
}

/* Sections are laid end to end; the total is their sum. */
void testLayoutOffsets()
{
    const StripSet set = makeSet();
    CHECK(3 == set.n_sections);
    CHECK(40 == set.led_count);
    CHECK(0 == set.sections[0].start && 12 == set.sections[0].len);
    CHECK(12 == set.sections[1].start && 16 == set.sections[1].len);
    CHECK(28 == set.sections[2].start && 12 == set.sections[2].len);

    /* a placeholder keeps its slot and contributes nothing */
    StripConfig sc = makeConfig();
    sc.sections[1].led_count = 0;
    StripSet holed;
    EventArbiter::layoutStrip(sc, &holed);
    CHECK(3 == holed.n_sections);
    CHECK(24 == holed.led_count);
    CHECK(12 == holed.sections[1].start && 0 == holed.sections[1].len);
    CHECK(12 == holed.sections[2].start);

    /* a config summing past the hardware limit truncates instead of overrunning */
    StripConfig big = {};
    big.n_sections = 2;
    big.sections[0].led_count = CFG_MAX_LEDS - 5;
    big.sections[1].led_count = 100;
    StripSet clamped;
    EventArbiter::layoutStrip(big, &clamped);
    CHECK(CFG_MAX_LEDS == clamped.led_count);
    CHECK(5 == clamped.sections[1].len);
}

/* With no input, every section paints its own idle, in its own direction. */
void testIdlePerSection()
{
    const StripSet set = makeSet();
    const CondState in = {};

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(3 == n);
    for (int i = 0; i < 3; i++)
    {
        CHECK(layers[i].fx == &g_idle);
        CHECK(layers[i].zone_start == set.sections[i].start);
        CHECK(layers[i].zone_len == set.sections[i].len);
        CHECK(layers[i].mirror == set.sections[i].reversed);
    }
}

/* Hazard: both sections follow the channel that was blinking FIRST. */
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
    const FxLayer* tl = layerOf(layers, n, set.sections[0], &g_on);
    const FxLayer* tr = layerOf(layers, n, set.sections[2], &g_on);
    CHECK(nullptr != tl && nullptr != tr);
    CHECK(1000 == tl->t0_ms && 1000 == tr->t0_ms);

    /* RIGHT was already blinking: both follow the right channel */
    in.left_blink_start_ms = 4000;
    in.right_blink_start_ms = 500;
    in.left_on = false;
    in.right_on = true;
    in.left_phase_ms = 2013;
    in.right_phase_ms = 2000;
    n = EventArbiter::buildLayers(in, set, layers);
    tl = layerOf(layers, n, set.sections[0], &g_on);
    tr = layerOf(layers, n, set.sections[2], &g_on);
    CHECK(nullptr != tl && nullptr != tr);
    CHECK(2000 == tl->t0_ms && 2000 == tr->t0_ms);

    /* simultaneous start: tie goes to the left channel */
    in.left_blink_start_ms = in.right_blink_start_ms = 500;
    in.left_on = true;
    in.right_on = false;
    in.left_phase_ms = 3000;
    in.right_phase_ms = 3006;
    n = EventArbiter::buildLayers(in, set, layers);
    tl = layerOf(layers, n, set.sections[0], &g_on);
    tr = layerOf(layers, n, set.sections[2], &g_on);
    CHECK(nullptr != tl && nullptr != tr);
    CHECK(3000 == tl->t0_ms && 3000 == tr->t0_ms);
}

/* A single active turn signal only lights the sections that follow it. */
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
    const FxLayer* const tr = layerOf(layers, n, set.sections[2], &g_off);
    CHECK(nullptr != tr);
    CHECK(2000 == tr->t0_ms);
    CHECK(nullptr == layerOf(layers, n, set.sections[0], &g_on));
    CHECK(nullptr == layerOf(layers, n, set.sections[0], &g_off));
}

/* The brake never paints a section whose own turn signal is blinking. */
void testBrakeSkippedInBlinkingSection()
{
    const StripSet set = makeSet();
    CondState in = {};
    in.period_ms = 750;
    in.brake = true;
    in.brake_intro = true;
    in.left_blink = true;

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(nullptr == layerOf(layers, n, set.sections[0], &g_brake));
    const FxLayer* const centre = layerOf(layers, n, set.sections[1], &g_brake);
    const FxLayer* const right = layerOf(layers, n, set.sections[2], &g_brake);
    CHECK(nullptr != centre && nullptr != right);
    CHECK(12 == centre->zone_start && 16 == centre->zone_len);
}

/* A reversed section reverses everything it paints, not just the sweep. */
/* The red floor is a safety net for a section that asked for a brake effect —
 * not a veto over one that asked for darkness. */
void testBrakeFloorHonoursOff()
{
    StripSet set = makeSet();
    CondState in = {};
    in.period_ms = 750;
    in.brake = true;

    CHECK(EventArbiter::brakeFloorActive(set.sections[1], in));

    set.sections[1].brake_floor = false;      /* section's brake effect is Off */
    CHECK(!EventArbiter::brakeFloorActive(set.sections[1], in));

    /* and a blinking section never gets the floor, whatever it asked for */
    set.sections[0].brake_floor = true;
    in.left_blink = true;
    CHECK(!EventArbiter::brakeFloorActive(set.sections[0], in));

    /* no brake input, no floor */
    in.brake = false;
    in.left_blink = false;
    CHECK(!EventArbiter::brakeFloorActive(set.sections[2], in));
}

void testSectionDirection()
{
    StripSet set = makeSet();
    set.sections[1].reversed = true;
    CondState in = {};
    in.period_ms = 750;
    in.brake = true;
    in.brake_intro = true;
    in.aux = true;
    set.sections[1].aux = &g_aux;

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    for (const FxEffect* fx : { &g_idle, &g_aux, &g_brake })
    {
        const FxLayer* const l = layerOf(layers, n, set.sections[1], fx);
        CHECK(nullptr != l);
        CHECK(nullptr == l || l->mirror);
    }
}

/* Turn timelines are normalized onto the flasher half-period, with a clamp
 * so an absurd period cannot explode the time scale. */
void testTurnTimeScale()
{
    const StripSet set = makeSet();
    CondState in = {};
    in.left_blink = true;
    in.left_on = true;
    in.period_ms = 800;

    FxLayer layers[Fx::MAX_LAYERS];
    int n = EventArbiter::buildLayers(in, set, layers);
    const FxLayer* l = layerOf(layers, n, set.sections[0], &g_on);
    CHECK(nullptr != l);
    CHECK(g_on.total_ms == l->t_num && 400 == l->t_den);

    in.period_ms = 10;                      /* below the clamp */
    n = EventArbiter::buildLayers(in, set, layers);
    l = layerOf(layers, n, set.sections[0], &g_on);
    CHECK(nullptr != l);
    CHECK(100 == l->t_den);
}

/* The layer array must hold a fully loaded strip: 8 sections, every event
 * firing at once. */
void testLayerBudget()
{
    StripConfig sc = {};
    sc.n_sections = CFG_MAX_SECTIONS;
    for (int i = 0; i < CFG_MAX_SECTIONS; i++)
    {
        sc.sections[i].led_count = 10;
        sc.sections[i].turn = (0 == i % 2) ? TurnSource::Left : TurnSource::Right;
    }
    StripSet set;
    EventArbiter::layoutStrip(sc, &set);
    for (int i = 0; i < set.n_sections; i++)
    {
        SectionSet& sec = set.sections[i];
        sec.idle = &g_idle;
        sec.aux = &g_aux;
        sec.brake = &g_brake;
        sec.turn_on = &g_on;
        sec.turn_off = &g_off;
    }

    CondState in = {};
    in.period_ms = 750;
    in.aux = true;
    in.brake = true;
    in.brake_intro = true;
    in.left_blink = in.right_blink = true;   /* hazard: no brake anywhere */
    in.left_on = true;

    FxLayer layers[Fx::MAX_LAYERS];
    const int n = EventArbiter::buildLayers(in, set, layers);
    CHECK(n <= Fx::MAX_LAYERS);
    CHECK(CFG_MAX_SECTIONS * 3 == n);        /* idle + aux + turn per section */

    /* without the blinkers, brake takes the turn layer's place */
    in.left_blink = in.right_blink = false;
    const int n2 = EventArbiter::buildLayers(in, set, layers);
    CHECK(n2 <= Fx::MAX_LAYERS);
    CHECK(CFG_MAX_SECTIONS * 3 == n2);
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
    testLayoutOffsets();
    testIdlePerSection();
    testHazardSyncsOnEarlierChannel();
    testSingleTurnUsesOwnChannel();
    testBrakeSkippedInBlinkingSection();
    testBrakeFloorHonoursOff();
    testSectionDirection();
    testTurnTimeScale();
    testLayerBudget();
    testFactoryBuild();

    if (0 != g_fail)
    {
        std::printf("arbiter tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("arbiter tests: all passed\n");
    return 0;
}

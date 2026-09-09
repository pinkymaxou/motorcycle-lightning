/* Host unit tests for the pure effect evaluator. */
#include <cstdio>
#include <cstring>

#include "effect_eval.h"
#include "factory_effects.h"

namespace
{

using Fx::RgbaColor;
using Fx::FxEffect;
using Fx::FxStep;

int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) \
    { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

FxStep stepFill(const uint16_t dur, const Fx::Ease ease, const RgbaColor ca,
                const RgbaColor cb)
{
    FxStep s = {};
    s.prim = Fx::Prim::Fill;
    s.ease = ease;
    s.dur_ms = dur;
    s.c1a = ca;
    s.c1b = cb;
    s.ps[0] = 0.0f;
    s.pe[0] = 0.0f;   /* range start */
    s.ps[1] = 1.0f;
    s.pe[1] = 1.0f;   /* range end */
    return s;
}

void testFillFull()
{
    FxEffect fx = {};
    fx.n_steps = 1;
    fx.loop_from = 0;
    fx.steps[0] = stepFill(1000, Fx::Ease::Hold,
                           RgbaColor{ 255, 90, 0, 255 }, RgbaColor{ 0, 0, 0, 0 });
    CHECK(Fx::finalize(&fx));

    RgbaColor out[10];
    Fx::evaluate(&fx, 500, 10, false, out);
    for (int i = 0; i < 10; i++)
    {
        CHECK(255 == out[i].r && 90 == out[i].g && 0 == out[i].b && 255 == out[i].a);
    }
    /* hold easing: end color never appears */
    Fx::evaluate(&fx, 999, 10, false, out);
    CHECK(255 == out[0].r && 255 == out[0].a);
}

void testFillRangeAndMirror()
{
    FxEffect fx = {};
    fx.n_steps = 1;
    fx.loop_from = 0;
    fx.steps[0] = stepFill(1000, Fx::Ease::Hold,
                           RgbaColor{ 10, 20, 30, 255 }, RgbaColor{ 10, 20, 30, 255 });
    fx.steps[0].ps[1] = fx.steps[0].pe[1] = 0.5f;  /* range [0, 0.5] */
    CHECK(Fx::finalize(&fx));

    RgbaColor out[10];
    Fx::evaluate(&fx, 0, 10, false, out);
    for (int i = 0; i < 5; i++)
    {
        CHECK(255 == out[i].a);
    }
    for (int i = 5; i < 10; i++)
    {
        CHECK(0 == out[i].a);
    }

    Fx::evaluate(&fx, 0, 10, true, out);
    for (int i = 0; i < 5; i++)
    {
        CHECK(0 == out[i].a);
    }
    for (int i = 5; i < 10; i++)
    {
        CHECK(255 == out[i].a);
    }
}

void testColorInterpolation()
{
    FxEffect fx = {};
    fx.n_steps = 1;
    fx.loop_from = 0;
    fx.steps[0] = stepFill(1000, Fx::Ease::Linear,
                           RgbaColor{ 0, 0, 0, 0 }, RgbaColor{ 200, 100, 50, 255 });
    CHECK(Fx::finalize(&fx));

    RgbaColor out[4];
    Fx::evaluate(&fx, 500, 4, false, out);   /* u = 0.5 linear */
    CHECK(100 == out[0].r && 50 == out[0].g && 25 == out[0].b);
    CHECK(128 == out[0].a); /* round(127.5) */
}

void testWipeSweep()
{
    FxEffect fx = {};
    fx.n_steps = 1;
    fx.loop_from = 0;
    FxStep* const s = &fx.steps[0];
    *s = FxStep{};
    s->prim = Fx::Prim::Wipe;
    s->mode = static_cast<uint8_t>(Fx::WipeMode::Low);
    s->ease = Fx::Ease::Linear;
    s->dur_ms = 1000;
    s->c1a = s->c1b = RgbaColor{ 255, 90, 0, 255 };   /* covered = amber */
    s->c2a = s->c2b = RgbaColor{ 0, 0, 0, 0 };        /* uncovered = transparent */
    /* Sweep edge to 1 + soft so the trailing soft edge clears the last
     * pixel — the authoring convention for a complete fill (see spec). */
    s->ps[0] = 0.0f;
    s->pe[0] = 1.05f;
    s->ps[1] = s->pe[1] = 0.05f;               /* soft */
    CHECK(Fx::finalize(&fx));

    RgbaColor out[20];
    Fx::evaluate(&fx, 500, 20, false, out);  /* edge ~0.525 */
    CHECK(255 == out[2].a);   /* pos 0.125: well covered */
    CHECK(0 == out[17].a);    /* pos 0.875: uncovered */
    Fx::evaluate(&fx, 999, 20, false, out);
    CHECK(255 == out[19].a);  /* end of sweep: everything covered */
}

void testLoopFrom()
{
    /* 2 steps of 100 ms; loop_from=1: after the first pass, time cycles
     * within step 1 only. */
    FxEffect fx = {};
    fx.n_steps = 2;
    fx.loop_from = 1;
    fx.steps[0] = stepFill(100, Fx::Ease::Hold,
                           RgbaColor{ 1, 1, 1, 255 }, RgbaColor{ 1, 1, 1, 255 });
    fx.steps[1] = stepFill(100, Fx::Ease::Hold,
                           RgbaColor{ 2, 2, 2, 255 }, RgbaColor{ 2, 2, 2, 255 });
    CHECK(Fx::finalize(&fx));
    CHECK(200 == fx.total_ms && 100 == fx.loop_at_ms);

    RgbaColor out[1];
    Fx::evaluate(&fx, 50, 1, false, out);
    CHECK(1 == out[0].r);               /* intro step */
    Fx::evaluate(&fx, 150, 1, false, out);
    CHECK(2 == out[0].r);               /* loop step */
    Fx::evaluate(&fx, 250, 1, false, out);   /* wrapped: 100 + (250-100)%100 */
    CHECK(2 == out[0].r);
    Fx::evaluate(&fx, 1050, 1, false, out);  /* never returns to intro */
    CHECK(2 == out[0].r);
}

void testOneShotHold()
{
    FxEffect fx = {};
    fx.n_steps = 1;
    fx.loop_from = -1;
    fx.steps[0] = stepFill(100, Fx::Ease::Linear,
                           RgbaColor{ 0, 0, 0, 255 }, RgbaColor{ 100, 0, 0, 255 });
    CHECK(Fx::finalize(&fx));

    RgbaColor out[1];
    Fx::evaluate(&fx, 100000, 1, false, out); /* long past the end: hold */
    CHECK(out[0].r >= 99);
}

void testBlendOver()
{
    uint8_t dst[9] = { 10, 10, 10, 10, 10, 10, 10, 10, 10 };
    const RgbaColor src[3] = { RgbaColor{ 200, 0, 0, 0 },
                               RgbaColor{ 200, 0, 0, 255 },
                               RgbaColor{ 200, 0, 0, 128 } };
    Fx::blendOver(dst, src, 3);
    CHECK(10 == dst[0]);                    /* alpha 0: untouched */
    CHECK(200 == dst[3] && 0 == dst[4]);    /* alpha 255: replaced */
    CHECK(dst[6] > 100 && dst[6] < 110);    /* alpha 128: ~mid mix */
    CHECK(5 == dst[7]);                     /* 10*(127)/255 ~ 5 */
}

/* A palette with an unmistakable value per slot, so a test can tell which
 * palette entry an effect actually reached for. */
Fx::FxPalette probePalette()
{
    Fx::FxPalette p = {};
    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        p.colors[i] = Fx::defaultColor(static_cast<Fx::FxColor>(i));
    }
    return p;
}

void testStrobeIsTenHertz()
{
    FxEffect fx = {};
    CHECK(Fx::factoryBuild("f_strobe", probePalette(), &fx));

    /* 10 Hz is the whole point of the effect, so it is pinned here rather
     * than left to whoever next edits the step table. */
    CHECK(100 == fx.total_ms);
    CHECK(2 == fx.n_steps);
    CHECK(0 == fx.loop_from);   /* runs for as long as the event lasts */

    const Fx::RgbaColor white = Fx::defaultColor(Fx::FxColor::White);
    RgbaColor out[8];
    auto lit = [&](const uint32_t t) {
        Fx::evaluate(&fx, t, 8, false, out);
        return out[0];
    };

    /* First half lit, second half dark, and the dark half is opaque so the
     * layers below cannot show through and soften it into a flicker. */
    CHECK(white.r == lit(0).r && white.g == lit(0).g && white.b == lit(0).b);
    CHECK(255 == lit(0).a);
    CHECK(white.r == lit(49).r);
    CHECK(0 == lit(50).r && 0 == lit(50).g && 0 == lit(50).b);
    CHECK(255 == lit(50).a);
    CHECK(0 == lit(99).r);

    /* Every period thereafter lands the same way: the timeline wraps on the
     * clock, so a long hazard cannot drift off the beat. */
    for (uint32_t period = 1; period < 600; period++)
    {
        const uint32_t base = period * 100;
        CHECK(white.r == lit(base).r);
        CHECK(0 == lit(base + 50).r);
    }
}

void testFinalizeRejects()
{
    FxEffect fx = {};
    fx.n_steps = 0;
    CHECK(!Fx::finalize(&fx));
    fx.n_steps = 1;
    fx.loop_from = 1;               /* loop_from >= n_steps */
    fx.steps[0] = stepFill(100, Fx::Ease::Linear,
                           RgbaColor{ 0, 0, 0, 0 }, RgbaColor{ 0, 0, 0, 0 });
    CHECK(!Fx::finalize(&fx));
    fx.loop_from = 0;
    fx.steps[0].dur_ms = 5;         /* below 10 ms floor */
    CHECK(!Fx::finalize(&fx));
}

} // namespace

int main()
{
    testFillFull();
    testFillRangeAndMirror();
    testColorInterpolation();
    testWipeSweep();
    testLoopFrom();
    testOneShotHold();
    testBlendOver();
    testFinalizeRejects();
    testStrobeIsTenHertz();

    if (0 != g_fail)
    {
        std::printf("eval tests: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    std::printf("eval tests: all passed\n");
    return 0;
}

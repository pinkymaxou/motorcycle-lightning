/*
 * Pure effect evaluator. Implements docs/EFFECT_SPEC.md — a semantic change
 * updates the spec, this file, and the golden vectors together.
 */
#include "effect_eval.h"

#include <cmath>
#include <cstring>

namespace Fx
{

namespace
{

constexpr float PI_F = 3.14159265358979323846f;
constexpr RgbaColor TRANSPARENT = { 0, 0, 0, 0 };

float clamp01(const float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

float fract(const float x)
{
    return x - std::floor(x);
}

float easeApply(const Ease ease, const float u)
{
    switch (ease)
    {
    case Ease::Linear:
        return u;
    case Ease::Hold:
        return 0.0f;
    case Ease::InQuad:
        return u * u;
    case Ease::OutQuad:
        return 1.0f - (1.0f - u) * (1.0f - u);
    case Ease::InOutQuad:
        return u < 0.5f ? 2.0f * u * u
                        : 1.0f - 2.0f * (1.0f - u) * (1.0f - u);
    case Ease::OutCubic:
    {
        const float v = 1.0f - u;
        return 1.0f - v * v * v;
    }
    case Ease::InOutSine:
        return 0.5f - 0.5f * std::cos(PI_F * u);
    default:
        return u;
    }
}

RgbaColor colorLerp(const RgbaColor a, const RgbaColor b, const float k)
{
    RgbaColor out;
    out.r = static_cast<uint8_t>(std::lround(a.r + (b.r - a.r) * k));
    out.g = static_cast<uint8_t>(std::lround(a.g + (b.g - a.g) * k));
    out.b = static_cast<uint8_t>(std::lround(a.b + (b.b - a.b) * k));
    out.a = static_cast<uint8_t>(std::lround(a.a + (b.a - a.a) * k));
    return out;
}

/* mix() from the spec: linear per-channel blend of two RGBA colors. */
RgbaColor colorMix(const RgbaColor a, const RgbaColor b, const float k)
{
    return colorLerp(a, b, clamp01(k));
}

/* Per-pixel primitive evaluation. c1/c2/p are already time-interpolated. */
RgbaColor primEval(const Prim prim, const uint8_t mode, const float pos,
                   const RgbaColor c1, const RgbaColor c2, const float *p,
                   const float min_soft)
{
    switch (prim)
    {
    case Prim::Fill:
        return (pos >= p[0] && pos <= p[1]) ? c1 : TRANSPARENT;

    case Prim::Wipe:
    {
        float s;
        switch (static_cast<WipeMode>(mode))
        {
        default:
        case WipeMode::Low:
            s = pos;
            break;
        case WipeMode::High:
            s = 1.0f - pos;
            break;
        case WipeMode::CenterOut:
            s = std::fabs(pos - 0.5f) * 2.0f;
            break;
        case WipeMode::EdgesIn:
            s = 1.0f - std::fabs(pos - 0.5f) * 2.0f;
            break;
        }
        const float soft = p[1] > min_soft ? p[1] : min_soft;
        const float k = clamp01((p[0] - s) / soft);
        return colorMix(c2, c1, k);
    }

    case Prim::Scan:
    {
        const float d = std::fabs(pos - p[0]);
        const float soft = p[2] > min_soft ? p[2] : min_soft;
        const float k = clamp01((p[1] * 0.5f - d) / soft);
        return colorMix(c2, c1, k);
    }

    case Prim::Gradient:
    {
        float s = pos * p[1] - p[0];
        s = (static_cast<GradientMode>(mode) == GradientMode::Repeat)
                ? fract(s) : clamp01(s);
        return colorMix(c1, c2, s);
    }

    case Prim::Chase:
    {
        const float period = p[1] > 1e-4f ? p[1] : 1e-4f;
        const float u = fract(pos / period - p[0]);
        const float duty = p[2];
        const float soft = p[3];
        float k;
        if (soft <= 0.0f)
        {
            k = (u < duty) ? 1.0f : 0.0f;
        }
        else
        {
            const float m = u < (duty - u) ? u : (duty - u);
            k = clamp01(m / soft);
        }
        return colorMix(c2, c1, k);
    }

    default:
        return TRANSPARENT;
    }
}

/* Map wall time to timeline time according to loop semantics. */
uint32_t wrapTime(const FxEffect *fx, const uint32_t t_ms)
{
    if (t_ms < fx->total_ms)
    {
        return t_ms;
    }
    if (fx->loop_from < 0)
    {
        return fx->total_ms - 1;               /* one-shot: hold last frame */
    }
    const uint32_t span = fx->total_ms - fx->loop_at_ms;
    if (0 == span)
    {
        return fx->total_ms - 1;
    }
    return fx->loop_at_ms + (t_ms - fx->loop_at_ms) % span;
}

} // namespace

bool finalize(FxEffect *fx)
{
    if (fx->n_steps < 1 || fx->n_steps > MAX_STEPS)
    {
        return false;
    }
    if (fx->loop_from < -1 || fx->loop_from >= static_cast<int8_t>(fx->n_steps))
    {
        return false;
    }

    uint32_t acc = 0;
    for (int i = 0; i < fx->n_steps; i++)
    {
        if (fx->steps[i].dur_ms < 10 || fx->steps[i].dur_ms > 60000)
        {
            return false;
        }
        acc += fx->steps[i].dur_ms;
        fx->step_end_ms[i] = acc;
    }
    fx->total_ms = acc;
    fx->loop_at_ms = 0;
    if (fx->loop_from > 0)
    {
        fx->loop_at_ms = fx->step_end_ms[fx->loop_from - 1];
    }
    return true;
}

void evaluate(const FxEffect *fx, const uint32_t t_ms, const uint16_t zone_len,
              const bool mirror, RgbaColor *out)
{
    if (0 == zone_len)
    {
        return;
    }
    if (nullptr == fx || 0 == fx->n_steps || 0 == fx->total_ms)
    {
        std::memset(out, 0, static_cast<size_t>(zone_len) * sizeof(RgbaColor));
        return;
    }

    const uint32_t t = wrapTime(fx, t_ms);

    int k = 0;
    while (k < fx->n_steps - 1 && t >= fx->step_end_ms[k])
    {
        k++;
    }
    const FxStep *const st = &fx->steps[k];
    const uint32_t start = (0 == k) ? 0 : fx->step_end_ms[k - 1];

    const float u = static_cast<float>(t - start) / st->dur_ms;
    const float e = easeApply(st->ease, clamp01(u));

    /* Interpolate once per frame, not per pixel. */
    const RgbaColor c1 = colorLerp(st->c1a, st->c1b, e);
    const RgbaColor c2 = colorLerp(st->c2a, st->c2b, e);
    float p[4];
    for (int i = 0; i < 4; i++)
    {
        p[i] = st->ps[i] + (st->pe[i] - st->ps[i]) * e;
    }

    const float min_soft = 1.0f / zone_len;

    for (uint16_t i = 0; i < zone_len; i++)
    {
        float pos = (i + 0.5f) / zone_len;
        if (mirror)
        {
            pos = 1.0f - pos;
        }
        out[i] = primEval(st->prim, st->mode, pos, c1, c2, p, min_soft);
    }
}

void blendOver(uint8_t *dst_rgb, const RgbaColor *src, const uint16_t count)
{
    for (uint16_t i = 0; i < count; i++)
    {
        const uint16_t a = src[i].a;
        if (0 == a)
        {
            continue;
        }
        uint8_t *const d = &dst_rgb[i * 3];
        if (255 == a)
        {
            d[0] = src[i].r;
            d[1] = src[i].g;
            d[2] = src[i].b;
        }
        else
        {
            d[0] = static_cast<uint8_t>((src[i].r * a + d[0] * (255 - a) + 127) / 255);
            d[1] = static_cast<uint8_t>((src[i].g * a + d[1] * (255 - a) + 127) / 255);
            d[2] = static_cast<uint8_t>((src[i].b * a + d[2] * (255 - a) + 127) / 255);
        }
    }
}

} // namespace Fx

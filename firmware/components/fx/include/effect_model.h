/*
 * Effect data model — pure C++, no ESP-IDF dependencies (host-compilable).
 * Normative semantics live in docs/EFFECT_SPEC.md.
 */
#pragma once

#include <cstdint>

namespace Fx
{

constexpr int MAX_STEPS = 16;
constexpr int ID_LEN = 16;      /* includes NUL */
constexpr int NAME_LEN = 32;    /* includes NUL */
constexpr int MAX_JSON = 4096;
constexpr int MAX_USER_FX = 32;

struct RgbaColor
{
    uint8_t r, g, b, a;
};

enum class Prim : uint8_t
{
    Fill = 0,
    Wipe,
    Scan,
    Gradient,
    Chase,
    Count
};

enum class Ease : uint8_t
{
    Linear = 0,
    Hold,
    InQuad,
    OutQuad,
    InOutQuad,
    OutCubic,
    InOutSine,
    Count
};

enum class WipeMode : uint8_t
{
    Low = 0,
    High,
    CenterOut,
    EdgesIn
};

enum class GradientMode : uint8_t
{
    Clamp = 0,
    Repeat
};

struct FxStep
{
    Prim     prim;
    Ease     ease;
    uint8_t  mode;              /* WipeMode / GradientMode, 0 = default */
    uint8_t  pad;
    uint16_t dur_ms;            /* 10..60000 */
    RgbaColor c1a, c1b;         /* color 1 start/end */
    RgbaColor c2a, c2b;         /* color 2 start/end */
    float    ps[4], pe[4];      /* param slots start/end */
};

struct FxEffect
{
    char     id[ID_LEN];
    char     name[NAME_LEN];
    uint8_t  n_steps;
    int8_t   loop_from;         /* -1 = one-shot (hold last frame) */
    uint32_t total_ms;          /* sum of step durations */
    uint32_t loop_at_ms;        /* cumulative start time of steps[loop_from] */
    uint32_t step_end_ms[MAX_STEPS];
    FxStep   steps[MAX_STEPS];
};

/* Fixed semantic color set — a hardcoded enum, not free-form ids. The color
 * VALUES are user-editable; the set itself never grows or shrinks. Effects
 * reference them as "@position" / "@brake" / "@turn" / "@white". The alpha
 * channel acts as a brightness multiplier (premultiplied at compile time). */
enum class FxColor : uint8_t
{
    Position = 0,
    Brake,
    Turn,
    White,
    Count
};

constexpr int COLOR_COUNT = static_cast<int>(FxColor::Count);

struct FxPalette
{
    RgbaColor colors[COLOR_COUNT];
};

} // namespace Fx

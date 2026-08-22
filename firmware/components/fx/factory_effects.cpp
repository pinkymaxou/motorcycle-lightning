#include "factory_effects.h"
#include "effect_eval.h"

#include <cstring>

namespace Fx
{

namespace
{

/* A step color: either a palette reference or a literal RGBA value. */
struct ColorRef
{
    bool      from_palette;
    FxColor   palette_id;
    RgbaColor literal;
};

constexpr ColorRef pal(const FxColor id)
{
    return ColorRef{ true, id, RgbaColor{ 0, 0, 0, 0 } };
}

constexpr ColorRef lit(const uint8_t r, const uint8_t g, const uint8_t b,
                       const uint8_t a = 255)
{
    return ColorRef{ false, FxColor::Position, RgbaColor{ r, g, b, a } };
}

constexpr ColorRef TRANSPARENT_REF = lit(0, 0, 0, 0);

struct StepDef
{
    Prim     prim;
    Ease     ease;
    uint8_t  mode;
    uint16_t dur_ms;
    ColorRef c1a, c1b;
    ColorRef c2a, c2b;
    float    ps[4], pe[4];
};

/* Full-zone fill holding one color. */
constexpr StepDef fill(const uint16_t dur_ms, const ColorRef c)
{
    return StepDef{ Prim::Fill, Ease::Hold, 0, dur_ms, c, c,
                    TRANSPARENT_REF, TRANSPARENT_REF,
                    { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f } };
}

constexpr int MAX_DEF_STEPS = 7;

struct EffectDef
{
    const char* id;
    const char* name;
    int8_t      loop_from;
    uint8_t     n_steps;
    StepDef     steps[MAX_DEF_STEPS];
};

/* Inner->outer amber sweep across the whole ON phase; the uncovered part is
 * the position color so the brake light never bleeds through mid-sweep.
 * Edge runs to 1.1 so the trailing soft edge clears the last pixel. */
constexpr StepDef TURN_SWEEP_STEP = {
    Prim::Wipe, Ease::OutQuad, static_cast<uint8_t>(WipeMode::Low), 280,
    pal(FxColor::Turn), pal(FxColor::Turn),
    pal(FxColor::Position), pal(FxColor::Position),
    /* edge, soft */
    { 0.0f, 0.10f, 0.0f, 0.0f }, { 1.1f, 0.10f, 0.0f, 0.0f },
};

const EffectDef FACTORY[] = {
    { "f_position", "Position light", 0, 1, { fill(1000, pal(FxColor::Position)) } },

    { "f_brake", "Brake solid", 0, 1, { fill(1000, pal(FxColor::Brake)) } },

    /* 3 rapid flashes (dim phase = position color, never fully dark), then
     * loop_from parks on the steady step. */
    { "f_brake_flash", "Brake 3x flash", 6, 7, {
        fill(60, pal(FxColor::Brake)),
        fill(60, pal(FxColor::Position)),
        fill(60, pal(FxColor::Brake)),
        fill(60, pal(FxColor::Position)),
        fill(60, pal(FxColor::Brake)),
        fill(60, pal(FxColor::Position)),
        fill(1000, pal(FxColor::Brake)),
    } },

    { "f_turn_on", "Turn ON (solid)", 0, 1, { fill(1000, pal(FxColor::Turn)) } },

    { "f_turn_off", "Turn off-phase (low red)", 0, 1,
      { fill(1000, pal(FxColor::Position)) } },

    /* One-shot: holds the fully-swept frame until the phase ends. */
    { "f_turn_sweep", "Turn ON sweep", -1, 1, { TURN_SWEEP_STEP } },

    { "f_white", "Full white", 0, 1, { fill(1000, pal(FxColor::White)) } },

    /* Opaque black: the section goes DARK ("none" shows the layers below). */
    { EFFECT_ID_OFF, "Off (dark)", 0, 1, { fill(1000, lit(0, 0, 0)) } },
};

constexpr int N_FACTORY = sizeof(FACTORY) / sizeof(FACTORY[0]);

/* Palette alpha is a BRIGHTNESS multiplier: resolve to an opaque
 * premultiplied color so the same palette color looks identical no matter
 * how many layers paint it. */
RgbaColor resolve(const ColorRef& ref, const FxPalette& palette)
{
    if (!ref.from_palette)
    {
        return ref.literal;
    }
    const RgbaColor c = palette.colors[static_cast<int>(ref.palette_id)];
    RgbaColor out;
    out.r = static_cast<uint8_t>((c.r * c.a + 127) / 255);
    out.g = static_cast<uint8_t>((c.g * c.a + 127) / 255);
    out.b = static_cast<uint8_t>((c.b * c.a + 127) / 255);
    out.a = 255;
    return out;
}

void build(const EffectDef& def, const FxPalette& palette, FxEffect* out)
{
    std::memset(out, 0, sizeof(*out));
    std::strncpy(out->id, def.id, ID_LEN - 1);
    std::strncpy(out->name, def.name, NAME_LEN - 1);
    out->n_steps = def.n_steps;
    out->loop_from = def.loop_from;
    for (int i = 0; i < def.n_steps; i++)
    {
        const StepDef& sd = def.steps[i];
        FxStep* const st = &out->steps[i];
        st->prim = sd.prim;
        st->ease = sd.ease;
        st->mode = sd.mode;
        st->dur_ms = sd.dur_ms;
        st->c1a = resolve(sd.c1a, palette);
        st->c1b = resolve(sd.c1b, palette);
        st->c2a = resolve(sd.c2a, palette);
        st->c2b = resolve(sd.c2b, palette);
        for (int j = 0; j < 4; j++)
        {
            st->ps[j] = sd.ps[j];
            st->pe[j] = sd.pe[j];
        }
    }
    finalize(out);
}

const EffectDef* find(const char* id)
{
    for (int i = 0; i < N_FACTORY; i++)
    {
        if (0 == std::strcmp(FACTORY[i].id, id))
        {
            return &FACTORY[i];
        }
    }
    return nullptr;
}

} // namespace

int factoryCount()
{
    return N_FACTORY;
}

const FactoryEntry* factoryGet(const int idx)
{
    static FactoryEntry m_entry;
    if (idx < 0 || idx >= N_FACTORY)
    {
        return nullptr;
    }
    m_entry.id = FACTORY[idx].id;
    m_entry.name = FACTORY[idx].name;
    return &m_entry;
}

bool factoryExists(const char* id)
{
    return nullptr != find(id);
}

bool factoryBuild(const char* id, const FxPalette& pal, FxEffect* out)
{
    const EffectDef* const def = find(id);
    if (nullptr == def)
    {
        return false;
    }
    build(*def, pal, out);
    return true;
}

RgbaColor defaultColor(const FxColor id)
{
    switch (id)
    {
    case FxColor::Position:
        return RgbaColor{ 0x3C, 0x00, 0x00, 0xFF };
    case FxColor::Brake:
        return RgbaColor{ 0xFF, 0x00, 0x00, 0xFF };
    case FxColor::Turn:
        return RgbaColor{ 0xFF, 0x80, 0x00, 0xFF };  /* amber */
    case FxColor::White:
    default:
        return RgbaColor{ 0xFF, 0xFF, 0xFF, 0xFF };
    }
}

const FxEffect* fallback(const FallbackRole role)
{
    static FxEffect m_fallback[static_cast<int>(FallbackRole::Count)];
    static bool m_ready = false;

    if (!m_ready)
    {
        FxPalette defaults;
        for (int i = 0; i < COLOR_COUNT; i++)
        {
            defaults.colors[i] = defaultColor(static_cast<FxColor>(i));
        }
        factoryBuild("f_position", defaults,
                     &m_fallback[static_cast<int>(FallbackRole::Position)]);
        factoryBuild("f_brake", defaults,
                     &m_fallback[static_cast<int>(FallbackRole::Brake)]);
        factoryBuild("f_turn_on", defaults,
                     &m_fallback[static_cast<int>(FallbackRole::TurnOn)]);
        factoryBuild("f_turn_off", defaults,
                     &m_fallback[static_cast<int>(FallbackRole::TurnOff)]);
        m_ready = true;
    }
    int i = static_cast<int>(role);
    if (i >= static_cast<int>(FallbackRole::Count))
    {
        i = 0;
    }
    return &m_fallback[i];
}

} // namespace Fx

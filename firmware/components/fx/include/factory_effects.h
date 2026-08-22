/* Factory effects: native constexpr step tables — no JSON, no parser, no
 * heap. Palette color references are resolved when an effect is built, so
 * editing a palette color re-skins every factory effect. */
#pragma once

#include "effect_model.h"

namespace Fx
{

struct FactoryEntry
{
    const char* id;
    const char* name;
};

/* Assigning this effect is an explicit "paint black here", and the arbiter
 * honours it: a section whose brake effect is Off gets no red floor. */
constexpr const char* EFFECT_ID_OFF = "f_off";

int factoryCount();
const FactoryEntry* factoryGet(int idx);
bool factoryExists(const char* id);

/* Build the compiled effect for a factory id, resolving palette references
 * (premultiplied brightness). Returns false for an unknown id. */
bool factoryBuild(const char* id, const FxPalette& pal, FxEffect* out);

/* Hard fallbacks with fixed default colors — the safety floor needs no
 * palette, no config, no storage. */
enum class FallbackRole : uint8_t
{
    Position = 0,
    Brake,
    TurnOn,
    TurnOff,
    Count
};

const FxEffect* fallback(FallbackRole role);

/* Default color values for the fixed palette (also used by ConfigStore). */
RgbaColor defaultColor(FxColor id);

} // namespace Fx

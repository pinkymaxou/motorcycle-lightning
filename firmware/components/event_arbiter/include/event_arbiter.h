/* Event arbiter — pure function: conditioned inputs + one strip's compiled
 * sections -> layer stack for the compositor. Host-compilable. */
#pragma once

#include "cond_state.h"
#include "compositor.h"
#include "sys_config.h"

namespace EventArbiter
{

/* One resolved section: geometry already offset into the strip, effects
 * already compiled. A nullptr effect means that event does not paint here. */
struct SectionSet
{
    const Fx::FxEffect *idle;
    const Fx::FxEffect *aux;
    const Fx::FxEffect *brake;
    const Fx::FxEffect *turn_on;
    const Fx::FxEffect *turn_off;

    uint16_t   start;      /* offset of the section's first LED in the strip */
    uint16_t   len;        /* 0 = the section paints nothing */
    TurnSource turn;
    bool       reversed;   /* animation direction (becomes FxLayer::mirror) */
};

/* Everything one strip needs, compiled and resolved. Strips share only the
 * inputs and the palette. Built off the render task, swapped in by pointer. */
struct StripSet
{
    SectionSet sections[CFG_MAX_SECTIONS];
    uint8_t    n_sections;
    uint16_t   led_count;   /* sum of the section lengths; 0 = not installed */

    /* output hardware */
    uint8_t    brightness;
    LedModel   led_model;
    ColorOrder color_order;
    bool       reversed;
};

/* Resolve geometry only — offsets, total, direction, turn source. The caller
 * fills the effect pointers in (they need the palette and the heap).
 * Truncates at CFG_MAX_LEDS so the render buffers can never be overrun. */
void layoutStrip(const StripConfig &sc, StripSet *out);

/* True while this section's own turn signal is in blink mode. The one place
 * that rule lives: the arbiter and the renderer's brake floor both call it. */
bool sectionBlinking(const SectionSet &sec, const CondState &in);

/* Builds the layer stack in paint order: for each section, idle, aux, brake,
 * then its turn layer. Sections never overlap, so grouping per section is a
 * valid global priority order. Returns the number of layers written. */
int buildLayers(const CondState &in, const StripSet &set,
                Fx::FxLayer out[Fx::MAX_LAYERS]);

} // namespace EventArbiter

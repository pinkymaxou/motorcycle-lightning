/* Event arbiter — pure function: conditioned inputs + one strip's compiled
 * effect set -> layer stack for the compositor. Host-compilable. */
#pragma once

#include "cond_state.h"
#include "compositor.h"
#include "sys_config.h"

namespace EventArbiter
{

/* Everything one strip needs, compiled and resolved. Each strip has its own
 * geometry, hardware type and event assignments; they only share the inputs
 * and the palette. Built off the render task and swapped in by pointer. */
struct StripSet
{
    /* compiled effects — nullptr disables the layer */
    const Fx::FxEffect *idle;
    const Fx::FxEffect *aux;
    const Fx::FxEffect *brake;
    const Fx::FxEffect *turn_on;
    const Fx::FxEffect *turn_off;

    /* resolved geometry (led_count 0 = strip disabled) */
    uint16_t led_count;
    uint16_t left_end;      /* left zone  = [0, left_end) */
    uint16_t center_end;    /* center     = [left_end, center_end) */
    ZoneId   brake_zone;
    ZoneId   aux_zone;

    /* output hardware */
    uint8_t    brightness;
    LedModel   led_model;
    ColorOrder color_order;
    bool       reversed;
};

/* Resolve a zone id against one strip's geometry. */
void zoneRange(const StripSet &set, ZoneId zone,
               uint16_t *start, uint16_t *len);

/* Builds the layer stack in paint order (idle, aux, brake, turn L, turn R).
 * Returns the number of layers written to out[Fx::MAX_LAYERS]. */
int buildLayers(const CondState &in, const StripSet &set,
                Fx::FxLayer out[Fx::MAX_LAYERS]);

} // namespace EventArbiter

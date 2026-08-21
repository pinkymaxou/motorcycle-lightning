/* Layer compositor — pure, host-compilable. One blend rule: layers are painted
 * in array order (priority ascending) with source-over alpha into an opaque
 * RGB frame. Zones decide territory; alpha decides visibility. */
#pragma once

#include "effect_model.h"

namespace Fx
{

constexpr int MAX_LAYERS = 6;

struct FxLayer
{
    const FxEffect *fx;             /* nullptr = skip layer */
    uint16_t zone_start;
    uint16_t zone_len;
    bool     mirror;
    uint32_t t0_ms;                 /* effect-local time = now - t0 */
    /* Timeline scaling: t is multiplied by t_num/t_den before evaluation
     * (1/1 = real time). Turn layers use total_ms/half_period so a sweep
     * always spans exactly one flasher phase, whatever the flasher rate. */
    uint32_t t_num;
    uint32_t t_den;
};

/* Composites into rgb[led_count*3], starting from black.
 * NOT thread-safe (uses a static scratch buffer); render-task only. */
void composite(const FxLayer *layers, int n_layers, uint32_t now_ms,
               uint8_t *rgb, uint16_t led_count);

} // namespace Fx

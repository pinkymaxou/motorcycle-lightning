/*
 * Pure effect evaluator — no ESP-IDF dependencies, compiles on host for the
 * test suite. Deterministic: (effect, t, zone_len, mirror) -> RGBA[].
 */
#pragma once

#include "effect_model.h"

namespace Fx
{

/* Evaluate one effect at time t_ms into out[zone_len] (RGBA, straight alpha).
 * Positions are zone-relative: pos = (i + 0.5) / zone_len, mirrored if asked. */
void evaluate(const FxEffect *fx, uint32_t t_ms, uint16_t zone_len,
              bool mirror, RgbaColor *out);

/* Source-over blend of src (RGBA, straight alpha) onto dst (opaque RGB). */
void blendOver(uint8_t *dst_rgb, const RgbaColor *src, uint16_t count);

/* Recompute total_ms / loop_at_ms / step_end_ms from steps + n_steps +
 * loop_from. Returns false if the effect is structurally invalid. */
bool finalize(FxEffect *fx);

} // namespace Fx

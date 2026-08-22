#include "compositor.h"
#include "effect_eval.h"

#include <cstring>

namespace Fx
{

void composite(const FxLayer* layers, const int n_layers, const uint32_t now_ms,
               uint8_t* rgb, const uint16_t led_count)
{
    static RgbaColor m_scratch[512];
    constexpr uint16_t SCRATCH_LEN = sizeof(m_scratch) / sizeof(m_scratch[0]);

    std::memset(rgb, 0, static_cast<size_t>(led_count) * 3);

    for (int li = 0; li < n_layers; li++)
    {
        const FxLayer* const l = &layers[li];
        if (nullptr == l->fx || 0 == l->zone_len)
        {
            continue;
        }
        uint16_t len = l->zone_len;
        if (l->zone_start >= led_count)
        {
            continue;
        }
        if (static_cast<uint32_t>(l->zone_start) + len > led_count)
        {
            len = led_count - l->zone_start;
        }
        if (len > SCRATCH_LEN)
        {
            len = SCRATCH_LEN;
        }

        uint32_t t = now_ms - l->t0_ms;
        if (0 != l->t_num && 0 != l->t_den && l->t_num != l->t_den)
        {
            t = static_cast<uint32_t>((static_cast<uint64_t>(t) * l->t_num) / l->t_den);
        }
        evaluate(l->fx, t, len, l->mirror, m_scratch);
        blendOver(&rgb[static_cast<size_t>(l->zone_start) * 3], m_scratch, len);
    }
}

} // namespace Fx

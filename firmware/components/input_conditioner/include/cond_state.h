/* Conditioned input snapshot — pure types shared with the arbiter. */
#pragma once

#include <cstdint>

enum class CondEvent : uint8_t
{
    Left = 0,
    Right,
    Brake,
    Aux,
    Count
};

constexpr int COND_EVENT_COUNT = static_cast<int>(CondEvent::Count);

struct CondState
{
    bool left_blink;          /* left channel in blink mode */
    bool left_on;             /* left signal currently ON (debounced phase) */
    bool right_blink;
    bool right_on;
    bool brake;
    bool brake_intro;         /* this braking episode replays the effect intro */
    bool aux;
    uint32_t left_phase_ms;   /* time of last left phase edge (effect t0) */
    uint32_t right_phase_ms;
    uint32_t left_blink_start_ms;   /* when each side entered blink mode */
    uint32_t right_blink_start_ms;  /* (hazard syncs on the earlier one)  */
    uint32_t brake_edge_ms;
    uint32_t aux_edge_ms;
    uint32_t period_ms;       /* current flasher period estimate */
    bool     learned;
};

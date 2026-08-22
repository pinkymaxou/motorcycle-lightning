/*
 * Blinker tracker — pure C++ state machine, no ESP-IDF dependencies (host-
 * testable). Fed at 1 kHz with raw "signal ON" booleans per channel.
 *
 * Algorithm (user-specified):
 *  - On a debounced ON-edge of a turn channel: enter blink mode immediately.
 *  - Measure the period between consecutive ON-edges; after 2 consistent
 *    measurements (±20%) the period is learned (one global period — the bike
 *    has one flasher relay). period_dirty asks the caller to persist it.
 *  - Exit blink mode when the signal is off AND no ON-edge has been seen for
 *    the expected period plus a small grace (default 20% extra, exit_x10=12):
 *    wait a bit for the next flash, stop if it never comes. While the signal
 *    reads ON, never exit (covers long duty cycles / stuck-on line).
 */
#pragma once

#include <cstdint>

namespace Blink
{

constexpr uint8_t  DEBOUNCE_SAMPLES = 5;    /* consecutive 1 kHz samples to flip */
constexpr uint32_t PERIOD_DEFAULT_MS = 750;
constexpr uint32_t PERIOD_MIN_MS = 200;     /* sanity window for measured intervals */
constexpr uint32_t PERIOD_MAX_MS = 3000;
/* The brake effect's intro (e.g. 3x strobe) replays only if the brake was
 * released at least this long before the new press. */
constexpr uint32_t BRAKE_HOLDOFF_MS = 25000;
/* Default blink-mode exit factor x10: period + 20% grace for the next flash. */
constexpr uint8_t  EXIT_X10_DEFAULT = 12;

struct BlinkChannel
{
    /* debounce */
    bool     debounced;
    bool     raw_prev;
    uint8_t  stable_cnt;
    /* blink tracking */
    bool     blink_mode;
    uint32_t blink_start_ms;      /* when blink mode was entered (hazard sync) */
    uint32_t last_on_edge_ms;     /* last debounced OFF->ON edge */
    uint32_t last_phase_edge_ms;  /* last debounced toggle (drives effect t0) */
};

struct BlinkSystem
{
    BlinkChannel left, right;
    /* plain debounced channels */
    BlinkChannel brake, aux;
    /* brake intro gating */
    uint32_t brake_holdoff_ms;    /* required release time (default 25 s) */
    uint32_t brake_off_edge_ms;   /* last debounced release */
    bool     brake_seen;          /* a press has happened since boot */
    bool     brake_intro;         /* current episode replays the intro */
    /* global period learning */
    uint32_t period_ms;           /* current estimate used for exit timing */
    bool     learned;
    bool     period_dirty;        /* a newly learned value should be persisted */
    uint32_t pending_ms;          /* last measured interval */
    uint8_t  consist;             /* consecutive consistent measurements */
    uint8_t  exit_x10;            /* exit factor x10 (12 = period + 20%) */
};

/* stored_period_ms: persisted value from NVS, or 0 if never learned. */
void init(BlinkSystem* s, uint32_t stored_period_ms, uint8_t exit_x10);

/* Call at 1 kHz. raw_* are true when the 12V signal is ON (already
 * polarity-corrected: the GPIO reads low-active). */
void tick(BlinkSystem* s, bool raw_left, bool raw_right,
          bool raw_brake, bool raw_aux, uint32_t now_ms);

} // namespace Blink

/* System configuration — pure types (host-compilable). */
#pragma once

#include <cstdint>
#include "effect_model.h"

constexpr uint32_t CFG_VERSION = 6;
constexpr uint16_t CFG_MAX_LEDS = 300;
constexpr int CFG_STA_SSID_LEN = 33;
constexpr int CFG_STA_PASS_LEN = 65;
/* The PCB's two independent WS2812B outputs (see main/board_pins.h). */
enum class StripId : uint8_t
{
    Strip1 = 0,
    Strip2 = 1,
    Count
};

constexpr int STRIP_COUNT = static_cast<int>(StripId::Count);

inline StripId stripAt(const int index)
{
    return static_cast<StripId>(index);
}

inline int stripIndex(const StripId id)
{
    return static_cast<int>(id);
}

enum class ZoneId : uint8_t
{
    Full = 0,
    Left,
    Center,
    Right,
    Last = Right
};

/* Wire order of the color components, as supported by the led_strip driver.
 * The W component exists on RGBW strips; the renderer leaves it off. */
enum class ColorOrder : uint8_t
{
    GRB = 0,   /* standard WS2812B */
    RGB = 1,
    GRBW = 2,
    RGBW = 3,
    Last = RGBW
};

/* LED controller family — each has its own bit timings (led_strip models).
 * WS2816 carries 16 bits per color component. */
enum class LedModel : uint8_t
{
    WS2812 = 0,
    SK6812 = 1,
    WS2811 = 2,
    WS2816 = 3,
    Last = WS2816
};

/* One physical strip: its own geometry, its own hardware type and its own
 * reaction to the bike's signals. The strips share only the color palette
 * and the input-domain settings (flasher tracking, brake holdoff). */
struct StripConfig
{
    uint16_t led_count;         /* 0 = strip disabled */
    uint8_t  brightness;        /* output scale 0..255 */
    LedModel led_model;
    ColorOrder color_order;
    bool     reversed;          /* flip strip direction at output */
    /* Which end of the strip is the bike's left. False: LED 1 is on the
     * left, so the low indices blink with the left signal. True: the strip
     * is installed the other way round and the sides swap. */
    bool     swap_sides;

    /* zones: left = [0, left_end), center = [left_end, center_end),
     * right = [center_end, led_count) */
    uint16_t zone_left_end;
    uint16_t zone_center_end;

    /* event -> effect assignments (factory ids; empty string = disabled) */
    char fx_idle[Fx::ID_LEN];
    char fx_aux[Fx::ID_LEN];
    char fx_brake[Fx::ID_LEN];
    char fx_turn_on[Fx::ID_LEN];   /* turn zone while the signal is ON */
    char fx_turn_off[Fx::ID_LEN];  /* turn zone during the off phase */
    ZoneId brake_zone;
    ZoneId aux_zone;
};

struct SysConfig
{
    uint32_t version;

    StripConfig strips[STRIP_COUNT];

    /* fixed semantic color set (Fx::FxColor) — values editable only */
    Fx::FxPalette palette;

    /* input domain: shared, the bike has one flasher and one brake line */
    uint8_t  blink_exit_x10;    /* blink-mode exit factor x10 (12 = 1.2x) */
    uint16_t brake_holdoff_s;   /* brake intro replays after this release */

    /* home-network STA (SoftAP always on) */
    char sta_ssid[CFG_STA_SSID_LEN];
    char sta_pass[CFG_STA_PASS_LEN];
    bool sta_active;
};

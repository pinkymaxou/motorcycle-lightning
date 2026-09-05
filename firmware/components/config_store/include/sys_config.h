/* System configuration — pure types (host-compilable). */
#pragma once

#include <cstdint>
#include "effect_model.h"

constexpr uint16_t CFG_MAX_LEDS = 300;
constexpr int CFG_MAX_SECTIONS = 8;   /* mirrored by ws_protocol.options */
constexpr int CFG_SSID_LEN = 33;
constexpr int CFG_PASS_LEN = 65;
/* WPA2 refuses anything shorter; the AP would simply fail to start, which
 * would lock the config page away. */
constexpr int CFG_AP_PASS_MIN = 8;
/* The WiFi driver's password field is 64 bytes including its terminator: a
 * 64-character key would be silently cut to 63, and an access point would
 * then start with a password nobody typed. */
constexpr int CFG_PASS_MAX = 63;

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

/* Which turn signal drives a section (None = it never blinks). */
enum class TurnSource : uint8_t
{
    None = 0,
    Left,
    Right,
    Last = Right
};

/* One contiguous run of LEDs inside a strip, in wiring order. The hardware
 * (model, color order, data direction) belongs to the strip; a
 * section only decides what plays there and in which direction. An empty
 * effect id means that event does not paint this section. */
struct SectionConfig
{
    uint16_t   led_count;       /* 0 = placeholder, occupies no LED */
    bool       reversed;        /* animation direction inside the section */
    TurnSource turn;
    char fx_idle[Fx::ID_LEN];
    char fx_aux[Fx::ID_LEN];
    char fx_brake[Fx::ID_LEN];
    char fx_turn_on[Fx::ID_LEN];   /* while the turn signal is ON */
    char fx_turn_off[Fx::ID_LEN];  /* during the off phase */
};

/* One physical output: its hardware, then the sections laid end to end. The
 * strip's LED count is the sum of the section lengths. */
struct StripConfig
{
    LedModel   led_model;
    ColorOrder color_order;
    bool       reversed;        /* flip strip direction at output (wiring) */
    uint8_t    n_sections;      /* 0 = strip not installed */
    SectionConfig sections[CFG_MAX_SECTIONS];
};

/* Sum of the section lengths; 0 = strip not installed. Saturates at
 * CFG_MAX_LEDS so callers can size buffers from it unconditionally. */
inline uint16_t stripTotalLeds(const StripConfig& sc)
{
    uint32_t total = 0;
    const int n = (sc.n_sections < CFG_MAX_SECTIONS) ? sc.n_sections
                                                     : CFG_MAX_SECTIONS;
    for (int i = 0; i < n; i++)
    {
        total += sc.sections[i].led_count;
    }
    return (total > CFG_MAX_LEDS) ? CFG_MAX_LEDS : static_cast<uint16_t>(total);
}

/* The factory layout, shared by the stored defaults and by the compiled-in
 * hard fallback so the two can never drift: one turn run at each end sweeping
 * outwards, a brake bar in the middle. */
struct DefaultSection
{
    uint16_t   led_count;
    bool       reversed;
    TurnSource turn;
};

constexpr DefaultSection CFG_DEFAULT_SECTIONS[] = {
    { 12, true,  TurnSource::Left  },   /* sweeps toward the low index */
    { 16, false, TurnSource::None  },
    { 12, false, TurnSource::Right },
};

constexpr int CFG_DEFAULT_SECTION_COUNT =
    sizeof(CFG_DEFAULT_SECTIONS) / sizeof(CFG_DEFAULT_SECTIONS[0]);

struct SysConfig
{
    StripConfig strips[STRIP_COUNT];

    /* fixed semantic color set (Fx::FxColor) — values editable only */
    Fx::FxPalette palette;

    /* Hazard is a whole-vehicle state, so its look is shared rather than
     * per section: with both signals blinking, these replace the sections'
     * turn effects. Empty = keep whatever the section already plays. A sweep
     * says "I am going that way", which is not what hazard means. */
    char fx_hazard_on[Fx::ID_LEN];
    char fx_hazard_off[Fx::ID_LEN];

    /* input domain: shared, the bike has one flasher and one brake line */
    uint8_t  blink_exit_x10;    /* blink-mode exit factor x10 (12 = 1.2x) */
    uint16_t brake_holdoff_s;   /* brake intro replays after this release */

    /* home-network STA (SoftAP always on) */
    char sta_ssid[CFG_SSID_LEN];
    char sta_pass[CFG_PASS_LEN];
    bool sta_active;

    /* Brake + hazard within the first seconds of a boot brings the config
     * WiFi up, for a module whose button is behind a top box. Stored
     * inverted because proto3 omits a false: a config written before this
     * field existed therefore reads as "not disabled", which is the default
     * this shortcut wants. */
    bool wifi_combo_off;

    /* The module's own access point. An empty SSID means the compiled-in
     * pair, so a module that was never configured still has a way in. */
    char ap_ssid[CFG_SSID_LEN];
    char ap_pass[CFG_PASS_LEN];
};

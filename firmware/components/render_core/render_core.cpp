#include "render_core.h"
#include "tasks.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

#include "input_conditioner.h"
#include "event_arbiter.h"
#include "compositor.h"
#include "factory_effects.h"
#include "led_driver.h"

namespace RenderCore
{

namespace
{

const char* const TAG = "render_core";

/* ~75 FPS (13 ms is the closest the 1 kHz FreeRTOS tick allows). Every frame
 * is pushed to the strip, even an unchanged one: a continuous refresh is what
 * heals a corrupted WS2812 transmission — a bad pixel is corrected within one
 * frame instead of latching forever. Wire time is 30 us per LED and the RMT
 * device is sized to the strip, so a 40-LED bar costs 1.2 ms and the 300-LED
 * maximum 9 ms; two full-length strips would overrun the period, and the
 * loop then simply runs as fast as the wire allows. */
constexpr uint32_t FRAME_PERIOD_MS = 13;
constexpr uint32_t FRAME_BUDGET_STATS_MS = 1000;
constexpr uint8_t BRAKE_RED_FLOOR = 64;
constexpr int CTRL_QUEUE_DEPTH = 8;
constexpr TickType_t CTRL_SEND_TIMEOUT_MS = 200;

/* One owned effect slot per assignable role. */
enum class Slot : uint8_t
{
    Idle = 0,
    Aux,
    Brake,
    TurnOn,
    TurnOff,
    Count
};

constexpr int SLOT_COUNT = static_cast<int>(Slot::Count);

/* Worst case one distinct effect per assignable slot of every section; in
 * practice a whole config uses a handful. */
constexpr int MAX_OWNED = CFG_MAX_SECTIONS * SLOT_COUNT * STRIP_COUNT;

/* Ownership-transferred to the render task via the control queue. Each
 * section carries its own compiled effects, so two sections can react to the
 * very same input differently. Effects are deduplicated by id: the palette is
 * global to a config, so the same id compiles to identical data, and a full
 * 8-section config would otherwise cost ~80 KB of heap (an FxEffect is ~1 KB)
 * — twice that while the old bundle is still alive. */
struct Bundle
{
    EventArbiter::StripSet strips[STRIP_COUNT];
    Fx::FxEffect* owned[MAX_OWNED];
    char owned_id[MAX_OWNED][Fx::ID_LEN];
    int n_owned;
};

struct RenderCmd
{
    Bundle* bundle;
};

static QueueHandle_t m_ctrl_q;
static char m_warnings[256];

static std::atomic<uint32_t> m_fps_x10;
static std::atomic<uint32_t> m_frame_us_max;

/* latest composited frame per strip for the webpage live view (tearing at
 * this poll rate is visually irrelevant, so a plain copy is fine) */
static uint8_t m_frame_copy[STRIP_COUNT][CFG_MAX_LEDS * 3];

/* Render-task only (single writer): 32 layers and the fallback sets are far
 * too big for a 6 KB task stack. */
static Fx::FxLayer m_layers[Fx::MAX_LAYERS];
static EventArbiter::StripSet m_fallback_sets[STRIP_COUNT];
static std::atomic<uint16_t> m_frame_leds[STRIP_COUNT];

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void warnAppend(const char* id)
{
    const size_t used = std::strlen(m_warnings);
    if (used < sizeof(m_warnings) - 48)
    {
        snprintf(m_warnings + used, sizeof(m_warnings) - used,
                 "unknown effect '%s', using fallback; ", id);
    }
}

/* Build one assigned effect. Empty id -> nullptr (layer disabled). Unknown
 * id -> hard fallback for the role + warning. */
const Fx::FxEffect* loadEffect(Bundle* bu, const char* id,
                               const Fx::FxPalette& pal,
                               const Fx::FallbackRole role)
{
    if ('\0' == id[0])
    {
        return nullptr;
    }
    for (int i = 0; i < bu->n_owned; i++)
    {
        if (0 == std::strcmp(bu->owned_id[i], id))
        {
            return bu->owned[i];   /* already compiled for this config */
        }
    }
    if (bu->n_owned >= MAX_OWNED)
    {
        warnAppend(id);
        return Fx::fallback(role);
    }

    Fx::FxEffect* const fx = new (std::nothrow) Fx::FxEffect;
    if (nullptr == fx || !Fx::factoryBuild(id, pal, fx))
    {
        delete fx;
        warnAppend(id);
        /* never cached: fallbacks are static and freeBundle would delete them */
        return Fx::fallback(role);
    }
    bu->owned[bu->n_owned] = fx;
    strlcpy(bu->owned_id[bu->n_owned], id, Fx::ID_LEN);
    bu->n_owned++;
    return fx;
}

/* Hard-fallback set for strip 0, built from the factory layout with static
 * effects: functional lighting from compiled-in data alone, before storage is
 * even mounted (strip 1 stays dark until a config says otherwise). */
void fallbackSet(EventArbiter::StripSet sets[STRIP_COUNT])
{
    std::memset(sets, 0, sizeof(EventArbiter::StripSet) * STRIP_COUNT);
    EventArbiter::StripSet& s0 = sets[0];
    s0.n_sections = CFG_DEFAULT_SECTION_COUNT;

    uint16_t offset = 0;
    for (int i = 0; i < CFG_DEFAULT_SECTION_COUNT; i++)
    {
        const DefaultSection& def = CFG_DEFAULT_SECTIONS[i];
        EventArbiter::SectionSet& sec = s0.sections[i];
        sec.start = offset;
        sec.len = def.led_count;
        sec.turn = def.turn;
        sec.reversed = def.reversed;
        sec.idle = Fx::fallback(Fx::FallbackRole::Position);
        sec.brake = Fx::fallback(Fx::FallbackRole::Brake);
        sec.brake_floor = true;
        if (TurnSource::None != def.turn)
        {
            sec.turn_on = Fx::fallback(Fx::FallbackRole::TurnOn);
            sec.turn_off = Fx::fallback(Fx::FallbackRole::TurnOff);
        }
        offset += sec.len;
    }
    s0.led_count = offset;
}

void freeBundle(Bundle* bu)
{
    if (nullptr == bu)
    {
        return;
    }
    for (int i = 0; i < bu->n_owned; i++)
    {
        delete bu->owned[i];
    }
    delete bu;
}

/* Post-composite safety floor: a section that carries a brake effect keeps a
 * minimum red level while the brake input is active, unless its own turn
 * signal is blinking (that section alternates position/turn colors only). No
 * effect can make braking invisible. */
void brakeFloor(uint8_t* rgb, const EventArbiter::StripSet& set,
                const CondState& in)
{
    for (int k = 0; k < set.n_sections; k++)
    {
        const EventArbiter::SectionSet& sec = set.sections[k];
        if (!EventArbiter::brakeFloorActive(sec, in))
        {
            continue;
        }
        const uint16_t hi = sec.start + sec.len;
        for (uint16_t i = sec.start; i < hi; i++)
        {
            if (rgb[i * 3] < BRAKE_RED_FLOOR)
            {
                rgb[i * 3] = BRAKE_RED_FLOOR;
            }
        }
    }
}

void renderTask(void* arg)
{
    (void)arg;

    esp_task_wdt_add(nullptr);


    Bundle* cur = nullptr;              /* nullptr = using the static fallback */
    fallbackSet(m_fallback_sets);
    const EventArbiter::StripSet* sets = m_fallback_sets;

    static uint8_t m_rgb[CFG_MAX_LEDS * 3];

    uint32_t frames = 0;
    uint32_t fps_mark = nowMs();
    uint32_t us_max = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;)
    {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_PERIOD_MS));
        esp_task_wdt_reset();

        const int64_t t_in = esp_timer_get_time();
        const uint32_t now = static_cast<uint32_t>(t_in / 1000);

        /* Drain control commands (non-blocking). */
        RenderCmd cmd;
        while (pdTRUE == xQueueReceive(m_ctrl_q, &cmd, 0))
        {
            freeBundle(cur);
            cur = cmd.bundle;
            sets = cur->strips;
            for (int i = 0; i < STRIP_COUNT; i++)
            {
                const StripId id = stripAt(i);
                LedDriver::setReversed(id, sets[i].reversed);
                /* recreates the RMT device when type or length changed; we
                 * are on the render task, where RMT channels must be born */
                LedDriver::setLayout(id, sets[i].led_model, sets[i].color_order,
                                     sets[i].led_count);
            }
        }

        CondState in;
        InputConditioner::get(&in);   /* simulated signals already merged upstream */

        for (int i = 0; i < STRIP_COUNT; i++)
        {
            const EventArbiter::StripSet& set = sets[i];
            if (0 == set.led_count)
            {
                continue;           /* strip not installed */
            }

            const int n = EventArbiter::buildLayers(in, set, m_layers);
            Fx::composite(m_layers, n, now, m_rgb, set.led_count);

            if (in.brake)
            {
                brakeFloor(m_rgb, set, in);
            }

            LedDriver::write(stripAt(i), m_rgb, set.led_count);

            std::memcpy(m_frame_copy[i], m_rgb,
                        static_cast<size_t>(set.led_count) * 3);
            m_frame_leds[i].store(set.led_count, std::memory_order_release);
        }

        /* stats */
        const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t_in);
        if (us > us_max)
        {
            us_max = us;
        }
        frames++;
        if (now - fps_mark >= FRAME_BUDGET_STATS_MS)
        {
            m_fps_x10.store(frames * 10000 / (now - fps_mark));
            m_frame_us_max.store(us_max);
            frames = 0;
            us_max = 0;
            fps_mark = now;
        }
    }
}

} // namespace

esp_err_t applyConfig(const SysConfig& cfg)
{
    Bundle* const bu = new (std::nothrow) Bundle();
    if (nullptr == bu)
    {
        return ESP_ERR_NO_MEM;
    }
    std::memset(bu, 0, sizeof(*bu));

    m_warnings[0] = '\0';

    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const StripConfig& sc = cfg.strips[i];
        EventArbiter::StripSet& set = bu->strips[i];
        EventArbiter::layoutStrip(sc, &set);
        if (0 == set.led_count)
        {
            continue;               /* strip not installed: no effects */
        }
        for (int k = 0; k < set.n_sections; k++)
        {
            const SectionConfig& sec_cfg = sc.sections[k];
            EventArbiter::SectionSet& sec = set.sections[k];
            sec.idle = loadEffect(bu, sec_cfg.fx_idle, cfg.palette,
                                  Fx::FallbackRole::Position);
            sec.aux = loadEffect(bu, sec_cfg.fx_aux, cfg.palette,
                                 Fx::FallbackRole::Position);
            sec.brake = loadEffect(bu, sec_cfg.fx_brake, cfg.palette,
                                   Fx::FallbackRole::Brake);
            set.hazard_on = loadEffect(bu, cfg.fx_hazard_on, cfg.palette,
                                       Fx::FallbackRole::TurnOn);
            set.hazard_off = loadEffect(bu, cfg.fx_hazard_off, cfg.palette,
                                        Fx::FallbackRole::TurnOff);
            /* Off is a deliberate "dark here", so it opts out of the floor. */
            sec.brake_floor = nullptr != sec.brake &&
                              0 != std::strcmp(sec_cfg.fx_brake, Fx::EFFECT_ID_OFF);
            if (TurnSource::None == sec.turn)
            {
                continue;           /* a turn effect could never play here */
            }
            sec.turn_on = loadEffect(bu, sec_cfg.fx_turn_on, cfg.palette,
                                     Fx::FallbackRole::TurnOn);
            sec.turn_off = loadEffect(bu, sec_cfg.fx_turn_off, cfg.palette,
                                      Fx::FallbackRole::TurnOff);
        }
    }

    const RenderCmd cmd = { bu };
    if (pdTRUE != xQueueSend(m_ctrl_q, &cmd, pdMS_TO_TICKS(CTRL_SEND_TIMEOUT_MS)))
    {
        freeBundle(bu);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void forceEvent(const CondEvent ev, const bool active)
{
    InputConditioner::forceEvent(ev, active);
}

bool eventForced(const CondEvent ev)
{
    return InputConditioner::eventForced(ev);
}

void setOverride(const bool active)
{
    InputConditioner::setOverride(active);
}

bool overrideActive()
{
    return InputConditioner::overrideActive();
}

void getStats(uint32_t* fps_x10, uint32_t* frame_us_max)
{
    if (nullptr != fps_x10)
    {
        *fps_x10 = m_fps_x10.load();
    }
    if (nullptr != frame_us_max)
    {
        *frame_us_max = m_frame_us_max.load();
    }
}

const char* warnings()
{
    return m_warnings; /* read-mostly; short bounded string */
}

uint16_t getFrame(const StripId strip, uint8_t* out_rgb, const uint16_t max_leds)
{
    const int i = stripIndex(strip);
    if (i < 0 || i >= STRIP_COUNT)
    {
        return 0;
    }
    uint16_t n = m_frame_leds[i].load(std::memory_order_acquire);
    if (n > max_leds)
    {
        n = max_leds;
    }
    std::memcpy(out_rgb, m_frame_copy[i], static_cast<size_t>(n) * 3);
    return n;
}

esp_err_t start()
{
    m_ctrl_q = xQueueCreate(CTRL_QUEUE_DEPTH, sizeof(RenderCmd));
    if (nullptr == m_ctrl_q)
    {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = Tasks::create(Tasks::RENDER, renderTask, nullptr);
    return (pdPASS == ok) ? ESP_OK : ESP_FAIL;
}

} // namespace RenderCore

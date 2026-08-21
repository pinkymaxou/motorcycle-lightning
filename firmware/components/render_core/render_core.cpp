#include "render_core.h"

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

const char *const TAG = "render_core";

/* ~75 FPS (13 ms is the closest the 1 kHz FreeRTOS tick allows). Every frame
 * is pushed to the strip, even an unchanged one: a continuous refresh is what
 * heals a corrupted WS2812 transmission — a bad pixel is corrected within one
 * frame instead of latching forever. The budget stays comfortable because the
 * wire time is 30 us per LED (3.6 ms at 120 LEDs, ~28% of the period). */
constexpr uint32_t FRAME_PERIOD_MS = 13;
constexpr uint32_t FRAME_BUDGET_STATS_MS = 1000;
constexpr uint8_t BRAKE_RED_FLOOR = 64;
constexpr int CTRL_QUEUE_DEPTH = 8;
constexpr uint32_t RENDER_TASK_STACK_BYTES = 6144;
constexpr UBaseType_t RENDER_TASK_PRIORITY = 10;
constexpr BaseType_t RENDER_CORE_ID = 1;    /* WiFi/httpd live on core 0 */
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

/* Ownership-transferred to the render task via the control queue. Each
 * strip carries its own compiled effects, so a strip can react to the very
 * same input differently from its neighbour. */
struct Bundle
{
    EventArbiter::StripSet strips[STRIP_COUNT];
    Fx::FxEffect *owned[SLOT_COUNT * STRIP_COUNT];
    int n_owned;
};

struct RenderCmd
{
    Bundle *bundle;
};

static QueueHandle_t m_ctrl_q;
static char m_warnings[256];
static int m_strip_gpios[STRIP_COUNT];

static std::atomic<uint32_t> m_fps_x10;
static std::atomic<uint32_t> m_frame_us_max;

/* latest composited frame per strip for the webpage live view (tearing at
 * this poll rate is visually irrelevant, so a plain copy is fine) */
static uint8_t m_frame_copy[STRIP_COUNT][CFG_MAX_LEDS * 3];
static std::atomic<uint16_t> m_frame_leds[STRIP_COUNT];

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void warnAppend(const char *id)
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
const Fx::FxEffect *loadEffect(Bundle *bu, const char *id,
                               const Fx::FxPalette &pal,
                               const Fx::FallbackRole role)
{
    if ('\0' == id[0])
    {
        return nullptr;
    }
    Fx::FxEffect *const fx = new (std::nothrow) Fx::FxEffect;
    if (nullptr == fx || !Fx::factoryBuild(id, pal, fx))
    {
        delete fx;
        warnAppend(id);
        return Fx::fallback(role);
    }
    bu->owned[bu->n_owned++] = fx;
    return fx;
}

/* Hard-fallback set for strip 0: functional lighting from compiled-in data
 * alone (strip 1 stays dark until a config says otherwise). */
void fallbackSet(EventArbiter::StripSet sets[STRIP_COUNT])
{
    std::memset(sets, 0, sizeof(EventArbiter::StripSet) * STRIP_COUNT);
    EventArbiter::StripSet &s0 = sets[0];
    s0.idle = Fx::fallback(Fx::FallbackRole::Position);
    s0.brake = Fx::fallback(Fx::FallbackRole::Brake);
    s0.turn_on = Fx::fallback(Fx::FallbackRole::TurnOn);
    s0.turn_off = Fx::fallback(Fx::FallbackRole::TurnOff);
    s0.led_count = 40;
    s0.left_end = 12;
    s0.center_end = 28;
    s0.brake_zone = ZoneId::Full;
    s0.brightness = 160;
}

void freeBundle(Bundle *bu)
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

/* Post-composite safety floor: while braking, pixels of the brake zone that
 * are not claimed by an active turn signal keep a minimum red level. No
 * effect can make braking invisible. */
void brakeFloor(uint8_t *rgb, const EventArbiter::StripSet &set,
                const CondState &in)
{
    uint16_t start, len;
    EventArbiter::zoneRange(set, set.brake_zone, &start, &len);
    const uint16_t lo = start;
    const uint16_t hi = start + len;

    for (uint16_t i = lo; i < hi; i++)
    {
        if (in.left_blink && i < set.left_end)
        {
            continue;
        }
        if (in.right_blink && i >= set.center_end)
        {
            continue;
        }
        if (rgb[i * 3] < BRAKE_RED_FLOOR)
        {
            rgb[i * 3] = BRAKE_RED_FLOOR;
        }
    }
}

void renderTask(void *arg)
{
    (void)arg;

    /* Init the strips from this task so the RMT interrupts land on core 1. */
    if (ESP_OK != LedDriver::init(m_strip_gpios, STRIP_COUNT, CFG_MAX_LEDS))
    {
        ESP_LOGE(TAG, "strip init failed — lighting dead, check wiring");
    }

    esp_task_wdt_add(nullptr);

    Bundle *cur = nullptr;              /* nullptr = using the static fallback */
    EventArbiter::StripSet fb[STRIP_COUNT];
    fallbackSet(fb);
    const EventArbiter::StripSet *sets = fb;

    static uint8_t m_rgb[CFG_MAX_LEDS * 3];
    Fx::FxLayer layers[Fx::MAX_LAYERS];

    for (int i = 0; i < STRIP_COUNT; i++)
    {
        LedDriver::setBrightness(stripAt(i), sets[i].brightness);
    }

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
                LedDriver::setBrightness(id, sets[i].brightness);
                LedDriver::setReversed(id, sets[i].reversed);
                /* recreates the RMT device when the strip type changed; we
                 * are on the render task, where RMT channels must be born */
                LedDriver::setLedType(id, sets[i].led_model, sets[i].color_order);
            }
        }

        CondState in;
        InputConditioner::get(&in);   /* simulated signals already merged upstream */

        for (int i = 0; i < STRIP_COUNT; i++)
        {
            const EventArbiter::StripSet &set = sets[i];
            if (0 == set.led_count)
            {
                continue;           /* strip not installed */
            }

            const int n = EventArbiter::buildLayers(in, set, layers);
            Fx::composite(layers, n, now, m_rgb, set.led_count);

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

esp_err_t applyConfig(const SysConfig &cfg)
{
    Bundle *const bu = new (std::nothrow) Bundle();
    if (nullptr == bu)
    {
        return ESP_ERR_NO_MEM;
    }
    std::memset(bu, 0, sizeof(*bu));

    m_warnings[0] = '\0';

    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const StripConfig &sc = cfg.strips[i];
        EventArbiter::StripSet &set = bu->strips[i];

        set.led_count = sc.led_count;
        set.left_end = sc.zone_left_end;
        set.center_end = sc.zone_center_end;
        set.brake_zone = sc.brake_zone;
        set.aux_zone = sc.aux_zone;
        set.brightness = sc.brightness;
        set.led_model = sc.led_model;
        set.color_order = sc.color_order;
        set.reversed = sc.reversed;

        if (0 == sc.led_count)
        {
            continue;               /* strip not installed: no effects */
        }
        set.idle = loadEffect(bu, sc.fx_idle, cfg.palette,
                              Fx::FallbackRole::Position);
        set.aux = loadEffect(bu, sc.fx_aux, cfg.palette,
                             Fx::FallbackRole::Position);
        set.brake = loadEffect(bu, sc.fx_brake, cfg.palette,
                               Fx::FallbackRole::Brake);
        set.turn_on = loadEffect(bu, sc.fx_turn_on, cfg.palette,
                                 Fx::FallbackRole::TurnOn);
        set.turn_off = loadEffect(bu, sc.fx_turn_off, cfg.palette,
                                  Fx::FallbackRole::TurnOff);
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

void getStats(uint32_t *fps_x10, uint32_t *frame_us_max)
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

const char *warnings()
{
    return m_warnings; /* read-mostly; short bounded string */
}

uint16_t getFrame(const StripId strip, uint8_t *out_rgb, const uint16_t max_leds)
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

esp_err_t start(const int *strip_gpios)
{
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        m_strip_gpios[i] = strip_gpios[i];
    }
    m_ctrl_q = xQueueCreate(CTRL_QUEUE_DEPTH, sizeof(RenderCmd));
    if (nullptr == m_ctrl_q)
    {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        renderTask, "render", RENDER_TASK_STACK_BYTES, nullptr,
        RENDER_TASK_PRIORITY, nullptr, RENDER_CORE_ID);
    return (pdPASS == ok) ? ESP_OK : ESP_FAIL;
}

} // namespace RenderCore

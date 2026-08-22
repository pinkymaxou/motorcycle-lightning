#include "status_led.h"

#include <atomic>
#include "led_strip.h"
#include "soc/soc_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tasks.hpp"

namespace StatusLed
{

namespace
{

/* 2 Hz alternation: the color changes every 250 ms. */
constexpr uint32_t BLINK_HALF_PERIOD_US = 250 * 1000;
constexpr uint32_t RMT_RESOLUTION_HZ = 10 * 1000 * 1000;
/* One block for one pixel; led_driver.cpp budgets the remaining blocks
 * between the strips and assumes exactly this much is taken here. */
constexpr size_t RMT_MEM_BLOCK_SYMBOLS = SOC_RMT_MEM_WORDS_PER_CHANNEL;

struct Rgb
{
    uint8_t r, g, b;
};

/* State colors (kept dim: the pixel sits right next to the rider). Blink
 * states alternate between their two colors at 2 Hz. */
constexpr Rgb COLOR_OFF = { 0, 0, 0 };
constexpr Rgb COLOR_BOOT = { 0, 0, 40 };
constexpr Rgb COLOR_OK_GREEN = { 0, 14, 0 };
constexpr Rgb COLOR_OK_BLUE = { 0, 0, 14 };
constexpr Rgb COLOR_NET_ERROR = { 40, 12, 0 };      /* orange */
constexpr Rgb COLOR_CFG_FALLBACK = { 30, 0, 30 };   /* purple */

static led_strip_handle_t m_strip;
static esp_timer_handle_t m_timer;
static std::atomic<State> m_state{ State::Boot };
static bool m_phase;
static TaskHandle_t m_init_waiter;

void tickCb(void* arg)
{
    (void)arg;
    m_phase = !m_phase;
    Rgb c = COLOR_OFF;
    switch (m_state.load())
    {
    case State::Boot:
        c = COLOR_BOOT;
        break;
    case State::RunningWifi:
        /* running, config WiFi up */
        c = m_phase ? COLOR_OK_GREEN : COLOR_OK_BLUE;
        break;
    case State::Running:
        /* running, WiFi off */
        c = m_phase ? COLOR_OK_GREEN : COLOR_OFF;
        break;
    case State::NetError:
        c = m_phase ? COLOR_NET_ERROR : COLOR_OFF;
        break;
    case State::ConfigFallback:
        c = m_phase ? COLOR_CFG_FALLBACK : COLOR_OFF;
        break;
    }
    led_strip_set_pixel(m_strip, 0, c.r, c.g, c.b);
    led_strip_refresh(m_strip);
}

/* All RMT channels must allocate their interrupt on core 1: the ESP32 RMT
 * peripheral has a single shared interrupt source, and splitting its handlers
 * across cores deadlocks during flash operations (one core's handler masked
 * mid-stall leaves the source asserted, the other core's shared ISR storms
 * and trips the interrupt watchdog). Channel creation pins the ISR, so create
 * the channel from a throwaway core-1 task. */
void createOnCore1(void* arg)
{
    const int gpio = static_cast<int>(reinterpret_cast<intptr_t>(arg));

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = gpio;
    strip_cfg.max_leds = 1;
    /* Fixed on purpose: this pixel is soldered on the module and never
     * changes, so it must NOT follow the user-selectable strip type — that
     * setting belongs to the external strips only. SK6812, GRB order. */
    strip_cfg.led_model = LED_MODEL_SK6812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_cfg.flags.invert_out = false;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = RMT_RESOLUTION_HZ;
    rmt_cfg.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS;
    rmt_cfg.flags.with_dma = false;

    if (ESP_OK != led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &m_strip))
    {
        m_strip = nullptr;
    }
    xTaskNotifyGive(m_init_waiter);
    vTaskDelete(nullptr);
}

} // namespace

esp_err_t init(const int gpio)
{
    m_init_waiter = xTaskGetCurrentTaskHandle();
    if (pdPASS != Tasks::create(Tasks::STATUS_LED_INIT, createOnCore1,
                                reinterpret_cast<void*>(
                                    static_cast<intptr_t>(gpio))))
    {
        return ESP_FAIL;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    if (nullptr == m_strip)
    {
        return ESP_FAIL;
    }

    esp_timer_create_args_t targs = {};
    targs.callback = tickCb;
    targs.name = "status_led";
    const esp_err_t err = esp_timer_create(&targs, &m_timer);
    if (ESP_OK != err)
    {
        return err;
    }
    return esp_timer_start_periodic(m_timer, BLINK_HALF_PERIOD_US);
}

void set(const State state)
{
    m_state.store(state);
}

} // namespace StatusLed

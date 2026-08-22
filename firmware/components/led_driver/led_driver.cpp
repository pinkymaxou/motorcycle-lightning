#include "led_driver.h"

#include <cmath>
#include "led_strip.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "tasks.hpp"

namespace LedDriver
{

namespace
{

const char *const TAG = "led_driver";

constexpr uint32_t RMT_RESOLUTION_HZ = 10 * 1000 * 1000;
/* RMT memory budget. The chip has RMT_TX_CHANNELS blocks of
 * SOC_RMT_MEM_WORDS_PER_CHANNEL symbols and a channel's blocks must be
 * contiguous, so this is a hard ceiling: ask for more and the last strip
 * created simply gets no channel (ESP_ERR_NOT_FOUND) instead of degrading.
 * The status LED holds one block; the strips split the rest evenly. Three
 * blocks each is a 120 us refill window, enough that a WiFi/httpd burst
 * can't starve the encoder (the ESP32 has no RMT DMA). */
constexpr size_t RMT_TX_CHANNELS = 8;
constexpr size_t RMT_TOTAL_SYMBOLS = RMT_TX_CHANNELS * SOC_RMT_MEM_WORDS_PER_CHANNEL;
/* Keep in sync with status_led.cpp, which allocates its own channel. */
constexpr size_t RMT_STATUS_LED_SYMBOLS = SOC_RMT_MEM_WORDS_PER_CHANNEL;
constexpr size_t RMT_MEM_BLOCK_SYMBOLS =
    (RMT_TOTAL_SYMBOLS - RMT_STATUS_LED_SYMBOLS) / STRIP_COUNT /
    SOC_RMT_MEM_WORDS_PER_CHANNEL * SOC_RMT_MEM_WORDS_PER_CHANNEL;
static_assert(RMT_STATUS_LED_SYMBOLS + RMT_MEM_BLOCK_SYMBOLS * STRIP_COUNT <=
                  RMT_TOTAL_SYMBOLS,
              "RMT memory budget exceeded: a strip would get no channel");
constexpr float GAMMA = 2.2f;
/* 8-bit value -> 16-bit component (WS2816): 255 * 257 == 65535. */
constexpr uint32_t WIDE_SCALE = 257;

/* One entry per physical output. */
struct Strip
{
    led_strip_handle_t handle;
    int gpio;
    bool reversed;
    LedModel model;
    ColorOrder order;
    bool wide;          /* 16 bits per color component (WS2816) */
};

/* The creation of the RMT channels is handed to a throwaway task pinned to
 * the lighting core (all RMT channels must be born there), so init() can be
 * called from app_main on core 0 — early enough to blank the strips before
 * anything else in the boot sequence can fail. */
constexpr uint32_t INIT_TIMEOUT_MS = 2000;

static Strip m_strips[STRIP_COUNT];
static int m_n_strips;
static TaskHandle_t m_init_waiter;
static esp_err_t m_init_err;
static uint16_t m_max_leds;
static uint8_t m_gamma_lut[256];

bool validStrip(const StripId strip)
{
    const int i = stripIndex(strip);
    return i >= 0 && i < m_n_strips;
}

led_model_t modelToDriver(const LedModel model)
{
    switch (model)
    {
    case LedModel::SK6812:
        return LED_MODEL_SK6812;
    case LedModel::WS2811:
        return LED_MODEL_WS2811;
    case LedModel::WS2816:
        return LED_MODEL_WS2816;
    default:
        return LED_MODEL_WS2812;
    }
}

led_color_component_format_t formatFor(const ColorOrder order, const bool wide)
{
    switch (order)
    {
    case ColorOrder::RGB:
        return wide ? LED_STRIP_COLOR_COMPONENT_FMT_RGB_16
                    : LED_STRIP_COLOR_COMPONENT_FMT_RGB;
    case ColorOrder::GRBW:
        return wide ? LED_STRIP_COLOR_COMPONENT_FMT_GRBW_16
                    : LED_STRIP_COLOR_COMPONENT_FMT_GRBW;
    case ColorOrder::RGBW:
        return wide ? LED_STRIP_COLOR_COMPONENT_FMT_RGBW_16
                    : LED_STRIP_COLOR_COMPONENT_FMT_RGBW;
    default:
        return wide ? LED_STRIP_COLOR_COMPONENT_FMT_GRB_16
                    : LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    }
}

/* (Re)create one strip's RMT device for its current model/order. Caller must
 * run on the render task: every RMT channel has to be born on core 1. */
esp_err_t createStrip(const int index)
{
    Strip &st = m_strips[index];
    if (nullptr != st.handle)
    {
        led_strip_del(st.handle);
        st.handle = nullptr;
    }

    st.wide = (LedModel::WS2816 == st.model);

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = st.gpio;
    strip_cfg.max_leds = m_max_leds;
    strip_cfg.led_model = modelToDriver(st.model);
    strip_cfg.color_component_format = formatFor(st.order, st.wide);
    strip_cfg.flags.invert_out = false;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = RMT_RESOLUTION_HZ;
    rmt_cfg.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS;
    rmt_cfg.flags.with_dma = false;

    const esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &st.handle);
    if (ESP_OK != err)
    {
        ESP_LOGE(TAG, "strip %d init failed: %s", index, esp_err_to_name(err));
        st.handle = nullptr;
        return err;
    }
    led_strip_clear(st.handle);
    return ESP_OK;
}

void createOnCore1(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < m_n_strips; i++)
    {
        /* createStrip() ends on led_strip_clear(): the strips are latched
         * black by the time this returns. */
        const esp_err_t e = createStrip(i);
        if (ESP_OK != e)
        {
            err = e;
        }
    }
    m_init_err = err;
    xTaskNotifyGive(m_init_waiter);
    vTaskDelete(nullptr);
}

inline uint8_t shade(const uint8_t c)
{
    return m_gamma_lut[c];
}

} // namespace

esp_err_t init(const int *gpios, const int count, const uint16_t max_leds)
{
    /* Gamma LUT. The browser shows raw authored values on an sRGB display;
     * this LUT makes the (linear) LEDs match that perception. */
    for (int i = 0; i < 256; i++)
    {
        const float v = std::pow(static_cast<float>(i) / 255.0f, GAMMA) * 255.0f;
        m_gamma_lut[i] = static_cast<uint8_t>(std::lround(v));
    }

    m_max_leds = max_leds;
    m_n_strips = (count < STRIP_COUNT) ? count : STRIP_COUNT;

    for (int i = 0; i < m_n_strips; i++)
    {
        m_strips[i].gpio = gpios[i];
        m_strips[i].model = LedModel::WS2812;
        m_strips[i].order = ColorOrder::GRB;
    }

    m_init_err = ESP_FAIL;
    m_init_waiter = xTaskGetCurrentTaskHandle();
    if (pdPASS != Tasks::create(Tasks::STRIP_INIT, createOnCore1, nullptr))
    {
        return ESP_FAIL;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INIT_TIMEOUT_MS));
    return m_init_err;
}

void setReversed(const StripId strip, const bool reversed)
{
    if (validStrip(strip))
    {
        m_strips[stripIndex(strip)].reversed = reversed;
    }
}

esp_err_t setLedType(const StripId strip, const LedModel model,
                     const ColorOrder order)
{
    if (!validStrip(strip))
    {
        return ESP_ERR_INVALID_ARG;
    }
    Strip &st = m_strips[stripIndex(strip)];
    if (model == st.model && order == st.order && nullptr != st.handle)
    {
        return ESP_OK;
    }
    st.model = model;
    st.order = order;
    ESP_LOGI(TAG, "strip %d type: model %d, order %d", stripIndex(strip),
             static_cast<int>(model), static_cast<int>(order));
    return createStrip(stripIndex(strip));
}

esp_err_t write(const StripId strip, const uint8_t *rgb, uint16_t count)
{
    if (!validStrip(strip) || nullptr == m_strips[stripIndex(strip)].handle)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const Strip &st = m_strips[stripIndex(strip)];
    if (count > m_max_leds)
    {
        count = m_max_leds;
    }

    for (uint16_t i = 0; i < count; i++)
    {
        const uint16_t src = st.reversed ? static_cast<uint16_t>(count - 1 - i) : i;
        const uint32_t r = shade(rgb[src * 3 + 0]);
        const uint32_t g = shade(rgb[src * 3 + 1]);
        const uint32_t b = shade(rgb[src * 3 + 2]);
        /* The driver places each component per the configured wire order and
         * writes 0 to the W channel of RGBW strips. */
        if (st.wide)
        {
            led_strip_set_pixel(st.handle, i, r * WIDE_SCALE, g * WIDE_SCALE,
                                b * WIDE_SCALE);
        }
        else
        {
            led_strip_set_pixel(st.handle, i, r, g, b);
        }
    }
    return led_strip_refresh(st.handle);
}

} // namespace LedDriver

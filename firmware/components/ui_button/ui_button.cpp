#include "ui_button.h"

#include "driver/gpio.h"
#include "esp_timer.h"

namespace UiButton
{

namespace
{

constexpr uint32_t POLL_PERIOD_US = 50 * 1000;
constexpr uint8_t DEBOUNCE_POLLS = 3;       /* 3 x 50 ms */

constexpr uint32_t HOLD_POLLS = HOLD_FACTORY_MS / (POLL_PERIOD_US / 1000);

static int m_gpio;
static Callback m_cb;
static Callback m_hold_cb;
static uint8_t m_stable;
static bool m_pressed;
static uint32_t m_held_polls;
static bool m_hold_fired;

void pollCb(void* arg)
{
    (void)arg;
    const bool raw = 0 == gpio_get_level(static_cast<gpio_num_t>(m_gpio));
    if (raw == m_pressed)
    {
        m_stable = 0;
        if (m_pressed && !m_hold_fired && ++m_held_polls >= HOLD_POLLS)
        {
            m_hold_fired = true;      /* once per press, still held */
            if (nullptr != m_hold_cb)
            {
                m_hold_cb();
            }
        }
        return;
    }
    if (++m_stable >= DEBOUNCE_POLLS)
    {
        m_stable = 0;
        m_pressed = raw;
        if (m_pressed)
        {
            m_held_polls = 0;
            m_hold_fired = false;
        }
        /* Act on release, so a hold that reached the factory reset does not
         * also toggle the WiFi on its way there. */
        else if (!m_hold_fired && nullptr != m_cb)
        {
            m_cb();
        }
    }
}

} // namespace

esp_err_t init(const int gpio, const Callback on_press, const Callback on_hold)
{
    m_gpio = gpio;
    m_cb = on_press;
    m_hold_cb = on_hold;

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gpio;
    io.mode = GPIO_MODE_INPUT;
    /* Pressed = low. Input-only pads (GPIO 34-39, the Stamp Pico's button
     * sits on 39) have no internal pull resistor and rely on the module's
     * external one. */
    io.pull_up_en = GPIO_IS_VALID_OUTPUT_GPIO(gpio) ? GPIO_PULLUP_ENABLE
                                                    : GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io);
    if (ESP_OK != err)
    {
        return err;
    }

    esp_timer_handle_t t;
    esp_timer_create_args_t targs = {};
    targs.callback = pollCb;
    targs.name = "ui_button";
    err = esp_timer_create(&targs, &t);
    if (ESP_OK != err)
    {
        return err;
    }
    return esp_timer_start_periodic(t, POLL_PERIOD_US);
}

} // namespace UiButton

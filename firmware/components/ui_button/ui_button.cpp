#include "ui_button.h"

#include "driver/gpio.h"
#include "esp_timer.h"

namespace UiButton
{

namespace
{

constexpr uint32_t POLL_PERIOD_US = 50 * 1000;
constexpr uint8_t DEBOUNCE_POLLS = 3;       /* 3 x 50 ms */

static int m_gpio;
static Callback m_cb;
static uint8_t m_stable;
static bool m_pressed;

void pollCb(void* arg)
{
    (void)arg;
    const bool raw = 0 == gpio_get_level(static_cast<gpio_num_t>(m_gpio));
    if (raw == m_pressed)
    {
        m_stable = 0;
        return;
    }
    if (++m_stable >= DEBOUNCE_POLLS)
    {
        m_stable = 0;
        m_pressed = raw;
        if (m_pressed && nullptr != m_cb)
        {
            m_cb();
        }
    }
}

} // namespace

esp_err_t init(const int gpio, const Callback on_press)
{
    m_gpio = gpio;
    m_cb = on_press;

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

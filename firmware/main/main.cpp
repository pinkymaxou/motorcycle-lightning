/* Boot order is safety-first: lighting runs from compiled-in fallbacks before
 * storage or network are touched; every later stage only upgrades it. */
#include <atomic>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "board_pins.h"
#if __has_include("wifi_creds.h")
#include "wifi_creds.h"           /* gitignored — see wifi_creds.h.example */
#else
#define WIFI_STA_SSID ""
#define WIFI_STA_PASS ""
#endif
#include "config_store.h"
#include "input_conditioner.h"
#include "crash_log.h"
#include "led_driver.h"
#include "render_core.h"
#include "net_services.h"
#include "status_led.h"
#include "ui_button.h"
#include "dev_console.h"

namespace
{

const char* const TAG = "main";

constexpr uint32_t HOUSEKEEPING_PERIOD_MS = 250;

static SysConfig m_cfg;   /* authoritative live config (mutated by httpd) */

enum class NetRequest : uint8_t
{
    None = 0,
    Toggle,
    On,
    Off
};

static std::atomic<NetRequest> m_net_request{ NetRequest::None };

/* Runs in the shared esp_timer task: starting/stopping WiFi is slow, so only
 * flag the request — the housekeeping loop does the heavy lifting. */
void onButtonPress()
{
    m_net_request.store(NetRequest::Toggle);
}

void applyNetRequest(const NetRequest req, const char* source)
{
    const bool running = NetServices::running();
    const bool want_on = (NetRequest::On == req) ||
                         (NetRequest::Toggle == req && !running);

    if (want_on == running)
    {
        ESP_LOGI(TAG, "%s: config WiFi already %s", source, running ? "ON" : "OFF");
        return;
    }
    if (!want_on)
    {
        NetServices::stop();
        StatusLed::set(StatusLed::State::Running);
        ESP_LOGI(TAG, "%s: config WiFi OFF", source);
        return;
    }
    if (ESP_OK == NetServices::start(&m_cfg))
    {
        StatusLed::set(StatusLed::State::RunningWifi);
        ESP_LOGI(TAG, "%s: config WiFi ON", source);
    }
    else
    {
        StatusLed::set(StatusLed::State::NetError);
        ESP_LOGE(TAG, "%s: WiFi start failed", source);
    }
}

/* Console commands (see DevConsole). Kept tiny on purpose: the page is the
 * real interface, this only has to unblock a bench session. */
void onConsoleLine(const char* line)
{
    if (0 == std::strcmp(line, "wifi on"))
    {
        m_net_request.store(NetRequest::On);
    }
    else if (0 == std::strcmp(line, "wifi off"))
    {
        m_net_request.store(NetRequest::Off);
    }
    else if (0 == std::strcmp(line, "wifi"))
    {
        ESP_LOGI(TAG, "config WiFi is %s", NetServices::running() ? "ON" : "OFF");
    }
    else if (0 == std::strcmp(line, "crashlog"))
    {
        const char* names[CrashLog::CRASH_LOG_ENTRIES];
        const int n = CrashLog::snapshot(names, CrashLog::CRASH_LOG_ENTRIES);
        ESP_LOGI(TAG, "%s", CrashLog::summary());
        for (int i = 0; i < n; i++)
        {
            ESP_LOGI(TAG, "  %d: %s", i + 1, names[i]);
        }
    }
    else if (0 == std::strcmp(line, "crashlog clear"))
    {
        CrashLog::clear();
        ESP_LOGI(TAG, "crash log cleared");
    }
    else if (0 == std::strcmp(line, "reboot"))
    {
        ESP_LOGW(TAG, "rebooting on console request");
        esp_restart();
    }
    else
    {
        ESP_LOGI(TAG, "commands: wifi | wifi on | wifi off | crashlog"
                      " | crashlog clear | reboot");
    }
}

/* Shown on the System page — board_pins.h is the source of truth. */
constexpr NetServices::PinDef PINOUT[] = {
    { "Strip 1 data", PIN_STRIP_1,
      "WS2812B data out, main strip (through the PCB's 5V level shifter)" },
    { "Strip 2 data", PIN_STRIP_2,
      "WS2812B data out, second strip (level-shifted, own config)" },
    { "Input LEFT", PIN_IN_LEFT,
      "Left turn signal, opto-isolated, active low (12V ON pulls it low)" },
    { "Input RIGHT", PIN_IN_RIGHT,
      "Right turn signal, opto-isolated, active low" },
    { "Input BRAKE", PIN_IN_BRAKE,
      "Brake light, opto-isolated, active low (input-only GPIO)" },
    { "Input AUX", PIN_IN_AUX,
      "Extra 12V input, opto-isolated, active low" },
    { "Button", PIN_BUTTON,
      "On-module button, pressed = low: toggles the config WiFi (off at boot)" },
    { "Status LED", PIN_STATUS_LED,
      "On-module RGB pixel, 2 Hz: green/blue = WiFi on, green blink = WiFi off" },
};

} // namespace

extern "C" void app_main()
{
    /* 1. Strips dark, before anything else can fail. A reset leaves the LEDs
     *    holding their last latched frame as long as 5 V is present, so the
     *    very first thing the firmware does is latch black: if the boot dies
     *    from here on, the bar stays off. This is supplementary lighting —
     *    dark is a correct outcome, a stale or wrong frame is not. */
    constexpr int STRIP_GPIOS[STRIP_COUNT] = { PIN_STRIP_1, PIN_STRIP_2 };
    if (ESP_OK != LedDriver::init(STRIP_GPIOS, STRIP_COUNT, CFG_MAX_LEDS))
    {
        ESP_LOGE(TAG, "strip init failed — lighting stays dark, check wiring");
    }

    /* 2. NVS (recover from corruption by erasing) */
    esp_err_t err = nvs_flash_init();
    if (ESP_ERR_NVS_NO_FREE_PAGES == err || ESP_ERR_NVS_NEW_VERSION_FOUND == err)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    CrashLog::init();
    ESP_LOGI(TAG, "crash log: %s", CrashLog::summary());

    /* 3. status LED */
    StatusLed::init(PIN_STATUS_LED);
    StatusLed::set(StatusLed::State::Boot);

    /* 4. config (any failure -> compiled defaults, never halt) */
    bool cfg_fallback = false;
    if (ESP_OK != ConfigStore::init() || ESP_OK != ConfigStore::load(&m_cfg))
    {
        ConfigStore::defaults(&m_cfg);
        cfg_fallback = true;
        ESP_LOGW(TAG, "using compiled default config");
    }
    /* seed the STA settings from the optional compiled-in credentials */
    if ('\0' == m_cfg.sta_ssid[0] && '\0' != WIFI_STA_SSID[0])
    {
        strlcpy(m_cfg.sta_ssid, WIFI_STA_SSID, sizeof(m_cfg.sta_ssid));
        strlcpy(m_cfg.sta_pass, WIFI_STA_PASS, sizeof(m_cfg.sta_pass));
        m_cfg.sta_active = true;
        ConfigStore::save(&m_cfg);
    }

    /* 5. inputs (with persisted flasher period) + render task.
     *    Lighting is live from here, on hard-fallback effects. */
    const uint32_t stored_period = ConfigStore::loadBlinkPeriod();
    const InputConditioner::InputPins pins = {
        PIN_IN_LEFT, PIN_IN_RIGHT, PIN_IN_BRAKE, PIN_IN_AUX
    };
    ESP_ERROR_CHECK(InputConditioner::init(pins, stored_period,
                                           m_cfg.blink_exit_x10));
    InputConditioner::setBrakeHoldoff(
        static_cast<uint32_t>(m_cfg.brake_holdoff_s) * 1000);
    ESP_ERROR_CHECK(RenderCore::start());

    /* 6. configured effect set (per-effect fallback inside) */
    RenderCore::applyConfig(m_cfg);

    /* 7. network stays OFF at boot — riding needs no radio. The module
     *    button brings the config WiFi up on demand. */
    NetServices::setPinout(PINOUT, sizeof(PINOUT) / sizeof(PINOUT[0]));
    ESP_LOGI(TAG, "config WiFi off — press the module button or type "
                  "'wifi on' to enable it");

    /* 8. button hook + serial console ("wifi on" brings the page up) */
    UiButton::init(PIN_BUTTON, onButtonPress);
    DevConsole::start(onConsoleLine);

    if (cfg_fallback)
    {
        StatusLed::set(StatusLed::State::ConfigFallback);
    }
    else
    {
        StatusLed::set(StatusLed::State::Running);
    }

    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const StripConfig& sc = m_cfg.strips[i];
        const uint16_t total = stripTotalLeds(sc);
        if (0 == total)
        {
            ESP_LOGI(TAG, "strip %d: not installed", i + 1);
            continue;
        }
        char layout[96] = {};
        size_t used = 0;
        for (int k = 0; k < sc.n_sections && used + 12 < sizeof(layout); k++)
        {
            const SectionConfig& sec = sc.sections[k];
            const char* const turn = (TurnSource::Left == sec.turn)    ? "L"
                                     : (TurnSource::Right == sec.turn) ? "R"
                                                                       : "-";
            used += snprintf(layout + used, sizeof(layout) - used, "[%u %s%s]",
                             sec.led_count, turn, sec.reversed ? " rev" : "");
        }
        ESP_LOGI(TAG, "strip %d: %u LEDs, %u sections %s", i + 1, total,
                 sc.n_sections, layout);
    }
    ESP_LOGI(TAG, "flasher period %ums", static_cast<unsigned>(stored_period));

    /* Housekeeping: persist the learned flasher period outside the timer and
     * render contexts (NVS writes block). */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(HOUSEKEEPING_PERIOD_MS));
        const NetRequest req = m_net_request.exchange(NetRequest::None);
        if (NetRequest::None != req)
        {
            applyNetRequest(req, (NetRequest::Toggle == req) ? "button" : "console");
        }
        uint32_t period;
        if (InputConditioner::takeDirtyPeriod(&period))
        {
            ConfigStore::saveBlinkPeriod(period);
        }
    }
}

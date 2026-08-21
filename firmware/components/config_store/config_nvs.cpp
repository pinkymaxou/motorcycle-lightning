#include "config_store.h"
#include "factory_effects.h"

#include <cstring>
#include "nvs.h"
#include "esp_log.h"

namespace ConfigStore
{

namespace
{

const char *const TAG = "config_nvs";

constexpr const char *NVS_NS = "motolight";
constexpr const char *KEY_SYSCFG = "syscfg";
constexpr const char *KEY_BLINK_MS = "blinkms";

static nvs_handle_t m_nvs;

} // namespace

namespace
{

/* Defaults for one strip. Strip 2 ships disabled (led_count 0) but fully
 * filled in, so enabling it from the page just works. */
void stripDefaults(StripConfig *sc, const uint16_t led_count)
{
    sc->led_count = led_count;
    sc->brightness = 160;
    sc->led_model = LedModel::WS2812;
    sc->color_order = ColorOrder::GRB;
    sc->reversed = false;
    sc->zone_left_end = 12;
    sc->zone_center_end = 28;

    std::strcpy(sc->fx_idle, "f_position");
    sc->fx_aux[0] = '\0';
    std::strcpy(sc->fx_brake, "f_brake");
    std::strcpy(sc->fx_turn_on, "f_turn_on");
    std::strcpy(sc->fx_turn_off, "f_turn_off");
    sc->brake_zone = ZoneId::Full;
    sc->aux_zone = ZoneId::Full;
}

/* A corrupt-but-right-sized blob must not hand out unterminated strings. */
bool stringOk(const char *s, const size_t cap)
{
    return nullptr != std::memchr(s, '\0', cap);
}

bool stripValid(const StripConfig &sc)
{
    if (!stringOk(sc.fx_idle, sizeof(sc.fx_idle)) ||
        !stringOk(sc.fx_aux, sizeof(sc.fx_aux)) ||
        !stringOk(sc.fx_brake, sizeof(sc.fx_brake)) ||
        !stringOk(sc.fx_turn_on, sizeof(sc.fx_turn_on)) ||
        !stringOk(sc.fx_turn_off, sizeof(sc.fx_turn_off)))
    {
        return false;
    }
    if (sc.led_count > CFG_MAX_LEDS)
    {
        return false;
    }
    /* A strip with no LEDs is simply not installed: its geometry is dormant
     * (the defaults keep sane zones so enabling it later just works). */
    if (0 != sc.led_count &&
        (sc.zone_left_end > sc.zone_center_end ||
         sc.zone_center_end > sc.led_count))
    {
        return false;
    }
    if (sc.brake_zone > ZoneId::Last || sc.aux_zone > ZoneId::Last)
    {
        return false;
    }
    if (sc.led_model > LedModel::Last || sc.color_order > ColorOrder::Last)
    {
        return false;
    }
    return true;
}

} // namespace

void defaults(SysConfig *cfg)
{
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->version = CFG_VERSION;

    stripDefaults(&cfg->strips[stripIndex(StripId::Strip1)], 40);
    stripDefaults(&cfg->strips[stripIndex(StripId::Strip2)], 0);

    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        cfg->palette.colors[i] = Fx::defaultColor(static_cast<Fx::FxColor>(i));
    }

    cfg->blink_exit_x10 = 12;   /* period + 20% grace for the next flash */
    cfg->brake_holdoff_s = 25;

    cfg->sta_ssid[0] = '\0';
    cfg->sta_pass[0] = '\0';
    cfg->sta_active = false;
}

bool validate(const SysConfig *cfg)
{
    if (CFG_VERSION != cfg->version)
    {
        return false;
    }
    if (!stringOk(cfg->sta_ssid, sizeof(cfg->sta_ssid)) ||
        !stringOk(cfg->sta_pass, sizeof(cfg->sta_pass)))
    {
        return false;
    }
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        if (!stripValid(cfg->strips[i]))
        {
            return false;
        }
    }
    if (cfg->blink_exit_x10 < 10 || cfg->blink_exit_x10 > 50)
    {
        return false;
    }
    if (cfg->brake_holdoff_s > 600)
    {
        return false;
    }
    return true;
}

esp_err_t init()
{
    return nvs_open(NVS_NS, NVS_READWRITE, &m_nvs);
}

esp_err_t load(SysConfig *cfg)
{
    size_t len = sizeof(*cfg);
    const esp_err_t err = nvs_get_blob(m_nvs, KEY_SYSCFG, cfg, &len);
    if (ESP_OK != err)
    {
        return err;
    }
    if (sizeof(*cfg) != len || !validate(cfg))
    {
        ESP_LOGW(TAG, "stored config invalid (len=%u)", static_cast<unsigned>(len));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t save(const SysConfig *cfg)
{
    esp_err_t err = nvs_set_blob(m_nvs, KEY_SYSCFG, cfg, sizeof(*cfg));
    if (ESP_OK == err)
    {
        err = nvs_commit(m_nvs);
    }
    return err;
}

uint32_t loadBlinkPeriod()
{
    uint32_t v = 0;
    if (ESP_OK != nvs_get_u32(m_nvs, KEY_BLINK_MS, &v))
    {
        return 0;
    }
    return v;
}

esp_err_t saveBlinkPeriod(const uint32_t period_ms)
{
    esp_err_t err = nvs_set_u32(m_nvs, KEY_BLINK_MS, period_ms);
    if (ESP_OK == err)
    {
        err = nvs_commit(m_nvs);
    }
    ESP_LOGI(TAG, "persisted blink period %u ms", static_cast<unsigned>(period_ms));
    return err;
}

} // namespace ConfigStore

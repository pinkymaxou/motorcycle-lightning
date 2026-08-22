#include "config_store.h"
#include "factory_effects.h"

#include <cstring>
#include "nvs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

namespace ConfigStore
{

namespace
{

const char *const TAG = "config_nvs";

constexpr const char *NVS_NS = "motolight";
constexpr const char *KEY_SYSCFG = "syscfg";
constexpr const char *KEY_BLINK_MS = "blinkms";

static nvs_handle_t m_nvs;

/* What actually sits in NVS: the struct plus a CRC over it. NVS checks its
 * own page integrity, so this guards the bytes between here and there — a
 * half-written or truncated blob of the right length would otherwise sail
 * through validate() whenever the damaged fields happen to be in range. */
struct StoredConfig
{
    uint32_t  crc;      /* over cfg only, the field itself excluded */
    SysConfig cfg;
};

/* 1.5 KB staging: save() and load() are called from low-priority task
 * context, whose stacks are not sized for a second copy of the config. */
static StoredConfig m_blob;

uint32_t configCrc(const SysConfig &cfg)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&cfg),
                            sizeof(cfg));
}

} // namespace

namespace
{

/* Defaults for one strip, from the shared factory layout. Strip 2 ships with
 * no sections (not installed) but sane hardware, so adding a section from the
 * page is all it takes to enable it. */
void stripDefaults(StripConfig *sc, const bool installed)
{
    sc->brightness = 160;
    sc->led_model = LedModel::WS2812;
    sc->color_order = ColorOrder::GRB;
    sc->reversed = false;
    sc->n_sections = installed ? CFG_DEFAULT_SECTION_COUNT : 0;

    for (int i = 0; i < CFG_DEFAULT_SECTION_COUNT; i++)
    {
        const DefaultSection &def = CFG_DEFAULT_SECTIONS[i];
        SectionConfig *const sec = &sc->sections[i];
        sec->led_count = def.led_count;
        sec->reversed = def.reversed;
        sec->turn = def.turn;

        std::strcpy(sec->fx_idle, "f_position");
        sec->fx_aux[0] = '\0';
        std::strcpy(sec->fx_brake, "f_brake");
        if (TurnSource::None != def.turn)
        {
            std::strcpy(sec->fx_turn_on, "f_turn_on");
            std::strcpy(sec->fx_turn_off, "f_turn_off");
        }
        else
        {
            sec->fx_turn_on[0] = '\0';
            sec->fx_turn_off[0] = '\0';
        }
    }
}

/* A corrupt-but-right-sized blob must not hand out unterminated strings. */
bool stringOk(const char *s, const size_t cap)
{
    return nullptr != std::memchr(s, '\0', cap);
}

bool sectionStringsOk(const SectionConfig &sec)
{
    return stringOk(sec.fx_idle, sizeof(sec.fx_idle)) &&
           stringOk(sec.fx_aux, sizeof(sec.fx_aux)) &&
           stringOk(sec.fx_brake, sizeof(sec.fx_brake)) &&
           stringOk(sec.fx_turn_on, sizeof(sec.fx_turn_on)) &&
           stringOk(sec.fx_turn_off, sizeof(sec.fx_turn_off));
}

bool stripValid(const StripConfig &sc)
{
    if (sc.n_sections > CFG_MAX_SECTIONS)
    {
        return false;
    }
    if (sc.led_model > LedModel::Last || sc.color_order > ColorOrder::Last)
    {
        return false;
    }

    uint32_t total = 0;
    for (int i = 0; i < CFG_MAX_SECTIONS; i++)
    {
        const SectionConfig &sec = sc.sections[i];
        /* every slot, used or not: a right-sized but corrupt blob must never
         * hand out an unterminated id */
        if (!sectionStringsOk(sec))
        {
            return false;
        }
        if (i >= sc.n_sections)
        {
            continue;
        }
        if (sec.turn > TurnSource::Last || sec.led_count > CFG_MAX_LEDS)
        {
            return false;
        }
        total += sec.led_count;
    }
    /* A zero-length section is a legal placeholder (the page creates one
     * before you type a count); a strip whose sections sum to zero is simply
     * not installed. */
    return total <= CFG_MAX_LEDS;
}

} // namespace

void defaults(SysConfig *cfg)
{
    std::memset(cfg, 0, sizeof(*cfg));
    cfg->version = CFG_VERSION;

    stripDefaults(&cfg->strips[stripIndex(StripId::Strip1)], true);
    stripDefaults(&cfg->strips[stripIndex(StripId::Strip2)], false);

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
    size_t len = sizeof(m_blob);
    const esp_err_t err = nvs_get_blob(m_nvs, KEY_SYSCFG, &m_blob, &len);
    if (ESP_OK != err)
    {
        /* A blob larger than the buffer fails here rather than at the size
         * check below (that is how a config from an older layout shows up),
         * so say which error it was instead of falling back silently. */
        if (ESP_ERR_NVS_NOT_FOUND != err)
        {
            ESP_LOGW(TAG, "stored config unreadable: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (sizeof(m_blob) != len)
    {
        ESP_LOGW(TAG, "stored config size %u, expected %u",
                 static_cast<unsigned>(len), static_cast<unsigned>(sizeof(m_blob)));
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t crc = configCrc(m_blob.cfg);
    if (crc != m_blob.crc)
    {
        ESP_LOGW(TAG, "stored config CRC %08x, expected %08x",
                 static_cast<unsigned>(m_blob.crc), static_cast<unsigned>(crc));
        return ESP_ERR_INVALID_CRC;
    }
    if (!validate(&m_blob.cfg))
    {
        ESP_LOGW(TAG, "stored config failed validation");
        return ESP_ERR_INVALID_STATE;
    }
    *cfg = m_blob.cfg;
    return ESP_OK;
}

esp_err_t save(const SysConfig *cfg)
{
    /* CRC the copy that is about to be written, not the caller's struct:
     * padding bytes are whatever the assignment left behind, and the two
     * must agree byte for byte. */
    m_blob.cfg = *cfg;
    m_blob.crc = configCrc(m_blob.cfg);

    esp_err_t err = nvs_set_blob(m_nvs, KEY_SYSCFG, &m_blob, sizeof(m_blob));
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

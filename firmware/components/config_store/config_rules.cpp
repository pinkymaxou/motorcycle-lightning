/* Compiled-in defaults and the sanity rules a configuration must satisfy.
 * No NVS, no ESP-IDF: this is the part worth testing on a host. */
#include "config_store.h"
#include "factory_effects.h"

#include <cstring>

namespace ConfigStore
{

namespace
{

/* Defaults for one strip, from the shared factory layout. Strip 2 ships with
 * no sections (not installed) but sane hardware, so adding a section from the
 * page is all it takes to enable it. */
void stripDefaults(StripConfig* sc, const bool installed)
{
    sc->led_model = LedModel::WS2812;
    sc->color_order = ColorOrder::GRB;
    sc->reversed = false;
    sc->n_sections = installed ? CFG_DEFAULT_SECTION_COUNT : 0;

    for (int i = 0; i < CFG_DEFAULT_SECTION_COUNT; i++)
    {
        const DefaultSection& def = CFG_DEFAULT_SECTIONS[i];
        SectionConfig* const sec = &sc->sections[i];
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
bool stringOk(const char* s, const size_t cap)
{
    return nullptr != std::memchr(s, '\0', cap);
}

bool sectionStringsOk(const SectionConfig& sec)
{
    return stringOk(sec.fx_idle, sizeof(sec.fx_idle)) &&
           stringOk(sec.fx_aux, sizeof(sec.fx_aux)) &&
           stringOk(sec.fx_brake, sizeof(sec.fx_brake)) &&
           stringOk(sec.fx_turn_on, sizeof(sec.fx_turn_on)) &&
           stringOk(sec.fx_turn_off, sizeof(sec.fx_turn_off));
}

bool stripValid(const StripConfig& sc)
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
        const SectionConfig& sec = sc.sections[i];
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

void defaults(SysConfig* cfg)
{
    std::memset(cfg, 0, sizeof(*cfg));

    stripDefaults(&cfg->strips[stripIndex(StripId::Strip1)], true);
    stripDefaults(&cfg->strips[stripIndex(StripId::Strip2)], false);

    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        cfg->palette.colors[i] = Fx::defaultColor(static_cast<Fx::FxColor>(i));
    }

    /* empty: hazard looks like two turn signals until someone says otherwise */
    cfg->fx_hazard_on[0] = '\0';
    cfg->fx_hazard_off[0] = '\0';

    cfg->blink_exit_x10 = 12;   /* period + 20% grace for the next flash */
    cfg->brake_holdoff_s = 25;

    cfg->sta_ssid[0] = '\0';
    cfg->sta_pass[0] = '\0';
    cfg->sta_active = false;
}

bool validate(const SysConfig* cfg)
{
    if (!stringOk(cfg->fx_hazard_on, sizeof(cfg->fx_hazard_on)) ||
        !stringOk(cfg->fx_hazard_off, sizeof(cfg->fx_hazard_off)))
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

} // namespace ConfigStore

/* SysConfig <-> protobuf. One conversion, two users: the HTTP API sends it on
 * the wire, and config_nvs.cpp writes the very same encoding to flash — which
 * is what makes a schema change additive instead of a factory reset. */
#include "config_store.h"

#include <cstring>
#include "esp_log.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "ws_protocol.pb.h"

namespace ConfigStore
{

namespace
{

const char* const TAG = "config_proto";

/* Handlers run one at a time and the render task never touches these, so a
 * static message beats ~1.8 KB on a caller's stack. */
static motolights_Config m_msg;

uint32_t packColor(const Fx::RgbaColor& c)
{
    return (static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16) |
           (static_cast<uint32_t>(c.b) << 8) | c.a;
}

Fx::RgbaColor unpackColor(const uint32_t v)
{
    return Fx::RgbaColor{ static_cast<uint8_t>((v >> 24) & 0xFF),
                          static_cast<uint8_t>((v >> 16) & 0xFF),
                          static_cast<uint8_t>((v >> 8) & 0xFF),
                          static_cast<uint8_t>(v & 0xFF) };
}

void sectionToProto(const SectionConfig& sec, motolights_Section* const out)
{
    out->led_count = sec.led_count;
    out->reversed = sec.reversed;
    out->turn = static_cast<uint32_t>(sec.turn);
    strlcpy(out->idle, sec.fx_idle, sizeof(out->idle));
    strlcpy(out->brake, sec.fx_brake, sizeof(out->brake));
    strlcpy(out->turn_on, sec.fx_turn_on, sizeof(out->turn_on));
    strlcpy(out->turn_off, sec.fx_turn_off, sizeof(out->turn_off));
    strlcpy(out->aux, sec.fx_aux, sizeof(out->aux));
}

void sectionFromProto(const motolights_Section& in, SectionConfig* const sec)
{
    sec->led_count = static_cast<uint16_t>(in.led_count);
    sec->reversed = in.reversed;
    sec->turn = static_cast<TurnSource>(in.turn);
    strlcpy(sec->fx_idle, in.idle, sizeof(sec->fx_idle));
    strlcpy(sec->fx_brake, in.brake, sizeof(sec->fx_brake));
    strlcpy(sec->fx_turn_on, in.turn_on, sizeof(sec->fx_turn_on));
    strlcpy(sec->fx_turn_off, in.turn_off, sizeof(sec->fx_turn_off));
    strlcpy(sec->fx_aux, in.aux, sizeof(sec->fx_aux));
}

void stripToProto(const StripConfig& sc, motolights_Strip* const out)
{
    out->led_model = static_cast<uint32_t>(sc.led_model);
    out->color_order = static_cast<uint32_t>(sc.color_order);
    out->reversed = sc.reversed;

    out->sections_count = (sc.n_sections < CFG_MAX_SECTIONS) ? sc.n_sections
                                                             : CFG_MAX_SECTIONS;
    for (pb_size_t i = 0; i < out->sections_count; i++)
    {
        sectionToProto(sc.sections[i], &out->sections[i]);
    }
}

void stripFromProto(const motolights_Strip& in, StripConfig* const sc)
{
    *sc = StripConfig{};        /* the message is authoritative */
    sc->led_model = static_cast<LedModel>(in.led_model);
    sc->color_order = static_cast<ColorOrder>(in.color_order);
    sc->reversed = in.reversed;

    const pb_size_t count = (in.sections_count < CFG_MAX_SECTIONS)
                                ? in.sections_count
                                : CFG_MAX_SECTIONS;
    sc->n_sections = static_cast<uint8_t>(count);
    for (pb_size_t i = 0; i < count; i++)
    {
        sectionFromProto(in.sections[i], &sc->sections[i]);
    }
}

} // namespace

size_t encode(const SysConfig& cfg, uint8_t* const out, const size_t cap,
              const Secrets secrets)
{
    m_msg = motolights_Config_init_zero;

    m_msg.strips_count = STRIP_COUNT;
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        stripToProto(cfg.strips[i], &m_msg.strips[i]);
    }

    m_msg.colors_count = Fx::COLOR_COUNT;
    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        m_msg.colors[i] = packColor(cfg.palette.colors[i]);
    }

    strlcpy(m_msg.hazard_on, cfg.fx_hazard_on, sizeof(m_msg.hazard_on));
    strlcpy(m_msg.hazard_off, cfg.fx_hazard_off, sizeof(m_msg.hazard_off));
    m_msg.blink_exit_x10 = cfg.blink_exit_x10;
    m_msg.brake_holdoff_s = cfg.brake_holdoff_s;

    m_msg.has_sta = true;
    strlcpy(m_msg.sta.ssid, cfg.sta_ssid, sizeof(m_msg.sta.ssid));
    m_msg.sta.active = cfg.sta_active;
    m_msg.sta.pass_set = '\0' != cfg.sta_pass[0];
    if (Secrets::Include == secrets)
    {
        /* Storage only. Over the wire the password is write-only: the page
         * learns that one is set, never what it is. */
        strlcpy(m_msg.sta.pass, cfg.sta_pass, sizeof(m_msg.sta.pass));
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, motolights_Config_fields, &m_msg))
    {
        ESP_LOGE(TAG, "encode failed: %s", PB_GET_ERROR(&stream));
        return 0;
    }
    return stream.bytes_written;
}

bool decode(const uint8_t* const data, const size_t len, SysConfig* const cfg)
{
    m_msg = motolights_Config_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&stream, motolights_Config_fields, &m_msg))
    {
        ESP_LOGW(TAG, "decode failed: %s", PB_GET_ERROR(&stream));
        return false;
    }

    /* The message is authoritative: absent fields mean "unset", not "keep the
     * old value" — that is how the page clears an effect assignment. Fields
     * this build does not know are skipped, which is the whole point of
     * storing protobuf rather than a struct. */
    *cfg = SysConfig{};

    for (pb_size_t i = 0; i < m_msg.strips_count && i < STRIP_COUNT; i++)
    {
        stripFromProto(m_msg.strips[i], &cfg->strips[i]);
    }
    for (pb_size_t i = 0; i < m_msg.colors_count && i < Fx::COLOR_COUNT; i++)
    {
        cfg->palette.colors[i] = unpackColor(m_msg.colors[i]);
    }
    strlcpy(cfg->fx_hazard_on, m_msg.hazard_on, sizeof(cfg->fx_hazard_on));
    strlcpy(cfg->fx_hazard_off, m_msg.hazard_off, sizeof(cfg->fx_hazard_off));
    cfg->blink_exit_x10 = static_cast<uint8_t>(m_msg.blink_exit_x10);
    cfg->brake_holdoff_s = static_cast<uint16_t>(m_msg.brake_holdoff_s);
    strlcpy(cfg->sta_ssid, m_msg.sta.ssid, sizeof(cfg->sta_ssid));
    strlcpy(cfg->sta_pass, m_msg.sta.pass, sizeof(cfg->sta_pass));
    cfg->sta_active = m_msg.sta.active;
    return true;
}

} // namespace ConfigStore

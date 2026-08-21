/* REST API — protobuf bodies (application/x-protobuf), see
 * docs/ws_protocol.proto. All protocol work happens on the httpd task;
 * compiled objects are handed to the render task through RenderCore. */
#include "net_internal.h"
#include "net_services.h"

#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "config_store.h"
#include "render_core.h"
#include "factory_effects.h"
#include "input_conditioner.h"
#include "status_led.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "ws_protocol.pb.h"

namespace NetServices
{

namespace
{

const char *const TAG = "http_api";

constexpr const char *CONTENT_TYPE_PB = "application/x-protobuf";
constexpr size_t MAX_BODY = 2048;
constexpr size_t HTTPD_STACK_BYTES = 8192;
constexpr uint16_t HTTPD_MAX_URI_HANDLERS = 12;
constexpr const char *AP_IP_STR = "192.168.4.1";
constexpr const char *FW_VERSION_FALLBACK = "unknown";
/* PUT sta.pass: omitted keeps the stored password, this sentinel clears it. */
constexpr const char *STA_PASS_CLEAR_SENTINEL = "-";

/* Field numbers, wire format and message limits all come from
 * proto/ws_protocol.proto through nanopb — nothing to keep in sync here. */
template <typename T, size_t N>
constexpr size_t ARRAY_LEN(T (&)[N])
{
    return N;
}

constexpr size_t CONFIG_BUF_BYTES = 1024;
constexpr size_t EFFECTS_BUF_BYTES = 768;
constexpr size_t SYSINFO_BUF_BYTES = 1024;

static httpd_handle_t m_server;
static SysConfig *m_cfg;      /* application's live config */
static esp_timer_handle_t m_sta_apply_timer;
static const PinDef *m_pins;
static int m_n_pins;

/* Applying a STA change can drop the very connection carrying the request:
 * defer it so the HTTP response gets out first. */
constexpr uint64_t STA_APPLY_DELAY_US = 300 * 1000;

void staApplyCb(void *arg)
{
    (void)arg;
    wifiReconfigureSta(m_cfg->sta_ssid, m_cfg->sta_pass, m_cfg->sta_active);
}

/* ---------- helpers ---------- */

esp_err_t sendPb(httpd_req_t *req, const uint8_t *buf, const size_t len)
{
    httpd_resp_set_type(req, CONTENT_TYPE_PB);
    return httpd_resp_send(req, reinterpret_cast<const char *>(buf), len);
}

esp_err_t sendError(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sendOk(httpd_req_t *req)
{
    httpd_resp_set_type(req, CONTENT_TYPE_PB);
    return httpd_resp_send(req, "", 0);
}

/* Read the full request body into buf. Returns received length or 0. */
size_t readBody(httpd_req_t *req, uint8_t *buf, const size_t cap)
{
    if (0 == req->content_len || req->content_len > cap)
    {
        return 0;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        const int r = httpd_req_recv(req, reinterpret_cast<char *>(buf) + got,
                                     req->content_len - got);
        if (r <= 0)
        {
            return 0;
        }
        got += r;
    }
    return got;
}

uint32_t packColor(const Fx::RgbaColor c)
{
    return (static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16) |
           (static_cast<uint32_t>(c.b) << 8) | c.a;
}

Fx::RgbaColor unpackColor(const uint32_t v)
{
    return Fx::RgbaColor{ static_cast<uint8_t>(v >> 24),
                          static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v) };
}

bool effectIdKnown(const char *id)
{
    return '\0' == id[0] || Fx::factoryExists(id);
}

/* ---------- Config <-> protobuf (nanopb) ---------- */

void stripToProto(const StripConfig &sc, motolights_Strip *out)
{
    out->led_count = sc.led_count;
    out->brightness = sc.brightness;
    out->led_model = static_cast<uint32_t>(sc.led_model);
    out->color_order = static_cast<uint32_t>(sc.color_order);
    out->reversed = sc.reversed;
    out->swap_sides = sc.swap_sides;

    out->has_zones = true;
    out->zones.left_end = sc.zone_left_end;
    out->zones.center_end = sc.zone_center_end;

    out->has_assign = true;
    strlcpy(out->assign.idle, sc.fx_idle, sizeof(out->assign.idle));
    strlcpy(out->assign.brake, sc.fx_brake, sizeof(out->assign.brake));
    out->assign.brake_zone = static_cast<uint32_t>(sc.brake_zone);
    strlcpy(out->assign.turn_on, sc.fx_turn_on, sizeof(out->assign.turn_on));
    strlcpy(out->assign.turn_off, sc.fx_turn_off, sizeof(out->assign.turn_off));
    strlcpy(out->assign.aux, sc.fx_aux, sizeof(out->assign.aux));
    out->assign.aux_zone = static_cast<uint32_t>(sc.aux_zone);
}

void stripFromProto(const motolights_Strip &in, StripConfig *sc)
{
    *sc = StripConfig{};        /* a PUT carries the whole strip */
    sc->led_count = static_cast<uint16_t>(in.led_count);
    sc->brightness = static_cast<uint8_t>(in.brightness);
    sc->led_model = static_cast<LedModel>(in.led_model);
    sc->color_order = static_cast<ColorOrder>(in.color_order);
    sc->reversed = in.reversed;
    sc->swap_sides = in.swap_sides;
    sc->zone_left_end = static_cast<uint16_t>(in.zones.left_end);
    sc->zone_center_end = static_cast<uint16_t>(in.zones.center_end);
    strlcpy(sc->fx_idle, in.assign.idle, sizeof(sc->fx_idle));
    strlcpy(sc->fx_brake, in.assign.brake, sizeof(sc->fx_brake));
    sc->brake_zone = static_cast<ZoneId>(in.assign.brake_zone);
    strlcpy(sc->fx_turn_on, in.assign.turn_on, sizeof(sc->fx_turn_on));
    strlcpy(sc->fx_turn_off, in.assign.turn_off, sizeof(sc->fx_turn_off));
    strlcpy(sc->fx_aux, in.assign.aux, sizeof(sc->fx_aux));
    sc->aux_zone = static_cast<ZoneId>(in.assign.aux_zone);
}

size_t encodeConfig(const SysConfig &cfg, uint8_t *out, const size_t cap)
{
    motolights_Config msg = motolights_Config_init_zero;

    msg.strips_count = STRIP_COUNT;
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        stripToProto(cfg.strips[i], &msg.strips[i]);
    }

    msg.colors_count = Fx::COLOR_COUNT;
    for (int i = 0; i < Fx::COLOR_COUNT; i++)
    {
        msg.colors[i] = packColor(cfg.palette.colors[i]);
    }

    msg.blink_exit_x10 = cfg.blink_exit_x10;
    msg.brake_holdoff_s = cfg.brake_holdoff_s;

    msg.has_sta = true;
    strlcpy(msg.sta.ssid, cfg.sta_ssid, sizeof(msg.sta.ssid));
    /* password is write-only: never echoed back, only its presence */
    msg.sta.active = cfg.sta_active;
    msg.sta.pass_set = '\0' != cfg.sta_pass[0];

    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, motolights_Config_fields, &msg))
    {
        ESP_LOGE(TAG, "config encode failed: %s", PB_GET_ERROR(&stream));
        return 0;
    }
    return stream.bytes_written;
}

/* The message is authoritative: a PUT replaces the configuration. Absent
 * fields therefore mean "unset" (proto3 omits empty strings, which is how
 * the page clears an effect assignment), not "keep the old value". */
bool decodeConfig(const uint8_t *data, const size_t len, SysConfig *cfg)
{
    motolights_Config msg = motolights_Config_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&stream, motolights_Config_fields, &msg))
    {
        ESP_LOGW(TAG, "config decode failed: %s", PB_GET_ERROR(&stream));
        return false;
    }

    *cfg = SysConfig{};
    cfg->version = CFG_VERSION;

    for (pb_size_t i = 0; i < msg.strips_count && i < STRIP_COUNT; i++)
    {
        stripFromProto(msg.strips[i], &cfg->strips[i]);
    }
    for (pb_size_t i = 0; i < msg.colors_count && i < Fx::COLOR_COUNT; i++)
    {
        cfg->palette.colors[i] = unpackColor(msg.colors[i]);
    }
    cfg->blink_exit_x10 = static_cast<uint8_t>(msg.blink_exit_x10);
    cfg->brake_holdoff_s = static_cast<uint16_t>(msg.brake_holdoff_s);
    strlcpy(cfg->sta_ssid, msg.sta.ssid, sizeof(cfg->sta_ssid));
    strlcpy(cfg->sta_pass, msg.sta.pass, sizeof(cfg->sta_pass));
    cfg->sta_active = msg.sta.active;
    return true;
}

esp_err_t hConfigGet(httpd_req_t *req)
{
    uint8_t out[1024];
    const size_t len = encodeConfig(*m_cfg, out, sizeof(out));
    if (0 == len)
    {
        return httpd_resp_send_500(req);
    }
    return sendPb(req, out, len);
}

esp_err_t hConfigPut(httpd_req_t *req)
{
    uint8_t body[MAX_BODY];
    const size_t len = readBody(req, body, sizeof(body));
    if (0 == len)
    {
        return sendError(req, "400 Bad Request", "missing/oversized body");
    }

    SysConfig tmp;
    if (!decodeConfig(body, len, &tmp))
    {
        return sendError(req, "400 Bad Request", "invalid protobuf");
    }
    /* Password is write-only: an omitted field keeps the stored one, while
     * the explicit sentinel clears it (open network). */
    if ('\0' == tmp.sta_pass[0])
    {
        std::strcpy(tmp.sta_pass, m_cfg->sta_pass);
    }
    else if (0 == std::strcmp(tmp.sta_pass, STA_PASS_CLEAR_SENTINEL))
    {
        tmp.sta_pass[0] = '\0';
    }
    if (!ConfigStore::validate(&tmp))
    {
        return sendError(req, "400 Bad Request",
                         "invalid config (led_count 1-300, zones ordered)");
    }
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const StripConfig &sc = tmp.strips[i];
        const char *const refs[] = { sc.fx_idle, sc.fx_aux, sc.fx_brake,
                                     sc.fx_turn_on, sc.fx_turn_off };
        for (size_t k = 0; k < sizeof(refs) / sizeof(refs[0]); k++)
        {
            if (!effectIdKnown(refs[k]))
            {
                return sendError(req, "400 Bad Request", "unknown effect id");
            }
        }
    }

    const bool sta_changed =
        0 != std::strcmp(tmp.sta_ssid, m_cfg->sta_ssid) ||
        0 != std::strcmp(tmp.sta_pass, m_cfg->sta_pass) ||
        tmp.sta_active != m_cfg->sta_active;

    *m_cfg = tmp;
    if (ESP_OK == ConfigStore::save(m_cfg))
    {
        /* Stored config is valid again: drop any boot-time fallback
         * indication. Serving this request means the config WiFi is up. */
        StatusLed::set(StatusLed::State::RunningWifi);
    }
    else
    {
        ESP_LOGW(TAG, "config NVS save failed (still applied live)");
    }
    RenderCore::applyConfig(*m_cfg);
    InputConditioner::setBrakeHoldoff(
        static_cast<uint32_t>(m_cfg->brake_holdoff_s) * 1000);
    InputConditioner::setExitFactor(m_cfg->blink_exit_x10);
    if (sta_changed && nullptr != m_sta_apply_timer)
    {
        esp_timer_stop(m_sta_apply_timer);
        esp_timer_start_once(m_sta_apply_timer, STA_APPLY_DELAY_US);
    }
    return sendOk(req);
}

esp_err_t hEffectsGet(httpd_req_t *req)
{
    static uint8_t out[EFFECTS_BUF_BYTES];
    motolights_EffectsList msg = motolights_EffectsList_init_zero;

    const int count = Fx::factoryCount();
    for (int i = 0; i < count && msg.effects_count < ARRAY_LEN(msg.effects); i++)
    {
        const Fx::FactoryEntry *const fe = Fx::factoryGet(i);
        bool assigned = false;
        for (int k = 0; k < STRIP_COUNT && !assigned; k++)
        {
            const StripConfig &sc = m_cfg->strips[k];
            assigned = 0 == std::strcmp(sc.fx_idle, fe->id) ||
                       0 == std::strcmp(sc.fx_aux, fe->id) ||
                       0 == std::strcmp(sc.fx_brake, fe->id) ||
                       0 == std::strcmp(sc.fx_turn_on, fe->id) ||
                       0 == std::strcmp(sc.fx_turn_off, fe->id);
        }
        motolights_EffectInfo &e = msg.effects[msg.effects_count++];
        strlcpy(e.id, fe->id, sizeof(e.id));
        strlcpy(e.name, fe->name, sizeof(e.name));
        e.assigned = assigned;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, sizeof(out));
    if (!pb_encode(&stream, motolights_EffectsList_fields, &msg))
    {
        return httpd_resp_send_500(req);
    }
    return sendPb(req, out, stream.bytes_written);
}

esp_err_t hSysinfoGet(httpd_req_t *req)
{
    const esp_app_desc_t *const app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    static uint8_t out[SYSINFO_BUF_BYTES];
    motolights_SysInfo msg = motolights_SysInfo_init_zero;

    snprintf(msg.chip, sizeof(msg.chip), "ESP32 rev %u.%u (%d cores)",
             chip.revision / 100, chip.revision % 100, chip.cores);
    strlcpy(msg.fw_version,
            ('\0' != app->version[0]) ? app->version : FW_VERSION_FALLBACK,
            sizeof(msg.fw_version));
    snprintf(msg.compile_time, sizeof(msg.compile_time), "%s %s",
             app->date, app->time);
    for (int i = 0; i < 32; i++)
    {
        snprintf(&msg.sha256[i * 2], 3, "%02X", app->app_elf_sha256[i]);
    }
    strlcpy(msg.idf, app->idf_ver, sizeof(msg.idf));

    const struct
    {
        char *dst;
        size_t cap;
        esp_mac_type_t type;
    } macs[] = {
        { msg.mac_sta, sizeof(msg.mac_sta), ESP_MAC_WIFI_STA },
        { msg.mac_ap, sizeof(msg.mac_ap), ESP_MAC_WIFI_SOFTAP },
        { msg.mac_bt, sizeof(msg.mac_bt), ESP_MAC_BT },
    };
    for (size_t i = 0; i < ARRAY_LEN(macs); i++)
    {
        uint8_t mac[6];
        if (ESP_OK == esp_read_mac(mac, macs[i].type))
        {
            snprintf(macs[i].dst, macs[i].cap, "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }

    msg.heap_free = esp_get_free_heap_size();
    msg.heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    strlcpy(msg.sta_ip, wifiStaIp(), sizeof(msg.sta_ip));
    strlcpy(msg.ap_ip, AP_IP_STR, sizeof(msg.ap_ip));
    msg.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

    for (int i = 0; i < m_n_pins && msg.pins_count < ARRAY_LEN(msg.pins); i++)
    {
        motolights_PinInfo &p = msg.pins[msg.pins_count++];
        strlcpy(p.name, m_pins[i].name, sizeof(p.name));
        p.gpio = static_cast<uint32_t>(m_pins[i].gpio);
        strlcpy(p.desc, m_pins[i].desc, sizeof(p.desc));
    }

    pb_ostream_t stream = pb_ostream_from_buffer(out, sizeof(out));
    if (!pb_encode(&stream, motolights_SysInfo_fields, &msg))
    {
        ESP_LOGE(TAG, "sysinfo encode failed: %s", PB_GET_ERROR(&stream));
        return httpd_resp_send_500(req);
    }
    return sendPb(req, out, stream.bytes_written);
}

esp_err_t hCommandPost(httpd_req_t *req)
{
    uint8_t body[128];
    const size_t len = readBody(req, body, sizeof(body));
    if (0 == len)
    {
        return sendError(req, "400 Bad Request", "missing body");
    }

    motolights_Command cmd = motolights_Command_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(body, len);
    if (!pb_decode(&stream, motolights_Command_fields, &cmd))
    {
        return sendError(req, "400 Bad Request", "invalid protobuf");
    }

    switch (cmd.which_cmd)
    {
    case motolights_Command_test_tag:
    {
        const uint32_t ev = cmd.cmd.test.event;
        if (ev >= static_cast<uint32_t>(CondEvent::Count))
        {
            return sendError(req, "400 Bad Request", "unknown event");
        }
        RenderCore::forceEvent(static_cast<CondEvent>(ev), cmd.cmd.test.active);
        break;
    }
    case motolights_Command_override_tag:
        RenderCore::setOverride(cmd.cmd.override);
        break;
    case motolights_Command_restore_defaults_tag:
        ConfigStore::defaults(m_cfg);
        ConfigStore::save(m_cfg);
        RenderCore::applyConfig(*m_cfg);
        StatusLed::set(StatusLed::State::RunningWifi);
        break;
    default:
        return sendError(req, "400 Bad Request", "empty command");
    }
    return sendOk(req);
}

struct Route
{
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
};

const Route ROUTES[] = {
    { "/api/config", HTTP_GET, hConfigGet },
    { "/api/config", HTTP_PUT, hConfigPut },
    { "/api/effects", HTTP_GET, hEffectsGet },
    { "/api/sysinfo", HTTP_GET, hSysinfoGet },
    { "/api/command", HTTP_POST, hCommandPost },
};

} // namespace

void setPinout(const PinDef *pins, const int count)
{
    m_pins = pins;
    m_n_pins = count;
}

esp_err_t httpStart(SysConfig *live_cfg)
{
    m_cfg = live_cfg;

    if (nullptr == m_sta_apply_timer)
    {
        esp_timer_create_args_t targs = {};
        targs.callback = staApplyCb;
        targs.name = "sta_apply";
        esp_timer_create(&targs, &m_sta_apply_timer);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    cfg.stack_size = HTTPD_STACK_BYTES;
    cfg.max_uri_handlers = HTTPD_MAX_URI_HANDLERS;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&m_server, &cfg);
    if (ESP_OK != err)
    {
        return err;
    }

    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++)
    {
        httpd_uri_t u = {};
        u.uri = ROUTES[i].uri;
        u.method = ROUTES[i].method;
        u.handler = ROUTES[i].handler;
        err = httpd_register_uri_handler(m_server, &u);
        if (ESP_OK != err)
        {
            return err;
        }
    }
    err = staticFilesRegister(m_server);
    if (ESP_OK != err)
    {
        return err;
    }
    return wsStreamStart(m_server);
}

esp_err_t httpStop()
{
    if (nullptr == m_server)
    {
        return ESP_OK;
    }
    wsStreamStop();
    const esp_err_t err = httpd_stop(m_server);
    m_server = nullptr;
    return err;
}

} // namespace NetServices

/* REST API — protobuf bodies (application/x-protobuf), see
 * docs/ws_protocol.proto. All protocol work happens on the httpd task;
 * compiled objects are handed to the render task through RenderCore. */
#include "net_internal.h"
#include "tasks.hpp"
#include "crash_log.h"
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
#include <unistd.h>

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

const char* const TAG = "http_api";

constexpr const char* CONTENT_TYPE_PB = "application/x-protobuf";
/* Sized from the generated worst-case message, so a .proto change resizes
 * them automatically. Handlers (and httpd_queue_work callbacks) all run on
 * the single httpd task, one at a time: these buffers are never re-entered,
 * and a full section config would not fit on an 8 KB stack. */
constexpr size_t MAX_BODY = motolights_Config_size;
constexpr uint16_t HTTPD_MAX_URI_HANDLERS = 12;
constexpr const char* AP_IP_STR = "192.168.4.1";
constexpr const char* FW_VERSION_FALLBACK = "unknown";
/* PUT sta.pass: omitted keeps the stored password, this sentinel clears it. */
constexpr const char* STA_PASS_CLEAR_SENTINEL = "-";

/* Field numbers, wire format and message limits all come from
 * proto/ws_protocol.proto through nanopb — nothing to keep in sync here. */
template <typename T, size_t N>
constexpr size_t ARRAY_LEN(T (&)[N])
{
    return N;
}

constexpr size_t CONFIG_BUF_BYTES = motolights_Config_size;
constexpr size_t EFFECTS_BUF_BYTES = 768;
constexpr int HTTPD_SEND_TIMEOUT_S = 1;
constexpr size_t SYSINFO_BUF_BYTES = motolights_SysInfo_size;

static httpd_handle_t m_server;
static SysConfig* m_cfg;      /* application's live config */
static esp_timer_handle_t m_sta_apply_timer;
static const PinDef* m_pins;
static uint8_t m_config_buf[CONFIG_BUF_BYTES];   /* GET encode */
static uint8_t m_body_buf[MAX_BODY];             /* PUT body */
static SysConfig m_pending_cfg;                  /* PUT staging */

static_assert(CFG_MAX_SECTIONS ==
                  sizeof(motolights_Strip::sections) / sizeof(motolights_Section),
              "proto Strip.sections max_count must match CFG_MAX_SECTIONS");
static int m_n_pins;

/* Applying a STA change can drop the very connection carrying the request:
 * defer it so the HTTP response gets out first. */
constexpr uint64_t STA_APPLY_DELAY_US = 300 * 1000;

void staApplyCb(void* arg)
{
    (void)arg;
    wifiReconfigureSta(m_cfg->sta_ssid, m_cfg->sta_pass, m_cfg->sta_active);
}

/* ---------- helpers ---------- */

esp_err_t sendPb(httpd_req_t* req, const uint8_t* buf, const size_t len)
{
    httpd_resp_set_type(req, CONTENT_TYPE_PB);
    return httpd_resp_send(req, reinterpret_cast<const char*>(buf), len);
}

esp_err_t sendError(httpd_req_t* req, const char* status, const char* msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sendOk(httpd_req_t* req)
{
    httpd_resp_set_type(req, CONTENT_TYPE_PB);
    return httpd_resp_send(req, "", 0);
}

/* Read the full request body into buf. Returns received length or 0. */
} // namespace

bool bodyTypeIs(httpd_req_t* const req, const char* const type)
{
    char got[48];
    if (ESP_OK != httpd_req_get_hdr_value_str(req, "Content-Type", got, sizeof(got)))
    {
        return false;
    }
    return 0 == std::strcmp(got, type);
}

namespace
{

size_t readBody(httpd_req_t* req, uint8_t* buf, const size_t cap)
{
    if (0 == req->content_len || req->content_len > cap)
    {
        return 0;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(buf) + got,
                                     req->content_len - got);
        if (r <= 0)
        {
            return 0;
        }
        got += r;
    }
    return got;
}

/* An empty id means "this event paints nothing", which is always legal. */
bool effectIdKnown(const char* const id)
{
    return '\0' == id[0] || Fx::factoryExists(id);
}

/* Config <-> protobuf lives in config_store: the same encoding goes on the
 * wire and into flash, so the two can never drift apart. */

esp_err_t hConfigGet(httpd_req_t* req)
{
    const size_t len = ConfigStore::encode(*m_cfg, m_config_buf,
                                          sizeof(m_config_buf),
                                          ConfigStore::Secrets::Omit);
    if (0 == len)
    {
        return httpd_resp_send_500(req);
    }
    return sendPb(req, m_config_buf, len);
}

/* Everything a new *m_cfg needs to become real: the render set, the input
 * conditioner's parameters, the STA link, and the flash. One path for a PUT
 * and for restore-defaults, so neither can forget a step — and an honest
 * answer: a config that did not take, or did not persist, is not a 200. */
esp_err_t applyAndPersist(httpd_req_t* const req, const bool sta_changed)
{
    const esp_err_t applied = RenderCore::applyConfig(*m_cfg);
    if (ESP_OK != applied)
    {
        ESP_LOGE(TAG, "applyConfig: %s — strip keeps the previous set",
                 esp_err_to_name(applied));
        return sendError(req, "500 Internal Server Error",
                         "could not apply the configuration (out of memory?)");
    }
    InputConditioner::setBrakeHoldoff(
        static_cast<uint32_t>(m_cfg->brake_holdoff_s) * 1000);
    InputConditioner::setExitFactor(m_cfg->blink_exit_x10);
    if (sta_changed && nullptr != m_sta_apply_timer)
    {
        esp_timer_stop(m_sta_apply_timer);
        esp_timer_start_once(m_sta_apply_timer, STA_APPLY_DELAY_US);
    }

    const esp_err_t saved = ConfigStore::save(m_cfg);
    if (ESP_OK != saved)
    {
        ESP_LOGE(TAG, "config NVS save: %s (applied live only)",
                 esp_err_to_name(saved));
        return sendError(req, "500 Internal Server Error",
                         "applied, but could not be saved to flash");
    }
    /* Stored config is valid again: drop any boot-time fallback indication.
     * Serving this request means the config WiFi is up. */
    StatusLed::set(StatusLed::State::RunningWifi);
    return sendOk(req);
}

esp_err_t hConfigPut(httpd_req_t* req)
{
    if (!bodyTypeIs(req, BODY_TYPE_PROTOBUF))
    {
        return sendError(req, "415 Unsupported Media Type", "expected application/x-protobuf");
    }
    const size_t len = readBody(req, m_body_buf, sizeof(m_body_buf));
    if (0 == len)
    {
        return sendError(req, "400 Bad Request", "missing/oversized body");
    }

    SysConfig& tmp = m_pending_cfg;
    if (!ConfigStore::decode(m_body_buf, len, &tmp))
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
    if ('\0' == tmp.ap_pass[0])
    {
        std::strcpy(tmp.ap_pass, m_cfg->ap_pass);
    }
    /* Said before the generic check so the answer names the actual problem:
     * this is the one setting that can lock the page away. */
    if ('\0' != tmp.ap_ssid[0] &&
        std::strlen(tmp.ap_pass) < CFG_AP_PASS_MIN)
    {
        return sendError(req, "400 Bad Request",
                         "the access point password needs at least 8 characters");
    }
    if (!ConfigStore::validate(&tmp))
    {
        return sendError(req, "400 Bad Request",
                         "invalid config: 8 sections and 300 LEDs per strip at most, "
                         "passwords 8-63 characters, a turn source needs a turn effect");
    }
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const StripConfig& sc = tmp.strips[i];
        for (int k = 0; k < sc.n_sections; k++)
        {
            const SectionConfig& sec = sc.sections[k];
            const char* const refs[] = { sec.fx_idle, sec.fx_aux, sec.fx_brake,
                                         sec.fx_turn_on, sec.fx_turn_off };
            for (size_t r = 0; r < sizeof(refs) / sizeof(refs[0]); r++)
            {
                if (!effectIdKnown(refs[r]))
                {
                    return sendError(req, "400 Bad Request",
                                     "unknown effect id");
                }
            }
        }
    }
    if (!effectIdKnown(tmp.fx_hazard_on) || !effectIdKnown(tmp.fx_hazard_off))
    {
        return sendError(req, "400 Bad Request", "unknown effect id");
    }

    if (otaRebootPending())
    {
        return sendError(req, "503 Service Unavailable", "rebooting into new firmware");
    }
    const bool sta_changed =
        0 != std::strcmp(tmp.sta_ssid, m_cfg->sta_ssid) ||
        0 != std::strcmp(tmp.sta_pass, m_cfg->sta_pass) ||
        tmp.sta_active != m_cfg->sta_active;

    *m_cfg = tmp;
    return applyAndPersist(req, sta_changed);
}

esp_err_t hEffectsGet(httpd_req_t* req)
{
    static uint8_t out[EFFECTS_BUF_BYTES];
    motolights_EffectsList msg = motolights_EffectsList_init_zero;

    const int count = Fx::factoryCount();
    for (int i = 0; i < count && msg.effects_count < ARRAY_LEN(msg.effects); i++)
    {
        const Fx::FactoryEntry* const fe = Fx::factoryGet(i);
        bool assigned = false;
        for (int k = 0; k < STRIP_COUNT && !assigned; k++)
        {
            const StripConfig& sc = m_cfg->strips[k];
            for (int j = 0; j < sc.n_sections && !assigned; j++)
            {
                const SectionConfig& sec = sc.sections[j];
                assigned = 0 == std::strcmp(sec.fx_idle, fe->id) ||
                           0 == std::strcmp(sec.fx_aux, fe->id) ||
                           0 == std::strcmp(sec.fx_brake, fe->id) ||
                           0 == std::strcmp(sec.fx_turn_on, fe->id) ||
                           0 == std::strcmp(sec.fx_turn_off, fe->id);
            }
        }
        assigned = assigned ||
                   0 == std::strcmp(m_cfg->fx_hazard_on, fe->id) ||
                   0 == std::strcmp(m_cfg->fx_hazard_off, fe->id);
        motolights_EffectInfo& e = msg.effects[msg.effects_count++];
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

esp_err_t hSysinfoGet(httpd_req_t* req)
{
    const esp_app_desc_t* const app = esp_app_get_description();
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
        char* dst;
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
    strlcpy(msg.crash_log, CrashLog::summary(), sizeof(msg.crash_log));

    for (int i = 0; i < m_n_pins && msg.pins_count < ARRAY_LEN(msg.pins); i++)
    {
        motolights_PinInfo& p = msg.pins[msg.pins_count++];
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

esp_err_t hCommandPost(httpd_req_t* req)
{
    if (!bodyTypeIs(req, BODY_TYPE_PROTOBUF))
    {
        return sendError(req, "415 Unsupported Media Type", "expected application/x-protobuf");
    }
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
    {
        if (!cmd.cmd.restore_defaults)
        {
            break;              /* presence alone is not a request */
        }
        if (otaRebootPending())
        {
            return sendError(req, "503 Service Unavailable",
                             "rebooting into new firmware");
        }
        const bool sta_was_set = '\0' != m_cfg->sta_ssid[0] || m_cfg->sta_active;
        ConfigStore::defaults(m_cfg);
        return applyAndPersist(req, sta_was_set);
    }
    default:
        return sendError(req, "400 Bad Request", "empty command");
    }
    return sendOk(req);
}

/* httpd hands every closing socket here (on its own task). Dropping the fd
 * from the WebSocket client list before it is recycled is what keeps a push
 * from landing in an unrelated HTTP response. Closing the fd is our job once
 * this hook is installed. */
void onSockClose(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    wsStreamOnSockClose(sockfd);
    close(sockfd);
}

struct Route
{
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
};

const Route ROUTES[] = {
    { "/api/config", HTTP_GET, hConfigGet },
    { "/api/config", HTTP_PUT, hConfigPut },
    { "/api/effects", HTTP_GET, hEffectsGet },
    { "/api/sysinfo", HTTP_GET, hSysinfoGet },
    { "/api/command", HTTP_POST, hCommandPost },
    { "/api/ota", HTTP_POST, otaPost },
};

} // namespace

void setPinout(const PinDef* pins, const int count)
{
    m_pins = pins;
    m_n_pins = count;
}

esp_err_t httpStart(SysConfig* live_cfg)
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
    cfg.core_id = Tasks::HTTPD.core;
    cfg.stack_size = Tasks::HTTPD.stack_bytes;
    cfg.task_priority = Tasks::HTTPD.priority;
    cfg.max_uri_handlers = HTTPD_MAX_URI_HANDLERS;
    cfg.lru_purge_enable = true;
    /* WebSocket pushes go through the same blocking send(): a client that has
     * stopped reading (phone screen off) must not hold the single httpd task
     * for the default 5 s per frame. */
    cfg.send_wait_timeout = HTTPD_SEND_TIMEOUT_S;
    cfg.close_fn = onSockClose;

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

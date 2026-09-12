#include "net_services.h"
#include "net_internal.h"

#include <cstring>
#include <cstdio>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace NetServices
{

namespace
{

const char* const TAG = "wifi";

/* SoftAP identity — page always reachable at http://192.168.4.1 */
/* Used when the configuration names no access point of its own, so a module
 * that was never configured (or was just reset) always has a way in. The
 */
constexpr const char* AP_SSID_DEFAULT = "MotoLights";
constexpr const char* AP_PASS_DEFAULT = "motolights";

static const char* m_ap_ssid = AP_SSID_DEFAULT;
constexpr uint8_t AP_CHANNEL = 6;    /* follows the STA channel when joined */
constexpr uint8_t AP_MAX_STA = 4;

/* Rejoining the home network backs off instead of retrying flat out. The AP
 * and the STA share one radio, and every join attempt scans every channel —
 * time the SoftAP is not on the air. Out of range, an immediate retry (which
 * is what this did) leaves the radio scanning back to back and the page
 * unreachable at exactly the moment it is the only way in. The delay doubles
 * from a couple of seconds to half a minute, so a brief drop is recovered
 * quickly while a network that is simply gone costs a scan every 30 s. */
constexpr uint32_t STA_RETRY_FIRST_MS = 2000;
constexpr uint32_t STA_RETRY_MAX_MS = 30000;

static esp_timer_handle_t m_sta_retry_timer;
static uint32_t m_sta_retry_ms = STA_RETRY_FIRST_MS;

static bool m_netif_ready;
static esp_netif_t* m_ap_netif;
static esp_netif_t* m_sta_netif;
static bool m_wifi_running;
static bool m_sta_enabled;
static bool m_sta_connected;
static char m_sta_ip[16];

void staRetryCb(void*)
{
    /* Re-checked here, not only when the timer was armed: the radio may have
     * been stopped or the STA switched off while this was pending. */
    if (m_wifi_running && m_sta_enabled && !m_sta_connected)
    {
        esp_wifi_connect();
    }
}

void cancelStaRetry()
{
    if (nullptr != m_sta_retry_timer)
    {
        esp_timer_stop(m_sta_retry_timer);
    }
    m_sta_retry_ms = STA_RETRY_FIRST_MS;
}

void scheduleStaRetry()
{
    if (nullptr == m_sta_retry_timer)
    {
        const esp_timer_create_args_t args = { staRetryCb, nullptr,
                                               ESP_TIMER_TASK, "sta_retry",
                                               false };
        if (ESP_OK != esp_timer_create(&args, &m_sta_retry_timer))
        {
            /* No timer, no rejoin: the SoftAP stays usable, which is the
             * half that matters. Say so once rather than fall back to the
             * hammering this exists to prevent. */
            ESP_LOGE(TAG, "no retry timer: STA will not rejoin by itself");
            return;
        }
    }
    esp_timer_stop(m_sta_retry_timer);
    ESP_LOGW(TAG, "STA down, retrying in %u s",
             static_cast<unsigned>(m_sta_retry_ms / 1000));
    esp_timer_start_once(m_sta_retry_timer,
                         static_cast<uint64_t>(m_sta_retry_ms) * 1000);
    m_sta_retry_ms = (m_sta_retry_ms * 2 > STA_RETRY_MAX_MS)
                         ? STA_RETRY_MAX_MS
                         : m_sta_retry_ms * 2;
}

void wifiEventCb(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (WIFI_EVENT == base)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            m_sta_connected = false;
            m_sta_ip[0] = '\0';
            if (m_wifi_running && m_sta_enabled)
            {
                scheduleStaRetry();
            }
            break;
        default:
            break;
        }
    }
    else if (IP_EVENT == base && IP_EVENT_STA_GOT_IP == id)
    {
        const ip_event_got_ip_t* const ev =
            static_cast<const ip_event_got_ip_t*>(data);
        snprintf(m_sta_ip, sizeof(m_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        m_sta_connected = true;
        cancelStaRetry();   /* joined: the next drop starts short again */
        ESP_LOGI(TAG, "STA got IP: %s — config page also at http://%s",
                 m_sta_ip, m_sta_ip);
    }
}

esp_err_t applyStaConfig(const char* ssid, const char* pass)
{
    wifi_config_t sta_cfg = {};
    strlcpy(reinterpret_cast<char*>(sta_cfg.sta.ssid), ssid,
            sizeof(sta_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta_cfg.sta.password),
            (nullptr != pass) ? pass : "", sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

} // namespace

/* A start that dies after esp_wifi_init() must undo it: the next attempt's
 * esp_wifi_init() would otherwise answer ESP_ERR_INVALID_STATE, and every
 * button press from then on would give the orange LED until a power cycle. */
esp_err_t failStart(const esp_err_t err)
{
    esp_wifi_deinit();
    return err;
}

esp_err_t wifiStart(const char* ap_ssid, const char* ap_pass,
                    const char* sta_ssid, const char* sta_pass,
                    const bool sta_active)
{
    esp_err_t err;

    m_sta_enabled = sta_active && nullptr != sta_ssid && '\0' != sta_ssid[0];

    if (!m_netif_ready)
    {
        err = esp_netif_init();
        if (ESP_OK != err)
        {
            return err;
        }
        err = esp_event_loop_create_default();
        if (ESP_OK != err && ESP_ERR_INVALID_STATE != err)
        {
            return err;
        }
        m_ap_netif = esp_netif_create_default_wifi_ap();
        if (nullptr == m_ap_netif)
        {
            return ESP_FAIL;
        }
        m_sta_netif = esp_netif_create_default_wifi_sta();
        if (nullptr == m_sta_netif)
        {
            return ESP_FAIL;
        }
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifiEventCb, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifiEventCb, nullptr));
        m_netif_ready = true;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (ESP_OK != err)
    {
        return err;
    }

    m_ap_ssid = ('\0' != ap_ssid[0]) ? ap_ssid : AP_SSID_DEFAULT;
    const char* const pass = ('\0' != ap_ssid[0]) ? ap_pass : AP_PASS_DEFAULT;

    wifi_config_t ap_cfg = {};
    strlcpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), m_ap_ssid,
            sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = std::strlen(m_ap_ssid);
    strlcpy(reinterpret_cast<char*>(ap_cfg.ap.password), pass,
            sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_STA;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(m_sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (ESP_OK != err)
    {
        return failStart(err);
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (ESP_OK != err)
    {
        return failStart(err);
    }
    if (m_sta_enabled)
    {
        err = applyStaConfig(sta_ssid, sta_pass);
        if (ESP_OK != err)
        {
            return failStart(err);
        }
    }

    err = esp_wifi_start();
    if (ESP_OK != err)
    {
        return failStart(err);
    }

    /* Powered by the bike's battery: latency beats microamps. Modem
     * power-save adds 100-300 ms bursts — unusable for the live stream. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    m_wifi_running = true;
    ESP_LOGI(TAG, "SoftAP '%s' up at http://192.168.4.1%s", m_ap_ssid,
             m_sta_enabled ? ", STA joining home network..." : "");
    return ESP_OK;
}

esp_err_t wifiReconfigureSta(const char* ssid, const char* pass,
                             const bool active)
{
    if (!m_wifi_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const bool enable = active && nullptr != ssid && '\0' != ssid[0];
    m_sta_enabled = enable;
    m_sta_connected = false;
    m_sta_ip[0] = '\0';
    cancelStaRetry();

    if (enable)
    {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ESP_OK != err)
        {
            return err;
        }
        err = applyStaConfig(ssid, pass);
        if (ESP_OK != err)
        {
            return err;
        }
        esp_wifi_disconnect();
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA reconfigured, joining '%s'", ssid);
        return ESP_OK;
    }

    esp_wifi_disconnect();
    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    ESP_LOGI(TAG, "STA disabled, SoftAP only");
    return err;
}

esp_err_t wifiStop()
{
    if (!m_wifi_running)
    {
        return ESP_OK;
    }
    m_wifi_running = false;
    m_sta_connected = false;
    m_sta_ip[0] = '\0';
    cancelStaRetry();
    esp_wifi_stop();
    esp_wifi_deinit();
    return ESP_OK;
}

bool wifiRunning()
{
    return m_wifi_running;
}

int wifiStaCount()
{
    if (!m_wifi_running)
    {
        return 0;
    }
    wifi_sta_list_t list;
    if (ESP_OK != esp_wifi_ap_get_sta_list(&list))
    {
        return 0;
    }
    return list.num;
}

const char* wifiStaIp()
{
    return m_sta_connected ? m_sta_ip : "";
}

/* ---------- public start/stop ---------- */

esp_err_t start(SysConfig* live_cfg)
{
    const esp_err_t err = wifiStart(live_cfg->ap_ssid, live_cfg->ap_pass,
                                    live_cfg->sta_ssid, live_cfg->sta_pass,
                                    live_cfg->sta_active);
    if (ESP_OK != err)
    {
        return err;
    }
    return httpStart(live_cfg);
}

esp_err_t stop()
{
    httpStop();
    return wifiStop();
}

bool running()
{
    return wifiRunning();
}

int staCount()
{
    return wifiStaCount();
}

esp_err_t reconfigureSta(const char* ssid, const char* pass, const bool active)
{
    return wifiReconfigureSta(ssid, pass, active);
}

} // namespace NetServices

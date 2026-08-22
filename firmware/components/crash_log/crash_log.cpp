#include "crash_log.h"

#include <cstdio>
#include <cstring>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"

namespace CrashLog
{

namespace
{

const char* const TAG = "crash_log";

constexpr const char* NVS_NS = "motolight";
constexpr const char* KEY_LOG = "crashlog";
constexpr uint16_t LOG_VERSION = 1;
constexpr size_t SUMMARY_LEN = 64;

/* Ring of reset reasons, oldest at head when it has wrapped. Kept tiny and
 * self-describing: a log that fails to make sense is simply started over,
 * it must never be a reason to stop lighting. */
struct LogBlob
{
    uint16_t version;
    uint16_t total;                       /* unexpected resets ever seen */
    uint8_t  head;                        /* next slot to write */
    uint8_t  used;
    uint8_t  reason[CRASH_LOG_ENTRIES];   /* esp_reset_reason_t values */
};

static LogBlob m_log;
static char m_summary[SUMMARY_LEN];

/* Power loss is not a crash: a brownout or a glitching supply says the bike
 * was switched off, not that the firmware failed. */
bool unexpected(const esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;
    }
}

const char* reasonName(const esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                 return "unknown";
    }
}

void buildSummary()
{
    if (0 == m_log.total)
    {
        std::snprintf(m_summary, sizeof(m_summary), "no unexpected reset");
        return;
    }
    const int last = (0 == m_log.used)
                         ? 0
                         : (m_log.head + CRASH_LOG_ENTRIES - 1) % CRASH_LOG_ENTRIES;
    std::snprintf(m_summary, sizeof(m_summary), "%u unexpected reset%s, last: %s",
                  static_cast<unsigned>(m_log.total),
                  (1 == m_log.total) ? "" : "s",
                  reasonName(static_cast<esp_reset_reason_t>(m_log.reason[last])));
}

esp_err_t openNvs(nvs_handle_t* out)
{
    return nvs_open(NVS_NS, NVS_READWRITE, out);
}

} // namespace

void init()
{
    std::memset(&m_log, 0, sizeof(m_log));
    m_log.version = LOG_VERSION;

    nvs_handle_t nvs;
    if (ESP_OK != openNvs(&nvs))
    {
        buildSummary();
        return;
    }

    LogBlob stored;
    size_t len = sizeof(stored);
    if (ESP_OK == nvs_get_blob(nvs, KEY_LOG, &stored, &len) &&
        sizeof(stored) == len && LOG_VERSION == stored.version &&
        stored.head < CRASH_LOG_ENTRIES && stored.used <= CRASH_LOG_ENTRIES)
    {
        m_log = stored;
    }

    const esp_reset_reason_t reason = esp_reset_reason();
    if (unexpected(reason))
    {
        m_log.reason[m_log.head] = static_cast<uint8_t>(reason);
        m_log.head = (m_log.head + 1) % CRASH_LOG_ENTRIES;
        if (m_log.used < CRASH_LOG_ENTRIES)
        {
            m_log.used++;
        }
        if (m_log.total < UINT16_MAX)
        {
            m_log.total++;
        }
        /* One write per crash, so this costs nothing on a normal boot. */
        if (ESP_OK == nvs_set_blob(nvs, KEY_LOG, &m_log, sizeof(m_log)))
        {
            nvs_commit(nvs);
        }
        ESP_LOGW(TAG, "previous run ended in %s", reasonName(reason));
    }
    nvs_close(nvs);
    buildSummary();
}

const char* summary()
{
    return m_summary;
}

int snapshot(const char* names[], const int max)
{
    const int n = (m_log.used < max) ? m_log.used : max;
    for (int i = 0; i < n; i++)
    {
        /* walk oldest -> newest */
        const int slot = (m_log.head + CRASH_LOG_ENTRIES - m_log.used + i) %
                         CRASH_LOG_ENTRIES;
        names[i] = reasonName(static_cast<esp_reset_reason_t>(m_log.reason[slot]));
    }
    return n;
}

esp_err_t clear()
{
    nvs_handle_t nvs;
    esp_err_t err = openNvs(&nvs);
    if (ESP_OK != err)
    {
        return err;
    }
    std::memset(&m_log, 0, sizeof(m_log));
    m_log.version = LOG_VERSION;
    err = nvs_set_blob(nvs, KEY_LOG, &m_log, sizeof(m_log));
    if (ESP_OK == err)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    buildSummary();
    return err;
}

} // namespace CrashLog

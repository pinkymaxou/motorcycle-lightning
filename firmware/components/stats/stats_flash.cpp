#include "stats.h"
#include "stats_store.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "input_conditioner.h"

namespace Stats
{

using namespace StatsRecord;

namespace
{

const char* TAG = "stats";
constexpr const char* PARTITION_LABEL = "stats";

/* An erase stalls both cores for tens of milliseconds, so it only ever runs
 * when nothing is being signalled and the supply has proved steady. */
constexpr uint32_t QUIET_HOLD_MS = 2000;
constexpr uint32_t ERASE_BOOT_HOLDOFF_MS = 60000;
constexpr uint32_t TICK_MS = TICK_SECONDS * 1000;
constexpr size_t   REQUIRED_BYTES = SECTOR_COUNT * SECTOR_BYTES;
constexpr size_t   SUMMARY_CHARS = 160;

const esp_partition_t* m_partition;
StatsStore::Store      m_store;
Counters               m_live;              /* including what has not reached flash */
Blink::EventCounts     m_seen;              /* last tally read from the conditioner */
const char*            m_why = "";
bool                   m_ready;
uint32_t               m_boot_ms;
uint32_t               m_last_ms;
uint32_t               m_powered_ms;        /* toward the next tick */
uint32_t               m_wifi_ms;
uint32_t               m_quiet_since_ms;    /* 0 when something is signalling */
char                   m_summary[SUMMARY_CHARS];

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

size_t offsetOf(const int sector, const int index)
{
    return static_cast<size_t>(sector) * SECTOR_BYTES +
           static_cast<size_t>(index) * SLOT_BYTES;
}

bool readSlot(void*, const int sector, const int index, uint8_t* out)
{
    return ESP_OK == esp_partition_read(m_partition, offsetOf(sector, index),
                                        out, SLOT_BYTES);
}

bool writeSlot(void*, const int sector, const int index, const uint8_t* in)
{
    return ESP_OK == esp_partition_write(m_partition, offsetOf(sector, index),
                                         in, SLOT_BYTES);
}

bool eraseSector(void*, const int sector)
{
    return ESP_OK == esp_partition_erase_range(m_partition,
                                               offsetOf(sector, 0), SECTOR_BYTES);
}

/* Folds everything the conditioner has counted since the last look into the
 * live totals. The tallies only ever grow, so a delta can neither be negative
 * nor double-counted. */
void takeEvents()
{
    Blink::EventCounts now;
    InputConditioner::eventCounts(&now);
    m_live.brake          += now.brake - m_seen.brake;
    m_live.left_uses      += now.left_uses - m_seen.left_uses;
    m_live.left_flashes   += now.left_flashes - m_seen.left_flashes;
    m_live.right_uses     += now.right_uses - m_seen.right_uses;
    m_live.right_flashes  += now.right_flashes - m_seen.right_flashes;
    m_live.hazard_uses    += now.hazard_uses - m_seen.hazard_uses;
    m_live.hazard_flashes += now.hazard_flashes - m_seen.hazard_flashes;
    m_live.aux            += now.aux - m_seen.aux;
    m_seen = now;
}

void giveUp(const char* why)
{
    m_ready = false;
    m_why = why;
    ESP_LOGW(TAG, "counters are no longer being saved: %s", why);
}

} // namespace

void init()
{
    std::memset(&m_live, 0, sizeof(m_live));
    std::memset(&m_seen, 0, sizeof(m_seen));
    m_boot_ms = nowMs();
    m_last_ms = m_boot_ms;

    m_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_ANY,
                                           PARTITION_LABEL);
    if (nullptr == m_partition)
    {
        /* An update over the air rewrites the app slot and nothing else, so a
         * module updated that way still carries the old partition table. */
        m_why = "no counter partition — update over the cable to add one";
        ESP_LOGW(TAG, "%s", m_why);
        return;
    }
    if (m_partition->size < REQUIRED_BYTES)
    {
        m_why = "counter partition too small";
        ESP_LOGW(TAG, "%s (%u bytes)", m_why,
                 static_cast<unsigned>(m_partition->size));
        m_partition = nullptr;
        return;
    }

    StatsStore::Ops ops = {};
    ops.readSlot = readSlot;
    ops.writeSlot = writeSlot;
    ops.eraseSector = eraseSector;
    ops.ctx = nullptr;
    if (!StatsStore::open(&m_store, ops))
    {
        m_why = "counter partition unreadable";
        ESP_LOGW(TAG, "%s", m_why);
        m_partition = nullptr;
        return;
    }

    m_live = m_store.counters;
    m_live.boots++;
    m_ready = true;
    ESP_LOGI(TAG, "%s", summary());
}

void tick(const bool wifi_on, const bool quiet)
{
    const uint32_t now = nowMs();
    const uint32_t elapsed = now - m_last_ms;
    m_last_ms = now;

    takeEvents();

    m_powered_ms += elapsed;
    if (wifi_on)
    {
        m_wifi_ms += elapsed;
    }
    while (m_wifi_ms >= TICK_MS)
    {
        m_wifi_ms -= TICK_MS;
        m_live.wifi_15s++;
    }

    if (!m_ready)
    {
        /* Still counted, just not written down. */
        while (m_powered_ms >= TICK_MS)
        {
            m_powered_ms -= TICK_MS;
            m_live.powered_15s++;
        }
        return;
    }

    /* The spare is erased ahead of the roll-over that will need it, at the
     * first moment where stalling the strip costs nothing. A supply that has
     * only just come up is the worst moment there is. */
    if (quiet)
    {
        if (0 == m_quiet_since_ms)
        {
            m_quiet_since_ms = now;
        }
    }
    else
    {
        m_quiet_since_ms = 0;
    }
    const bool settled = (0 != m_quiet_since_ms) &&
                         (now - m_quiet_since_ms >= QUIET_HOLD_MS) &&
                         (now - m_boot_ms >= ERASE_BOOT_HOLDOFF_MS);
    if (settled && StatsStore::needsSpare(&m_store))
    {
        ESP_LOGI(TAG, "erasing the spare sector");
        StatsStore::prepareSpare(&m_store);
    }

    bool due = false;
    while (m_powered_ms >= TICK_MS)
    {
        m_powered_ms -= TICK_MS;
        m_live.powered_15s++;
        due = true;
    }
    if (due)
    {
        const uint32_t was = m_store.generation;
        /* A failure here loses nothing: the totals stay in RAM and the next
         * tick tries again, once a quiet moment has freed a sector. */
        StatsStore::append(&m_store, m_live);
        if (m_store.generation != was)
        {
            ESP_LOGI(TAG, "rolled over to sector %d, generation %u",
                     m_store.sector,
                     static_cast<unsigned>(m_store.generation));
        }
        if (!m_store.persisting)
        {
            giveUp("flash stopped accepting writes");
        }
    }
}

void flush()
{
    if (!m_ready)
    {
        return;
    }
    takeEvents();
    StatsStore::append(&m_store, m_live);
}

void eraseAll()
{
    std::memset(&m_live, 0, sizeof(m_live));
    if (nullptr == m_partition)
    {
        return;
    }
    m_ready = false;
    m_why = "erased";
    esp_partition_erase_range(m_partition, 0, REQUIRED_BYTES);
}

void get(Counters* out)
{
    *out = m_live;
}

bool stored(const char** why)
{
    if (nullptr != why)
    {
        *why = m_why;
    }
    return m_ready;
}

bool reset()
{
    std::memset(&m_live, 0, sizeof(m_live));
    m_powered_ms = 0;
    m_wifi_ms = 0;
    InputConditioner::eventCounts(&m_seen);   /* drop what has not been folded in */

    if (!m_ready)
    {
        return false;
    }
    if (!StatsStore::reset(&m_store))
    {
        if (!m_store.persisting)
        {
            giveUp("flash stopped accepting writes");
        }
        return false;
    }
    ESP_LOGI(TAG, "counters cleared");
    return true;
}

const char* summary()
{
    if (!m_ready)
    {
        std::snprintf(m_summary, sizeof(m_summary), "not saved: %s", m_why);
        return m_summary;
    }
    const uint32_t hours = m_live.powered_15s / (3600 / TICK_SECONDS);
    const uint32_t minutes = (m_live.powered_15s % (3600 / TICK_SECONDS)) /
                             (60 / TICK_SECONDS);
    std::snprintf(m_summary, sizeof(m_summary),
                  "%uh%02um powered, %u boots, %u brake, %u left, %u right, "
                  "%u hazard, %u aux",
                  static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                  static_cast<unsigned>(m_live.boots),
                  static_cast<unsigned>(m_live.brake),
                  static_cast<unsigned>(m_live.left_uses),
                  static_cast<unsigned>(m_live.right_uses),
                  static_cast<unsigned>(m_live.hazard_uses),
                  static_cast<unsigned>(m_live.aux));
    return m_summary;
}

} // namespace Stats

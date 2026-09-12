#include "stats.h"
#include "stats_store.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "input_conditioner.h"

namespace Stats
{

using namespace StatsRecord;

namespace
{

static const char* const TAG = "stats";
constexpr const char* PARTITION_LABEL = "stats";

/* An erase stalls both cores for tens of milliseconds, so it only ever runs
 * when nothing is being signalled and the supply has proved steady. */
constexpr uint32_t QUIET_HOLD_MS = 2000;
constexpr uint32_t ERASE_BOOT_HOLDOFF_MS = 60000;
constexpr uint32_t TICK_MS = TICK_SECONDS * 1000;
constexpr size_t   REQUIRED_BYTES = SECTOR_COUNT * SECTOR_BYTES;
constexpr size_t   SUMMARY_CHARS = 200;
/* A full sector that has waited this many ticks for a quiet moment is said so
 * on the page rather than left to count in RAM as if all were well. */
constexpr uint32_t STALL_REPORT_TICKS = 4;
constexpr uint32_t REQUEST_POLL_MS = 20;

constexpr const char* WHY_NO_PARTITION =
    "no counter partition — update over the cable to add one";
constexpr const char* WHY_TOO_SMALL = "counter partition too small";
constexpr const char* WHY_FLASH = "flash stopped accepting writes";
constexpr const char* WHY_STALLED =
    "waiting for the signals to stop, to make room in flash";
constexpr const char* WHY_ERASED = "erased";
constexpr const char* WHY_BUSY =
    "the module is still signalling — try again when it is quiet";

enum class Outcome : uint8_t
{
    Pending,
    Ok,
    Failed
};

/* Owned by the housekeeping task. Nothing on another task touches these. */
static const esp_partition_t* m_partition;
static StatsStore::Store      m_store;
static Counters               m_live;           /* including what has not reached flash */
static Blink::EventCounts     m_seen;           /* last tally read from the conditioner */
static uint32_t               m_boot_ms;
static uint32_t               m_last_ms;
static uint32_t               m_powered_ms;     /* toward the next tick */
static uint32_t               m_wifi_ms;
static uint32_t               m_quiet_since_ms; /* 0 when something is signalling */
static uint32_t               m_stalled_ticks;
static TaskHandle_t           m_owner;

/* Published for other tasks: a double-buffered snapshot the owner flips with
 * one atomic store, and a reason pointer. */
static Counters                       m_snap[2];
static std::atomic<uint8_t>           m_snap_idx;
static std::atomic<const char*>       m_why;    /* nullptr = being saved */
static char                           m_summary[SUMMARY_CHARS];

/* Requests from other tasks, served by tick(). */
static std::atomic<bool>              m_reset_pending;
static std::atomic<Outcome>           m_reset_outcome;
static std::atomic<bool>              m_flush_pending;
static std::atomic<uint32_t>          m_flush_serial;

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool onOwnerTask()
{
    return xTaskGetCurrentTaskHandle() == m_owner;
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

bool saving()
{
    return nullptr != m_partition && m_store.persisting;
}

/* Folds everything the conditioner has counted since the last look into the
 * live totals. The tallies only ever grow, so a delta can neither be negative
 * nor double-counted. Word-wise, so a counter added to EventCounts cannot be
 * forgotten here. */
void takeEvents()
{
    Blink::EventCounts now;
    InputConditioner::eventCounts(&now);
    constexpr size_t WORDS = sizeof(Blink::EventCounts) / sizeof(uint32_t);
    static_assert(sizeof(Blink::EventCounts) == WORDS * sizeof(uint32_t),
                  "EventCounts must be a packed run of 32-bit words");
    uint32_t a[WORDS];
    uint32_t b[WORDS];
    std::memcpy(a, &now, sizeof(a));
    std::memcpy(b, &m_seen, sizeof(b));
    /* brake .. aux sit together in Counters, in EventCounts order. */
    constexpr size_t FIRST = offsetof(Counters, brake) / sizeof(uint32_t);
    static_assert(offsetof(Counters, aux) - offsetof(Counters, brake) ==
                      (WORDS - 1) * sizeof(uint32_t),
                  "the event counters must sit together in Counters, in "
                  "EventCounts order");
    uint32_t live[COUNTER_WORDS];
    std::memcpy(live, &m_live, sizeof(live));
    for (size_t i = 0; i < WORDS; i++)
    {
        live[FIRST + i] += a[i] - b[i];
    }
    std::memcpy(&m_live, live, sizeof(live));
    m_seen = now;
}

void publish()
{
    const uint8_t next = static_cast<uint8_t>(1 - m_snap_idx.load(std::memory_order_relaxed));
    m_snap[next] = m_live;
    m_snap_idx.store(next, std::memory_order_release);
}

void giveUp(const char* const why)
{
    m_why.store(why, std::memory_order_release);
    ESP_LOGW(TAG, "counters are no longer being saved: %s", why);
}

/* Zeroes the counters. Owner task only. Flash first, RAM only once flash has
 * taken it, so a failed reset leaves both exactly as they were. */
bool doReset()
{
    if (saving() && !StatsStore::reset(&m_store))
    {
        if (!m_store.persisting)
        {
            giveUp(WHY_FLASH);
        }
        return false;
    }
    std::memset(&m_live, 0, sizeof(m_live));
    m_powered_ms = 0;
    m_wifi_ms = 0;
    m_stalled_ticks = 0;
    InputConditioner::eventCounts(&m_seen);   /* drop what had not been folded in */
    if (saving())
    {
        m_why.store(nullptr, std::memory_order_release);   /* a fresh sector: no stall */
    }
    publish();
    return true;
}

/* Writes the counters now. Owner task only. */
void doFlush()
{
    takeEvents();
    if (saving())
    {
        StatsStore::append(&m_store, m_live);
        if (!m_store.persisting)
        {
            giveUp(WHY_FLASH);
        }
    }
    publish();
}

} // namespace

void init()
{
    m_owner = xTaskGetCurrentTaskHandle();
    std::memset(&m_live, 0, sizeof(m_live));
    std::memset(&m_seen, 0, sizeof(m_seen));
    m_boot_ms = nowMs();
    m_last_ms = m_boot_ms;
    m_live.boots = 1;   /* this power-up; below it becomes the stored count + 1 */

    m_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           ESP_PARTITION_SUBTYPE_ANY,
                                           PARTITION_LABEL);
    if (nullptr == m_partition)
    {
        /* An update over the air rewrites the app slot and nothing else, so a
         * module updated that way still carries the old partition table. */
        giveUp(WHY_NO_PARTITION);
        publish();
        return;
    }
    if (m_partition->size < REQUIRED_BYTES)
    {
        m_partition = nullptr;
        giveUp(WHY_TOO_SMALL);
        publish();
        return;
    }

    StatsStore::Ops ops = {};
    ops.readSlot = readSlot;
    ops.writeSlot = writeSlot;
    ops.eraseSector = eraseSector;
    ops.ctx = nullptr;
    StatsStore::open(&m_store, ops);

    m_live = m_store.counters;
    m_live.boots++;
    m_why.store(nullptr, std::memory_order_release);
    publish();
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
    bool due = false;
    while (m_powered_ms >= TICK_MS)
    {
        m_powered_ms -= TICK_MS;
        m_live.powered_15s++;
        due = true;
    }

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

    /* The spare is erased ahead of the roll-over that will need it, at the
     * first moment where stalling the strip costs nothing. A supply that has
     * only just come up is the worst moment there is. */
    if (saving())
    {
        const bool settled = quiet &&
                             (now - m_quiet_since_ms >= QUIET_HOLD_MS) &&
                             (now - m_boot_ms >= ERASE_BOOT_HOLDOFF_MS);
        if (settled && StatsStore::needsSpare(&m_store))
        {
            ESP_LOGI(TAG, "erasing the spare sector");
            StatsStore::prepareSpare(&m_store);
        }
    }

    /* A reset asked for from the page or the console. It erases, so it waits
     * for the signals to be off — the instant they are, not the full hold: it
     * is a deliberate action and somebody is watching. */
    if (quiet && m_reset_pending.load(std::memory_order_acquire))
    {
        m_reset_pending.store(false, std::memory_order_relaxed);
        m_reset_outcome.store(doReset() ? Outcome::Ok : Outcome::Failed,
                              std::memory_order_release);
        due = false;   /* the zero record has just been written */
    }

    if (m_flush_pending.exchange(false, std::memory_order_acq_rel))
    {
        doFlush();
        m_flush_serial.fetch_add(1, std::memory_order_release);
        due = false;
    }

    if (due && saving())
    {
        const uint32_t was = m_store.generation;
        if (StatsStore::append(&m_store, m_live))
        {
            m_stalled_ticks = 0;
            if (WHY_STALLED == m_why.load(std::memory_order_relaxed))
            {
                m_why.store(nullptr, std::memory_order_release);
            }
            if (m_store.generation != was)
            {
                ESP_LOGI(TAG, "rolled over to sector %d, generation %u",
                         m_store.sector,
                         static_cast<unsigned>(m_store.generation));
            }
        }
        else if (m_store.persisting)
        {
            /* The sector is full and no quiet moment has come to make room.
             * Nothing is lost while the power holds — but say so, rather than
             * let the page claim the counters are being saved. */
            if (++m_stalled_ticks >= STALL_REPORT_TICKS)
            {
                m_why.store(WHY_STALLED, std::memory_order_release);
            }
        }
        if (!m_store.persisting)
        {
            giveUp(WHY_FLASH);
        }
    }

    publish();
}

void factoryWipe()
{
    if (nullptr == m_partition)
    {
        return;
    }
    if (m_store.persisting)
    {
        /* Zeros in a fresh generation first, then the old sector: a cut
         * anywhere leaves either the old counters or zeros as the newest data,
         * never something in between, and never something older. */
        if (StatsStore::reset(&m_store))
        {
            StatsStore::prepareSpare(&m_store);
        }
    }
    else
    {
        esp_partition_erase_range(m_partition, 0, REQUIRED_BYTES);
    }
    std::memset(&m_live, 0, sizeof(m_live));
    m_partition = nullptr;   /* nothing more is written before the restart */
    m_why.store(WHY_ERASED, std::memory_order_release);
    publish();
}

void get(Counters* out)
{
    /* The owner flips the index once per pass, hundreds of milliseconds
     * apart; a flip during the copy is caught by the second read and the copy
     * is simply redone. No spinning: on this core the owner is the lowest
     * priority task and could never run under a spin. */
    const uint8_t first = m_snap_idx.load(std::memory_order_acquire);
    *out = m_snap[first];
    const uint8_t again = m_snap_idx.load(std::memory_order_acquire);
    if (again != first)
    {
        *out = m_snap[again];
    }
}

const char* notStoredWhy()
{
    return m_why.load(std::memory_order_acquire);
}

bool reset(const uint32_t wait_ms, const char** why)
{
    if (onOwnerTask())
    {
        const bool ok = doReset();
        *why = ok ? "" : notStoredWhy();
        return ok;
    }

    m_reset_outcome.store(Outcome::Pending, std::memory_order_relaxed);
    m_reset_pending.store(true, std::memory_order_release);
    for (uint32_t waited = 0; waited < wait_ms; waited += REQUEST_POLL_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(REQUEST_POLL_MS));
        if (Outcome::Pending != m_reset_outcome.load(std::memory_order_acquire))
        {
            break;
        }
    }

    /* Still pending: take the request back — unless the owner has just taken
     * it, in which case its outcome is moments away. */
    bool expected = true;
    if (m_reset_pending.compare_exchange_strong(expected, false,
                                                std::memory_order_acq_rel))
    {
        *why = WHY_BUSY;
        return false;
    }
    while (Outcome::Pending == m_reset_outcome.load(std::memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(REQUEST_POLL_MS));
    }
    if (Outcome::Ok != m_reset_outcome.load(std::memory_order_acquire))
    {
        const char* const reason = notStoredWhy();
        *why = (nullptr != reason) ? reason : WHY_FLASH;
        return false;
    }
    *why = "";
    return true;
}

void flush(const uint32_t wait_ms)
{
    if (onOwnerTask())
    {
        doFlush();
        return;
    }
    const uint32_t before = m_flush_serial.load(std::memory_order_acquire);
    m_flush_pending.store(true, std::memory_order_release);
    for (uint32_t waited = 0; waited < wait_ms; waited += REQUEST_POLL_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(REQUEST_POLL_MS));
        if (before != m_flush_serial.load(std::memory_order_acquire))
        {
            return;
        }
    }
}

const char* summary()
{
    Counters c;
    get(&c);
    const uint32_t hours = c.powered_15s / (3600 / TICK_SECONDS);
    const uint32_t minutes = (c.powered_15s % (3600 / TICK_SECONDS)) /
                             (60 / TICK_SECONDS);
    const char* const why = notStoredWhy();
    std::snprintf(m_summary, sizeof(m_summary),
                  "%uh%02um powered, %u boots, %u brake, %u left, %u right, "
                  "%u hazard, %u aux%s%s%s",
                  static_cast<unsigned>(hours), static_cast<unsigned>(minutes),
                  static_cast<unsigned>(c.boots),
                  static_cast<unsigned>(c.brake),
                  static_cast<unsigned>(c.left_uses),
                  static_cast<unsigned>(c.right_uses),
                  static_cast<unsigned>(c.hazard_uses),
                  static_cast<unsigned>(c.aux),
                  (nullptr != why) ? " (not saved: " : "",
                  (nullptr != why) ? why : "",
                  (nullptr != why) ? ")" : "");
    return m_summary;
}

} // namespace Stats

/* On-flash format for the ride counters — pure logic, no ESP-IDF.
 *
 * Two 4 KB sectors are used in alternation: the one holding the newest valid
 * data is never the one being erased, so a power cut during a write costs one
 * interval and the module reloads from the other. Everything that decides
 * what is valid lives here so the host tests exercise the same code the
 * module runs.
 *
 * Each sector is 64 slots of 64 bytes. Slot 0 is a header, written only once
 * the erase has returned — it is the sole proof that the erase completed.
 * Slots 1..63 are records, appended in order. Sectors are ordered by the
 * generation in their header, records within a sector by slot index: nothing
 * that decides ordering is stored in a field a torn write can inflate.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace StatsRecord
{

constexpr uint32_t SECTOR_BYTES = 4096;
constexpr uint32_t SLOT_WORDS = 16;
constexpr uint32_t SLOT_BYTES = SLOT_WORDS * 4;              /* 64 */
constexpr int      SLOTS_PER_SECTOR = SECTOR_BYTES / SLOT_BYTES;  /* 64 */
constexpr int      HEADER_SLOT = 0;
constexpr int      FIRST_RECORD_SLOT = HEADER_SLOT + 1;
constexpr int      RECORDS_PER_SECTOR = SLOTS_PER_SECTOR - FIRST_RECORD_SLOT; /* 63 */
constexpr int      SECTOR_COUNT = 2;

constexpr uint32_t HEADER_MAGIC = 0x53544148;   /* 'STAH' */
constexpr uint32_t RECORD_MAGIC = 0xA5C35A3C;   /* both 0s and 1s: erase cannot forge it */
constexpr uint32_t FORMAT_VERSION = 1;
constexpr uint32_t ERASED_WORD = 0xFFFFFFFF;
/* Generations stop one short of the erased value so a header can never be
 * confused with blank flash. 2^32 records at 15 s is ~2000 years; the module
 * gives up persisting rather than carry untestable wrap-around code. */
constexpr uint32_t GENERATION_MAX = 0xFFFFFFFE;

/* The counters, in the order they occupy the record. */
struct Counters
{
    uint32_t boots;
    uint32_t powered_15s;
    uint32_t wifi_15s;
    uint32_t brake;
    uint32_t left_uses;
    uint32_t left_flashes;
    uint32_t right_uses;
    uint32_t right_flashes;
    uint32_t hazard_uses;
    uint32_t hazard_flashes;
    uint32_t aux;
    uint32_t anomalies;
};
constexpr uint32_t COUNTER_WORDS = sizeof(Counters) / 4;   /* 12 */

/* One tick of powered time. The unit the time counters are kept in. */
constexpr uint32_t TICK_SECONDS = 15;
/* Plausibility ceilings, per second of the elapsed time a record reports. */
constexpr uint32_t MAX_BRAKE_PER_S = 1;
constexpr uint32_t MAX_USES_PER_S = 1;
constexpr uint32_t MAX_FLASHES_PER_S = 2;
constexpr uint32_t MAX_AUX_PER_S = 1;

/* Standard CRC-32 (reflected 0xEDB88320, init and final xor 0xFFFFFFFF), the
 * same one zlib computes. It lives here rather than in esp_rom so the device
 * and the host test run one implementation, not two that might disagree. */
uint32_t crc32(const uint8_t* data, size_t len);

/* Slot predicates. A slot is free only when every one of its sixteen words
 * reads erased: a torn write can leave the first word all-ones while later
 * words already have bits cleared, and programming such a slot a second time
 * between erases is how cells come to verify now and rot months later. */
bool slotErased(const uint8_t* slot);

/* Header: valid magic, known format, plausible generation, matching CRC. */
bool headerValid(const uint8_t* slot, uint32_t* generation);
void encodeHeader(uint8_t* slot, uint32_t generation);

/* Record: valid magic, generation matching its sector's, matching CRC. */
bool recordValid(const uint8_t* slot, uint32_t generation, Counters* out);
void encodeRecord(uint8_t* slot, uint32_t generation, const Counters& c);

/* True when the newer record can physically follow the older one: no counter
 * ever decreases, and no event counter grows faster than the elapsed time the
 * record itself reports allows. This is what turns "corrupt but the CRC
 * happened to match" from permanent into rejected. */
bool plausible(const Counters& older, const Counters& newer);

/* Reads SLOT_BYTES of slot `index` into `out`. Returns false on a read
 * error. The scan is written against this rather than a 4 KB buffer so the
 * module needs only one slot of RAM and the host test can back it with an
 * array. */
typedef bool (*SlotReader)(void* ctx, int index, uint8_t* out);

struct ScanResult
{
    bool     header_ok;
    bool     blank;         /* header slot and every record slot read erased */
    uint32_t generation;
    bool     have_record;   /* a valid, coherent record was found */
    Counters counters;      /* only meaningful when have_record */
    int      record_slot;   /* slot the winning record came from, -1 if none */
    int      next_slot;     /* where the next record goes */
    bool     full;          /* no free slot left: roll over before writing */
};

/* Walks one sector: validates the header, then scans backwards for the last
 * occupied slot and takes the newest record that is both valid and coherent
 * with the one before it. */
void scanSector(SlotReader read, void* ctx, ScanResult* out);

} // namespace StatsRecord

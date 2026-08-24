#include "config_store.h"
#include "factory_effects.h"

#include <cstring>
#include "nvs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "ws_protocol.pb.h"

namespace ConfigStore
{

namespace
{

const char* const TAG = "config_nvs";

constexpr const char* NVS_NS = "motolight";
constexpr const char* KEY_SYSCFG = "syscfgpb";
/* The raw-struct blob this replaced. Erased once, then forgotten. */
constexpr const char* KEY_LEGACY_STRUCT = "syscfg";
constexpr const char* KEY_BLINK_MS = "blinkms";

static nvs_handle_t m_nvs;

/* What sits in NVS: a CRC32 followed by the protobuf encoding of the config.
 * NVS checks its own page integrity, so the CRC guards the bytes between here
 * and there — a half-written blob could otherwise still parse. */
constexpr size_t STORED_CRC_BYTES = sizeof(uint32_t);
/* Every field at its maximum, straight from the generated bindings. */
constexpr size_t MAX_ENCODED_BYTES = motolights_Config_size;

/* Staging for the encoded config: save() and load() run in low-priority task
 * context, whose stacks are not sized for two kilobytes of scratch. */
static uint8_t m_blob[STORED_CRC_BYTES + MAX_ENCODED_BYTES];

uint32_t payloadCrc(const uint8_t* const payload, const size_t len)
{
    return esp_rom_crc32_le(0, payload, len);
}

} // namespace

esp_err_t init()
{
    const esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &m_nvs);
    if (ESP_OK == err && ESP_OK == nvs_erase_key(m_nvs, KEY_LEGACY_STRUCT))
    {
        /* One-time: reclaim the raw-struct blob from before the config was
         * stored as protobuf. Its absence is the normal case, not an error. */
        nvs_commit(m_nvs);
        ESP_LOGI(TAG, "dropped the legacy struct blob");
    }
    return err;
}

esp_err_t load(SysConfig* cfg)
{
    size_t len = sizeof(m_blob);
    const esp_err_t err = nvs_get_blob(m_nvs, KEY_SYSCFG, m_blob, &len);
    if (ESP_OK != err)
    {
        if (ESP_ERR_NVS_NOT_FOUND != err)
        {
            ESP_LOGW(TAG, "stored config unreadable: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (len <= STORED_CRC_BYTES)
    {
        ESP_LOGW(TAG, "stored config truncated (%u bytes)",
                 static_cast<unsigned>(len));
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t stored_crc;
    std::memcpy(&stored_crc, m_blob, STORED_CRC_BYTES);
    const uint8_t* const payload = m_blob + STORED_CRC_BYTES;
    const size_t payload_len = len - STORED_CRC_BYTES;
    const uint32_t crc = payloadCrc(payload, payload_len);
    if (crc != stored_crc)
    {
        ESP_LOGW(TAG, "stored config CRC %08x, expected %08x",
                 static_cast<unsigned>(stored_crc), static_cast<unsigned>(crc));
        return ESP_ERR_INVALID_CRC;
    }
    if (!decode(payload, payload_len, cfg))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!validate(cfg))
    {
        ESP_LOGW(TAG, "stored config failed validation");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t save(const SysConfig* cfg)
{
    uint8_t* const payload = m_blob + STORED_CRC_BYTES;
    const size_t len = encode(*cfg, payload, MAX_ENCODED_BYTES,
                              Secrets::Include);
    if (0 == len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t crc = payloadCrc(payload, len);
    std::memcpy(m_blob, &crc, STORED_CRC_BYTES);

    esp_err_t err = nvs_set_blob(m_nvs, KEY_SYSCFG, m_blob,
                                 STORED_CRC_BYTES + len);
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

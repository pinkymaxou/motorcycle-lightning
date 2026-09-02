/* Firmware update over the config WiFi: POST the .bin to /api/ota.
 *
 * The image is streamed straight into the inactive OTA slot — 860 KB will not
 * fit in RAM. The LEDs are left alone: a flash write stalls the render task
 * while the cache is off, so the strips may glitch or sit still for the
 * duration. That is accepted here — an update happens with the config WiFi up,
 * which means the bike is parked and someone is standing at a laptop. */
#include "net_internal.h"

#include <cstring>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

namespace NetServices
{

namespace
{

const char* const TAG = "ota";

/* One flash page per recv: bigger buffers buy nothing here and this one is
 * static because the httpd task's stack is 8 KB. */
constexpr size_t OTA_CHUNK_BYTES = 4096;
/* Long enough for the response to reach the browser before the reset. */
constexpr uint64_t REBOOT_DELAY_US = 800 * 1000;

static uint8_t m_chunk[OTA_CHUNK_BYTES];
static esp_timer_handle_t m_reboot_timer;
static bool m_reboot_pending;

esp_err_t sendText(httpd_req_t* const req, const char* const status,
                   const char* const msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

void rebootCb(void*)
{
    esp_restart();
}

void scheduleReboot()
{
    m_reboot_pending = true;
    if (nullptr == m_reboot_timer)
    {
        const esp_timer_create_args_t args = { rebootCb, nullptr,
                                               ESP_TIMER_TASK, "ota_reboot",
                                               false };
        if (ESP_OK != esp_timer_create(&args, &m_reboot_timer))
        {
            esp_restart();
            return;
        }
    }
    esp_timer_start_once(m_reboot_timer, REBOOT_DELAY_US);
}

/* Refuse an image built for something else before it is written anywhere: the
 * header sits in the first chunk, and a wrong binary is a bricked module until
 * someone opens the box and finds the USB port. */
bool imageIsForThisProject(const uint8_t* const buf, const size_t len,
                           const char** const why)
{
    constexpr size_t HEADER_BYTES = sizeof(esp_image_header_t) +
                                    sizeof(esp_image_segment_header_t) +
                                    sizeof(esp_app_desc_t);
    if (len < HEADER_BYTES)
    {
        *why = "image too short";
        return false;
    }

    const esp_image_header_t* const img =
        reinterpret_cast<const esp_image_header_t*>(buf);
    if (ESP_IMAGE_HEADER_MAGIC != img->magic)
    {
        *why = "not an ESP32 application image";
        return false;
    }

    const esp_app_desc_t* const desc = reinterpret_cast<const esp_app_desc_t*>(
        buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ESP_APP_DESC_MAGIC_WORD != desc->magic_word)
    {
        *why = "no application descriptor";
        return false;
    }

    const esp_app_desc_t* const running = esp_app_get_description();
    if (0 != std::strncmp(desc->project_name, running->project_name,
                          sizeof(desc->project_name)))
    {
        *why = "image belongs to another project";
        return false;
    }
    return true;
}

} // namespace

bool otaRebootPending()
{
    return m_reboot_pending;
}

esp_err_t otaPost(httpd_req_t* const req)
{
    if (!bodyTypeIs(req, BODY_TYPE_OCTETS))
    {
        return sendText(req, "415 Unsupported Media Type",
                        "expected application/octet-stream");
    }
    if (m_reboot_pending)
    {
        /* A second upload queued behind the first would erase the slot the
         * module is about to boot from, and the reboot would cut it short:
         * the bootloader would fall back and the update would be lost. */
        return sendText(req, "503 Service Unavailable", "already rebooting");
    }
    const esp_partition_t* const target = esp_ota_get_next_update_partition(nullptr);
    if (nullptr == target)
    {
        return sendText(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len <= 0 ||
        static_cast<size_t>(req->content_len) > target->size)
    {
        return sendText(req, "400 Bad Request", "image does not fit the slot");
    }
    ESP_LOGW(TAG, "update started: %d bytes -> %s", req->content_len,
             target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (ESP_OK != err)
    {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return sendText(req, "500 Internal Server Error", "cannot open the slot");
    }

    size_t got = 0;
    bool checked = false;
    while (got < static_cast<size_t>(req->content_len))
    {
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(m_chunk),
                                     sizeof(m_chunk));
        if (n <= 0)
        {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "upload interrupted after %u bytes",
                     static_cast<unsigned>(got));
            return sendText(req, "400 Bad Request", "upload interrupted");
        }
        if (!checked)
        {
            const char* why = "";
            if (!imageIsForThisProject(m_chunk, static_cast<size_t>(n), &why))
            {
                esp_ota_abort(handle);
                ESP_LOGE(TAG, "rejected: %s", why);
                return sendText(req, "400 Bad Request", why);
            }
            checked = true;
        }
        err = esp_ota_write(handle, m_chunk, static_cast<size_t>(n));
        if (ESP_OK != err)
        {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return sendText(req, "500 Internal Server Error", "flash write failed");
        }
        got += static_cast<size_t>(n);
    }

    err = esp_ota_end(handle);   /* verifies the image */
    if (ESP_OK != err)
    {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return sendText(req, "400 Bad Request", "image failed verification");
    }
    err = esp_ota_set_boot_partition(target);
    if (ESP_OK != err)
    {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return sendText(req, "500 Internal Server Error", "cannot switch slots");
    }

    ESP_LOGW(TAG, "update written to %s, rebooting", target->label);
    scheduleReboot();
    return sendText(req, "200 OK", "update written, rebooting");
}

} // namespace NetServices

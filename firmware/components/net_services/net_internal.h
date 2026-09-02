#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "sys_config.h"

namespace NetServices
{

esp_err_t wifiStart(const char* ap_ssid, const char* ap_pass,
                    const char* sta_ssid, const char* sta_pass,
                    bool sta_active);
esp_err_t wifiStop();
bool wifiRunning();
int wifiStaCount();
const char* wifiStaIp();   /* "" when not connected */
esp_err_t wifiReconfigureSta(const char* ssid, const char* pass, bool active);

esp_err_t httpStart(SysConfig* live_cfg);
esp_err_t httpStop();

esp_err_t staticFilesRegister(httpd_handle_t server);

/* POST /api/ota — streams a firmware image into the inactive slot and
 * reboots into it (ota_update.cpp). */
esp_err_t otaPost(httpd_req_t* req);
/* True once an image has been written and the reboot timer is running: no
 * further write may touch the slot or the config. */
bool otaRebootPending();

esp_err_t wsStreamStart(httpd_handle_t server);
void wsStreamStop();
/* httpd is closing this socket: forget it before the fd number is recycled
 * for another connection. */
void wsStreamOnSockClose(int fd);

} // namespace NetServices

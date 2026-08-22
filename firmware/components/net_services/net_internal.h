#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "sys_config.h"

namespace NetServices
{

esp_err_t wifiStart(const char *sta_ssid, const char *sta_pass, bool sta_active);
esp_err_t wifiStop();
bool wifiRunning();
int wifiStaCount();
const char *wifiStaIp();   /* "" when not connected */
esp_err_t wifiReconfigureSta(const char *ssid, const char *pass, bool active);

esp_err_t httpStart(SysConfig *live_cfg);
esp_err_t httpStop();

esp_err_t staticFilesRegister(httpd_handle_t server);

esp_err_t wsStreamStart(httpd_handle_t server);
void wsStreamStop();
/* httpd is closing this socket: forget it before the fd number is recycled
 * for another connection. */
void wsStreamOnSockClose(int fd);

} // namespace NetServices

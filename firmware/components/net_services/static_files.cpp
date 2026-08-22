/* Serves the embedded, pre-gzipped single-page app.
 * Regenerate webui_dist/index.html.gz with tools/build_webui.sh. */
#include "net_internal.h"

namespace NetServices
{

namespace
{

extern "C" const uint8_t index_gz_start[] asm("_binary_index_html_gz_start");
extern "C" const uint8_t index_gz_end[] asm("_binary_index_html_gz_end");

esp_err_t hIndex(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(index_gz_start),
                           index_gz_end - index_gz_start);
}

} // namespace

esp_err_t staticFilesRegister(httpd_handle_t server)
{
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = hIndex;
    httpd_uri_t idx = root;
    idx.uri = "/index.html";

    const esp_err_t err = httpd_register_uri_handler(server, &root);
    if (ESP_OK != err)
    {
        return err;
    }
    return httpd_register_uri_handler(server, &idx);
}

} // namespace NetServices

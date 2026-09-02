/* WebSocket push stream (/ws): the module PUSHES protobuf WsMessage frames
 * (proto/ws_protocol.proto) — one Frame per installed strip at ~30 FPS,
 * Status at ~15 Hz. All socket I/O runs on the httpd task via
 * httpd_queue_work; an esp_timer only schedules the pushes. */
#include "net_internal.h"

#include <atomic>
#include <cstring>

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include <pb_encode.h>
#include "ws_protocol.pb.h"

#include "render_core.h"
#include "input_conditioner.h"
#include "sys_config.h"

namespace NetServices
{

namespace
{

const char* const TAG = "ws_stream";

constexpr int WS_MAX_CLIENTS = 4;
constexpr uint64_t PUSH_PERIOD_US = 33 * 1000;  /* ~30 FPS */
constexpr int STATUS_EVERY_N = 2;               /* status every ~66 ms: crisp
                                                   edges on the debug timeline */
/* httpd_ws_send_frame_async() is, despite its name, a blocking send() on the
 * httpd task under the server's send timeout — nothing is queued on the heap.
 * A client that stops reading costs one timeout per frame and is then
 * dropped; the timeout is kept short in http_api.cpp for exactly that. */
/* RFC 6455: control frames carry at most 125 bytes of payload. */
constexpr size_t WS_CONTROL_FRAME_MAX = 125;
/* Frame payload plus protobuf framing. */
constexpr size_t FRAME_BUF_BYTES = CFG_MAX_LEDS * 3 + 64;
constexpr size_t STATUS_BUF_BYTES = 512;

static httpd_handle_t m_server;
static esp_timer_handle_t m_timer;
static int m_fds[WS_MAX_CLIENTS];     /* touched only on the httpd task */
static std::atomic<int> m_n_clients;
static std::atomic<bool> m_work_pending;
static int m_tick;

/* ---- client list (httpd task context only) ---- */

void clientAdd(const int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (fd == m_fds[i])
        {
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (0 == m_fds[i])
        {
            m_fds[i] = fd;
            /* small frames every 33 ms: don't let Nagle batch them */
            const int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            m_n_clients.fetch_add(1);
            ESP_LOGI(TAG, "client fd=%d connected", fd);
            return;
        }
    }
    ESP_LOGW(TAG, "too many ws clients, dropping fd=%d", fd);
    httpd_sess_trigger_close(m_server, fd);
}

void clientRemove(const int fd)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (fd == m_fds[i])
        {
            m_fds[i] = 0;
            m_n_clients.fetch_sub(1);
        }
    }
}

void sendAll(uint8_t* payload, const size_t len)
{
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = payload;
    frame.len = len;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        const int fd = m_fds[i];
        if (0 == fd)
        {
            continue;
        }
        if (ESP_OK != httpd_ws_send_frame_async(m_server, fd, &frame))
        {
            clientRemove(fd);
            httpd_sess_trigger_close(m_server, fd);
            continue;
        }
        /* The LRU purge counts requests *received* on a socket, and a
         * WebSocket only ever sends — so it was always the first session
         * evicted when a browser opened its sixth keep-alive connection.
         * Every push is activity. */
        httpd_sess_update_lru_counter(m_server, fd);
    }
}

/* ---- protobuf encoding (nanopb) ---- */

struct RgbPayload
{
    const uint8_t* data;
    size_t len;
};

/* The LED payload is the one big field: encode it straight from the render
 * core's buffer instead of copying it into the message struct. */
bool encodeRgb(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const RgbPayload* const payload = static_cast<const RgbPayload*>(*arg);
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, payload->data, payload->len);
}

size_t encodeFrameMsg(const StripId strip, uint8_t* out, const size_t cap)
{
    static uint8_t m_rgb[CFG_MAX_LEDS * 3];

    const uint16_t n = RenderCore::getFrame(strip, m_rgb, CFG_MAX_LEDS);
    if (0 == n)
    {
        return 0;   /* strip not installed */
    }

    RgbPayload payload = { m_rgb, static_cast<size_t>(n) * 3 };

    motolights_WsMessage msg = motolights_WsMessage_init_zero;
    msg.which_msg = motolights_WsMessage_frame_tag;
    msg.msg.frame.led_count = n;
    msg.msg.frame.strip = static_cast<uint32_t>(stripIndex(strip));
    msg.msg.frame.rgb.funcs.encode = encodeRgb;
    msg.msg.frame.rgb.arg = &payload;

    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, motolights_WsMessage_fields, &msg))
    {
        ESP_LOGW(TAG, "frame encode failed: %s", PB_GET_ERROR(&stream));
        return 0;
    }
    return stream.bytes_written;
}

size_t encodeStatusMsg(uint8_t* out, const size_t cap)
{
    CondState in;
    InputConditioner::get(&in);
    uint32_t fps_x10, frame_us;
    RenderCore::getStats(&fps_x10, &frame_us);

    motolights_WsMessage msg = motolights_WsMessage_init_zero;
    msg.which_msg = motolights_WsMessage_status_tag;
    motolights_Status& st = msg.msg.status;

    st.has_inputs = true;
    st.inputs.left_blink = in.left_blink;
    st.inputs.left_on = in.left_on;
    st.inputs.right_blink = in.right_blink;
    st.inputs.right_on = in.right_on;
    st.inputs.brake = in.brake;
    st.inputs.aux = in.aux;

    st.blink_period_ms = in.period_ms;
    st.blink_learned = in.learned;
    st.override_active = RenderCore::overrideActive();
    st.fps_x10 = fps_x10;
    st.frame_us_max = frame_us;
    st.heap_free = esp_get_free_heap_size();
    st.sta_count = static_cast<uint32_t>(wifiStaCount());
    strlcpy(st.sta_ip, wifiStaIp(), sizeof(st.sta_ip));
    strlcpy(st.warnings, RenderCore::warnings(), sizeof(st.warnings));
    strlcpy(st.fw, esp_app_get_description()->version, sizeof(st.fw));

    pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, motolights_WsMessage_fields, &msg))
    {
        ESP_LOGW(TAG, "status encode failed: %s", PB_GET_ERROR(&stream));
        return 0;
    }
    return stream.bytes_written;
}

/* ---- push work (httpd task context) ---- */

void pushWork(void* arg)
{
    (void)arg;
    m_work_pending.store(false);

    static uint8_t m_out[FRAME_BUF_BYTES];
    for (int i = 0; i < STRIP_COUNT; i++)
    {
        const size_t len = encodeFrameMsg(stripAt(i), m_out, sizeof(m_out));
        if (0 != len)
        {
            sendAll(m_out, len);
        }
    }

    if (++m_tick >= STATUS_EVERY_N)
    {
        m_tick = 0;
        static uint8_t m_sout[STATUS_BUF_BYTES];
        const size_t len = encodeStatusMsg(m_sout, sizeof(m_sout));
        if (0 != len)
        {
            sendAll(m_sout, len);
        }
    }
}

void timerCb(void* arg)
{
    (void)arg;
    if (nullptr == m_server || 0 == m_n_clients.load())
    {
        return;
    }
    if (m_work_pending.exchange(true))
    {
        return; /* previous push not yet drained */
    }
    if (ESP_OK != httpd_queue_work(m_server, pushWork, nullptr))
    {
        m_work_pending.store(false);
    }
}

/* ---- /ws endpoint ---- */

esp_err_t wsHandler(httpd_req_t* req)
{
    if (HTTP_GET == req->method)
    {
        /* handshake done by httpd — register the socket */
        clientAdd(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* Drain unexpected incoming frames; drop the client on close. */
    httpd_ws_frame_t frame = {};
    const esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (ESP_OK != err)
    {
        return err;
    }
    if (HTTPD_WS_TYPE_CLOSE == frame.type)
    {
        clientRemove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.len > 0 && frame.len <= WS_CONTROL_FRAME_MAX)
    {
        uint8_t tmp[WS_CONTROL_FRAME_MAX];
        frame.payload = tmp;
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    return ESP_OK;
}

} // namespace

void wsStreamOnSockClose(const int fd)
{
    clientRemove(fd);
}

esp_err_t wsStreamStart(httpd_handle_t server)
{
    m_server = server;
    std::memset(m_fds, 0, sizeof(m_fds));
    m_n_clients.store(0);
    m_tick = 0;

    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = wsHandler;
    ws_uri.is_websocket = true;
    esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
    if (ESP_OK != err)
    {
        return err;
    }

    esp_timer_create_args_t targs = {};
    targs.callback = timerCb;
    targs.name = "ws_push";
    targs.skip_unhandled_events = true;
    err = esp_timer_create(&targs, &m_timer);
    if (ESP_OK != err)
    {
        return err;
    }
    return esp_timer_start_periodic(m_timer, PUSH_PERIOD_US);
}

void wsStreamStop()
{
    if (nullptr != m_timer)
    {
        esp_timer_stop(m_timer);
        esp_timer_delete(m_timer);
        m_timer = nullptr;
    }
    m_server = nullptr;
    m_n_clients.store(0);
    std::memset(m_fds, 0, sizeof(m_fds));
}

} // namespace NetServices

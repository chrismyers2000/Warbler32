#include "rtsp_server.h"
#include "audio_pipeline.h"
#include "status_led.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

static const char *TAG = "rtsp";

// ---------------------------------------------------------------------------
// RTP helpers
// ---------------------------------------------------------------------------

typedef struct {
    // Byte 0: V(2) P(1) X(1) CC(4)
    // Byte 1: M(1) PT(7)
    uint8_t  vpxcc;
    uint8_t  mpt;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed)) rtp_header_t;

typedef struct {
    int      udp_sock;
    int      tcp_fd;          // client socket used when tcp_mode=true
    bool     tcp_mode;
    struct sockaddr_in client_addr;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
    volatile bool playing;
    // Serializes the client socket between the RTP sender task and the
    // control-channel responses: in TCP-interleaved mode both write to the
    // same fd, and a control response landing between an RTP frame's
    // partial writes would corrupt the framing.
    SemaphoreHandle_t tx_mtx;
} rtp_session_t;

// One slot per simultaneous client (e.g. BirdNET-Go + a VLC spot-check).
// `in_use` is claimed by the listener task and released by the client task,
// both under s_slots_mtx; everything else in the slot is owned by the client
// task while in_use is set.
typedef struct {
    bool          in_use;
    int           id;          // 1-based, used as the RTSP Session id
    int           client_fd;
    int           reader;      // audio_pipeline subscription, -1 when none
    bool          setup_done;  // a SETUP has populated rtp; PLAY is valid
    rtp_session_t rtp;
    TaskHandle_t  rtp_task;
    // Given by rtp_sender_task right before it exits, so cleanup can wait
    // for it before closing/reusing the socket fd.
    SemaphoreHandle_t rtp_stopped;
} rtsp_client_slot_t;

static rtsp_client_slot_t s_slots[RTSP_MAX_CLIENTS];
static SemaphoreHandle_t  s_slots_mtx;
static atomic_int s_connected = 0;
static atomic_int s_streaming = 0;
static char s_server_ip[16] = "0.0.0.0";

int rtsp_server_client_count(void)    { return atomic_load(&s_connected); }
int rtsp_server_streaming_count(void) { return atomic_load(&s_streaming); }

static void led_refresh(void)
{
    status_led_set(atomic_load(&s_streaming) > 0 ? LED_STREAMING : LED_CONNECTED);
}

// Max interleaved packet: 4-byte RTSP framing + RTP header + one packet of PCM
#define RTP_BUF_MAX (4 + sizeof(rtp_header_t) + RTP_SAMPLES_PER_PACKET * sizeof(int16_t))

// Send exactly `len` bytes on `fd`, retrying on short writes/timeouts up to a
// total budget (each attempt is paced by the socket's SO_SNDTIMEO). Returns
// false on a hard error or if the budget is exceeded — the caller must treat
// that as fatal for the connection, since a partially-written interleaved
// frame permanently desyncs the RTSP TCP framing for every frame after it.
static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    int attempts = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);

    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        attempts++;
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (xTaskGetTickCount() >= deadline) {
                ESP_LOGW(TAG, "TCP send stalled >1.5s, ending session");
                return false;
            }
            continue;
        }
        ESP_LOGW(TAG, "TCP send error: errno %d", errno);
        return false;
    }

    if (attempts > 1) {
        static uint32_t s_warn_ms = 0;
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - s_warn_ms >= 1000) {
            ESP_LOGW(TAG, "TCP send backpressure: %d attempts for %u bytes", attempts, (unsigned)len);
            s_warn_ms = now_ms;
        }
    }
    return true;
}

// Returns false if the frame could not be delivered and the session should end.
static bool rtp_send_packet(rtp_session_t *s, const uint8_t *payload, size_t len)
{
    uint8_t pkt[RTP_BUF_MAX];
    int offset = s->tcp_mode ? 4 : 0;

    rtp_header_t *hdr = (rtp_header_t *)(pkt + offset);
    hdr->vpxcc     = 0x80;
    hdr->mpt       = RTP_PAYLOAD & 0x7F;
    hdr->seq       = htons(s->seq++);
    hdr->timestamp = htonl(s->timestamp);
    hdr->ssrc      = htonl(s->ssrc);

    // Advance by the actual sample count delivered, not the nominal packet
    // size — a short read (e.g. right after a client subscribes, before
    // the ring buffer has a full packet queued) would otherwise desync the
    // RTP clock from the real audio position.
    s->timestamp += len / sizeof(int16_t);

    memcpy(pkt + offset + sizeof(rtp_header_t), payload, len);

    size_t rtp_len = sizeof(rtp_header_t) + len;
    if (s->tcp_mode) {
        // RFC 2326 §10.12: interleaved binary data framing over RTSP TCP
        pkt[0] = '$';
        pkt[1] = 0;  // channel 0 = RTP
        pkt[2] = (uint8_t)(rtp_len >> 8);
        pkt[3] = (uint8_t)(rtp_len & 0xFF);
        xSemaphoreTake(s->tx_mtx, portMAX_DELAY);
        bool ok = send_all(s->tcp_fd, pkt, 4 + rtp_len);
        xSemaphoreGive(s->tx_mtx);
        return ok;
    }

    // UDP is message-based (no partial-write/framing-desync risk); ordinary
    // packet loss here is normal RTP behavior, not a correctness bug.
    sendto(s->udp_sock, pkt + offset, rtp_len, 0,
           (struct sockaddr *)&s->client_addr, sizeof(s->client_addr));
    return true;
}

// ---------------------------------------------------------------------------
// RTP sender task (one per playing client)
// ---------------------------------------------------------------------------

static void rtp_sender_task(void *arg)
{
    rtsp_client_slot_t *c = (rtsp_client_slot_t *)arg;
    uint8_t *pcm_buf = malloc(RTP_SAMPLES_PER_PACKET * sizeof(int16_t));

    ESP_LOGI(TAG, "[%d] RTP sender started → %s:%d", c->id,
             inet_ntoa(c->rtp.client_addr.sin_addr),
             ntohs(c->rtp.client_addr.sin_port));

    while (c->rtp.playing && pcm_buf != NULL) {
        size_t got = audio_pipeline_read(c->reader, pcm_buf,
                                         RTP_SAMPLES_PER_PACKET * sizeof(int16_t), 200);
        if (got == 0) continue;
        if (!rtp_send_packet(&c->rtp, pcm_buf, got))
            c->rtp.playing = false;
    }

    free(pcm_buf);
    ESP_LOGI(TAG, "[%d] RTP sender stopped", c->id);
    xSemaphoreGive(c->rtp_stopped);
    vTaskDelete(NULL);
}

// Send a control-channel response, serialized against the RTP sender's
// writes on the same socket (see rtp_session_t.tx_mtx). Returns false when
// the send failed and the session should end.
static bool ctrl_send(rtsp_client_slot_t *c, const char *data, size_t len)
{
    xSemaphoreTake(c->rtp.tx_mtx, portMAX_DELAY);
    bool ok = send_all(c->client_fd, (const uint8_t *)data, len);
    xSemaphoreGive(c->rtp.tx_mtx);
    return ok;
}

// Stop this slot's sender task (waiting for it to actually exit) and drop
// its pipeline subscription. Safe to call when nothing is playing.
static void stop_streaming(rtsp_client_slot_t *c)
{
    if (c->rtp_task != NULL) {
        c->rtp.playing = false;
        // Wait for the sender to actually exit before the fd can be closed
        // or reused — otherwise a still-running sender can write stray RTP
        // bytes into the next client's control stream. Must exceed
        // send_all()'s 1.5 s stall budget, or a sender blocked in its final
        // send can outlive this wait and race the slot's next occupant.
        xSemaphoreTake(c->rtp_stopped, pdMS_TO_TICKS(2000));
        c->rtp_task = NULL;
        atomic_fetch_sub(&s_streaming, 1);
    }
    if (c->reader >= 0) {
        audio_pipeline_unsubscribe(c->reader);
        c->reader = -1;
    }
    // After a TEARDOWN the client must re-SETUP before another PLAY
    c->setup_done = false;
    led_refresh();
}

// ---------------------------------------------------------------------------
// Minimal RTSP helpers
// ---------------------------------------------------------------------------

// Extract a header value from an RTSP request (case-insensitive key search)
static const char *rtsp_get_header(const char *req, const char *key)
{
    const char *p = req;
    size_t klen = strlen(key);
    while ((p = strcasestr(p, key)) != NULL) {
        // Make sure it's at a line start (preceded by \n or start of string)
        if (p == req || *(p - 1) == '\n') {
            p += klen;
            if (*p == ':') {
                p++;
                while (*p == ' ') p++;
                return p;
            }
        } else {
            p++;
        }
    }
    return NULL;
}

static int rtsp_get_cseq(const char *req)
{
    const char *v = rtsp_get_header(req, "CSeq");
    return v ? atoi(v) : 0;
}

// Build the SDP body describing a mono L16 audio stream
static int build_sdp(char *buf, size_t size, const char *server_ip)
{
    return snprintf(buf, size,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=Warbler32\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d L16/%lu/%d\r\n"
        "a=sendonly\r\n"
        "a=control:trackID=0\r\n",
        server_ip, server_ip,
        RTP_PAYLOAD, RTP_PAYLOAD,
        (unsigned long)g_config.sample_rate, AUDIO_CHANNELS);
}

// ---------------------------------------------------------------------------
// RTSP session handler (runs in a per-connection task)
// ---------------------------------------------------------------------------

static void handle_rtsp_client(rtsp_client_slot_t *c, const char *server_ip)
{
    char buf[2048];
    char resp[512];
    char sdp[256];
    int client_fd = c->client_fd;
    int buf_len = 0;
    bool done = false;

    ESP_LOGI(TAG, "[%d] client connected", c->id);

    while (!done) {
        int len = recv(client_fd, buf + buf_len, sizeof(buf) - 1 - buf_len, 0);
        if (len <= 0) break;
        buf_len += len;
        buf[buf_len] = '\0';

        // Process every complete request (\r\n\r\n terminated) in the buffer.
        // gortsplib (used by BirdNET-Go) pipelines SETUP+PLAY in one TCP segment,
        // so a single recv() can contain multiple back-to-back requests.
        char *pos = buf;
        while (pos < buf + buf_len) {
            char *req_end = strstr(pos, "\r\n\r\n");
            if (!req_end) break;
            req_end += 4;

            char saved = *req_end;
            *req_end = '\0';

            int cseq = rtsp_get_cseq(pos);
            ESP_LOGI(TAG, "[%d] >> %.60s", c->id, pos);

            if (strncmp(pos, "OPTIONS", 7) == 0) {
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n",
                    cseq);
                if (!ctrl_send(c, resp, strlen(resp)))
                    done = true;

            } else if (strncmp(pos, "DESCRIBE", 8) == 0) {
                int sdp_len = build_sdp(sdp, sizeof(sdp), server_ip);
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Content-Base: rtsp://%s/audio/\r\n"
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: %d\r\n\r\n",
                    cseq, server_ip, sdp_len);
                if (!ctrl_send(c, resp, strlen(resp)) ||
                    !ctrl_send(c, sdp, (size_t)sdp_len))
                    done = true;

            } else if (strncmp(pos, "SETUP", 5) == 0) {
                if (c->rtp_task != NULL) {
                    // Transport can't be renegotiated mid-play: the sender
                    // task is actively using the current session/socket.
                    // A client that wants a new transport must TEARDOWN first.
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 455 Method Not Valid in This State\r\n"
                        "CSeq: %d\r\n\r\n", cseq);
                    if (!ctrl_send(c, resp, strlen(resp)))
                        done = true;
                    *req_end = saved;
                    pos = req_end;
                    if (done) break;
                    continue;
                }

                const char *transport = rtsp_get_header(pos, "Transport");
                bool tcp = transport && strstr(transport, "TCP") != NULL;

                c->rtp.tcp_mode  = tcp;
                c->rtp.tcp_fd    = tcp ? client_fd : -1;
                c->rtp.ssrc      = (uint32_t)esp_random();
                c->rtp.seq       = (uint16_t)esp_random();
                c->rtp.timestamp = esp_random();

                if (tcp) {
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                        "Session: %d\r\n\r\n",
                        cseq, c->id);
                    if (!ctrl_send(c, resp, strlen(resp)))
                        done = true;
                    c->setup_done = true;
                    ESP_LOGI(TAG, "[%d] SETUP: RTP → TCP interleaved", c->id);
                } else {
                    uint16_t client_rtp_port = 5004;
                    if (transport) {
                        const char *cp = strstr(transport, "client_port=");
                        if (cp) client_rtp_port = (uint16_t)atoi(cp + 12);
                    }

                    struct sockaddr_in peer;
                    socklen_t peer_len = sizeof(peer);
                    getpeername(client_fd, (struct sockaddr *)&peer, &peer_len);

                    c->rtp.client_addr.sin_family = AF_INET;
                    c->rtp.client_addr.sin_addr   = peer.sin_addr;
                    c->rtp.client_addr.sin_port   = htons(client_rtp_port);

                    if (c->rtp.udp_sock >= 0) close(c->rtp.udp_sock);
                    c->rtp.udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=5004-5005\r\n"
                        "Session: %d\r\n\r\n",
                        cseq, client_rtp_port, client_rtp_port + 1, c->id);
                    if (!ctrl_send(c, resp, strlen(resp)))
                        done = true;
                    c->setup_done = true;
                    ESP_LOGI(TAG, "[%d] SETUP: RTP → UDP %s:%d", c->id,
                             inet_ntoa(c->rtp.client_addr.sin_addr), client_rtp_port);
                }

            } else if (strncmp(pos, "PLAY", 4) == 0) {
                if (!c->setup_done) {
                    // Without a SETUP the rtp session is unconfigured — a
                    // sender started now would stream into a zeroed
                    // address / fd -1 while burning a pipeline reader slot.
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 455 Method Not Valid in This State\r\n"
                        "CSeq: %d\r\n\r\n", cseq);
                    if (!ctrl_send(c, resp, strlen(resp)))
                        done = true;
                    *req_end = saved;
                    pos = req_end;
                    if (done) break;
                    continue;
                }
                if (c->rtp_task == NULL) {
                    c->reader = audio_pipeline_subscribe(true);
                    if (c->reader < 0) {
                        snprintf(resp, sizeof(resp),
                            "RTSP/1.0 453 Not Enough Bandwidth\r\n"
                            "CSeq: %d\r\n\r\n", cseq);
                        if (!ctrl_send(c, resp, strlen(resp)))
                            done = true;
                        *req_end = saved;
                        pos = req_end;
                        if (done) break;
                        continue;
                    }
                    // Drain a possibly-stale give from a previous session
                    // whose 500 ms stop-wait timed out.
                    xSemaphoreTake(c->rtp_stopped, 0);
                    c->rtp.playing = true;
                    xTaskCreatePinnedToCore(rtp_sender_task, "rtp_sender",
                                            TASK_RTP_STACK, c,
                                            TASK_RTP_PRIORITY, &c->rtp_task,
                                            TASK_RTP_CORE);
                    atomic_fetch_add(&s_streaming, 1);
                    led_refresh();
                }

                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Session: %d\r\n"
                    "RTP-Info: url=rtsp://%s/audio/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
                    cseq, c->id, server_ip,
                    (unsigned)c->rtp.seq, (unsigned)c->rtp.timestamp);
                if (!ctrl_send(c, resp, strlen(resp)))
                    done = true;
                ESP_LOGI(TAG, "[%d] PLAY", c->id);

            } else if (strncmp(pos, "TEARDOWN", 8) == 0) {
                stop_streaming(c);
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Session: %d\r\n\r\n",
                    cseq, c->id);
                ctrl_send(c, resp, strlen(resp));
                ESP_LOGI(TAG, "[%d] TEARDOWN", c->id);
                *req_end = saved;
                pos = req_end;
                done = true;
                break;

            } else {
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 405 Method Not Allowed\r\n"
                    "CSeq: %d\r\n\r\n", cseq);
                if (!ctrl_send(c, resp, strlen(resp)))
                    done = true;
            }

            *req_end = saved;
            pos = req_end;
            // A failed/short control send is session-fatal: on the TCP-
            // interleaved transport it permanently desyncs the framing, and
            // even over UDP a client hung waiting on a lost response is
            // better served by a clean disconnect.
            if (done) break;
        }

        // Slide any unprocessed bytes to the front of the buffer
        int consumed = (int)(pos - buf);
        buf_len -= consumed;
        if (buf_len > 0 && consumed > 0)
            memmove(buf, pos, buf_len);
        else if (consumed > 0)
            buf_len = 0;
    }

    ESP_LOGI(TAG, "[%d] client disconnected", c->id);
}

// Per-connection task: run the session, then release the slot.
static void rtsp_client_task(void *arg)
{
    rtsp_client_slot_t *c = (rtsp_client_slot_t *)arg;

    handle_rtsp_client(c, s_server_ip);

    stop_streaming(c);
    close(c->client_fd);
    c->client_fd = -1;
    if (c->rtp.udp_sock >= 0) { close(c->rtp.udp_sock); c->rtp.udp_sock = -1; }

    xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
    c->in_use = false;
    xSemaphoreGive(s_slots_mtx);
    atomic_fetch_sub(&s_connected, 1);

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// RTSP listener task
// ---------------------------------------------------------------------------

static void rtsp_listener_task(void *arg)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(RTSP_PORT),
    };
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, RTSP_MAX_CLIENTS);

    ESP_LOGI(TAG, "RTSP listening on port %d (max %d clients)",
             RTSP_PORT, RTSP_MAX_CLIENTS);

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(s_server_ip, sizeof(s_server_ip), IPSTR, IP2STR(&ip_info.ip));
    }
    ESP_LOGI(TAG, "stream URL → rtsp://%s/audio", s_server_ip);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "accept error: errno %d", errno);
            continue;
        }
        // Re-read our own IP for this session's DESCRIBE/SDP: a runtime
        // WiFi reconnect can (rarely) come back with a different DHCP
        // address, which would leave the boot-time snapshot stale.
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            snprintf(s_server_ip, sizeof(s_server_ip), IPSTR, IP2STR(&ip_info.ip));
        // Disable Nagle algorithm so RTP frames are sent immediately,
        // and cap send() blocking time to avoid stalling the audio pipeline.
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval sndtv = { .tv_sec = 0, .tv_usec = 20000 }; // 20 ms
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv));

        rtsp_client_slot_t *slot = NULL;
        xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
        for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
            if (!s_slots[i].in_use) {
                s_slots[i].in_use     = true;
                s_slots[i].client_fd  = client_fd;
                s_slots[i].reader     = -1;
                s_slots[i].rtp_task   = NULL;
                s_slots[i].setup_done = false;
                slot = &s_slots[i];
                break;
            }
        }
        xSemaphoreGive(s_slots_mtx);

        if (slot == NULL) {
            ESP_LOGW(TAG, "rejecting client: %d sessions already active",
                     RTSP_MAX_CLIENTS);
            static const char busy[] =
                "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: 0\r\n\r\n";
            send(client_fd, busy, sizeof(busy) - 1, 0);
            close(client_fd);
            continue;
        }

        atomic_fetch_add(&s_connected, 1);
        char task_name[16];
        snprintf(task_name, sizeof(task_name), "rtsp_cli%d", slot->id);
        if (xTaskCreatePinnedToCore(rtsp_client_task, task_name,
                                    TASK_RTSP_STACK, slot,
                                    TASK_RTSP_PRIORITY, NULL,
                                    TASK_RTSP_CORE) != pdPASS) {
            ESP_LOGE(TAG, "failed to start client task");
            close(client_fd);
            xSemaphoreTake(s_slots_mtx, portMAX_DELAY);
            slot->in_use = false;
            xSemaphoreGive(s_slots_mtx);
            atomic_fetch_sub(&s_connected, 1);
        }
    }

    vTaskDelete(NULL);
}

esp_err_t rtsp_server_start(void)
{
    s_slots_mtx = xSemaphoreCreateMutex();
    if (s_slots_mtx == NULL) return ESP_ERR_NO_MEM;

    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        s_slots[i].id           = i + 1;
        s_slots[i].client_fd    = -1;
        s_slots[i].reader       = -1;
        s_slots[i].rtp.udp_sock = -1;
        s_slots[i].rtp_stopped  = xSemaphoreCreateBinary();
        s_slots[i].rtp.tx_mtx   = xSemaphoreCreateMutex();
        if (s_slots[i].rtp_stopped == NULL || s_slots[i].rtp.tx_mtx == NULL)
            return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        rtsp_listener_task, "rtsp_ctrl",
        TASK_RTSP_STACK, NULL,
        TASK_RTSP_PRIORITY, NULL,
        TASK_RTSP_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

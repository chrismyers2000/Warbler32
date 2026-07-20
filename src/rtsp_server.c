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
    bool     playing;
} rtp_session_t;

static rtp_session_t s_rtp = { .udp_sock = -1 };

// Given by rtp_sender_task right before it exits, so the session cleanup
// path can wait for it before closing/reusing its socket fd.
static SemaphoreHandle_t s_rtp_stopped;

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

    s->timestamp += RTP_SAMPLES_PER_PACKET;

    memcpy(pkt + offset + sizeof(rtp_header_t), payload, len);

    size_t rtp_len = sizeof(rtp_header_t) + len;
    if (s->tcp_mode) {
        // RFC 2326 §10.12: interleaved binary data framing over RTSP TCP
        pkt[0] = '$';
        pkt[1] = 0;  // channel 0 = RTP
        pkt[2] = (uint8_t)(rtp_len >> 8);
        pkt[3] = (uint8_t)(rtp_len & 0xFF);
        return send_all(s->tcp_fd, pkt, 4 + rtp_len);
    }

    // UDP is message-based (no partial-write/framing-desync risk); ordinary
    // packet loss here is normal RTP behavior, not a correctness bug.
    sendto(s->udp_sock, pkt + offset, rtp_len, 0,
           (struct sockaddr *)&s->client_addr, sizeof(s->client_addr));
    return true;
}

// ---------------------------------------------------------------------------
// RTP sender task (runs while a client is playing)
// ---------------------------------------------------------------------------

static void rtp_sender_task(void *arg)
{
    static uint8_t pcm_buf[RTP_SAMPLES_PER_PACKET * sizeof(int16_t)];

    ESP_LOGI(TAG, "RTP sender started → %s:%d",
             inet_ntoa(s_rtp.client_addr.sin_addr),
             ntohs(s_rtp.client_addr.sin_port));

    while (s_rtp.playing) {
        size_t got = audio_pipeline_read(pcm_buf, sizeof(pcm_buf), 200);
        if (got == 0) continue;
        if (!rtp_send_packet(&s_rtp, pcm_buf, got))
            s_rtp.playing = false;
    }

    ESP_LOGI(TAG, "RTP sender stopped");
    xSemaphoreGive(s_rtp_stopped);
    vTaskDelete(NULL);
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
// RTSP session handler (runs per accepted TCP connection)
// ---------------------------------------------------------------------------

static void handle_rtsp_client(int client_fd, const char *server_ip)
{
    char buf[2048];
    char resp[512];
    char sdp[256];
    TaskHandle_t rtp_task = NULL;
    int buf_len = 0;
    bool done = false;

    ESP_LOGI(TAG, "client connected");

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
            ESP_LOGI(TAG, ">> %.60s", pos);

            if (strncmp(pos, "OPTIONS", 7) == 0) {
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n",
                    cseq);
                send(client_fd, resp, strlen(resp), 0);

            } else if (strncmp(pos, "DESCRIBE", 8) == 0) {
                int sdp_len = build_sdp(sdp, sizeof(sdp), server_ip);
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Content-Base: rtsp://%s/audio/\r\n"
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: %d\r\n\r\n",
                    cseq, server_ip, sdp_len);
                send(client_fd, resp, strlen(resp), 0);
                send(client_fd, sdp, sdp_len, 0);

            } else if (strncmp(pos, "SETUP", 5) == 0) {
                const char *transport = rtsp_get_header(pos, "Transport");
                bool tcp = transport && strstr(transport, "TCP") != NULL;

                s_rtp.tcp_mode  = tcp;
                s_rtp.tcp_fd    = tcp ? client_fd : -1;
                s_rtp.ssrc      = (uint32_t)esp_random();
                s_rtp.seq       = (uint16_t)esp_random();
                s_rtp.timestamp = esp_random();

                if (tcp) {
                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                        "Session: 1\r\n\r\n",
                        cseq);
                    send(client_fd, resp, strlen(resp), 0);
                    ESP_LOGI(TAG, "SETUP: RTP → TCP interleaved");
                } else {
                    uint16_t client_rtp_port = 5004;
                    if (transport) {
                        const char *cp = strstr(transport, "client_port=");
                        if (cp) client_rtp_port = (uint16_t)atoi(cp + 12);
                    }

                    struct sockaddr_in peer;
                    socklen_t peer_len = sizeof(peer);
                    getpeername(client_fd, (struct sockaddr *)&peer, &peer_len);

                    s_rtp.client_addr.sin_family = AF_INET;
                    s_rtp.client_addr.sin_addr   = peer.sin_addr;
                    s_rtp.client_addr.sin_port   = htons(client_rtp_port);

                    if (s_rtp.udp_sock >= 0) close(s_rtp.udp_sock);
                    s_rtp.udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

                    snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "CSeq: %d\r\n"
                        "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=5004-5005\r\n"
                        "Session: 1\r\n\r\n",
                        cseq, client_rtp_port, client_rtp_port + 1);
                    send(client_fd, resp, strlen(resp), 0);
                    ESP_LOGI(TAG, "SETUP: RTP → UDP %s:%d",
                             inet_ntoa(s_rtp.client_addr.sin_addr), client_rtp_port);
                }

            } else if (strncmp(pos, "PLAY", 4) == 0) {
                audio_pipeline_flush();
                s_rtp.playing = true;
                xTaskCreatePinnedToCore(rtp_sender_task, "rtp_sender",
                                        TASK_RTP_STACK, NULL,
                                        TASK_RTP_PRIORITY, &rtp_task,
                                        TASK_RTP_CORE);
                status_led_set(LED_STREAMING);

                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Session: 1\r\n"
                    "RTP-Info: url=rtsp://%s/audio/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
                    cseq, server_ip, (unsigned)s_rtp.seq, (unsigned)s_rtp.timestamp);
                send(client_fd, resp, strlen(resp), 0);
                ESP_LOGI(TAG, "PLAY");

            } else if (strncmp(pos, "TEARDOWN", 8) == 0) {
                s_rtp.playing = false;
                status_led_set(LED_CONNECTED);
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 200 OK\r\n"
                    "CSeq: %d\r\n"
                    "Session: 1\r\n\r\n",
                    cseq);
                send(client_fd, resp, strlen(resp), 0);
                ESP_LOGI(TAG, "TEARDOWN");
                *req_end = saved;
                pos = req_end;
                done = true;
                break;

            } else {
                snprintf(resp, sizeof(resp),
                    "RTSP/1.0 405 Method Not Allowed\r\n"
                    "CSeq: %d\r\n\r\n", cseq);
                send(client_fd, resp, strlen(resp), 0);
            }

            *req_end = saved;
            pos = req_end;
        }

        // Slide any unprocessed bytes to the front of the buffer
        int consumed = (int)(pos - buf);
        buf_len -= consumed;
        if (buf_len > 0 && consumed > 0)
            memmove(buf, pos, buf_len);
        else if (consumed > 0)
            buf_len = 0;
    }

    s_rtp.playing = false;
    if (rtp_task != NULL) {
        // Wait for the sender task to actually exit before this fd can be
        // reused by the next accept() — otherwise a still-running sender
        // can write stray RTP bytes into the next client's control stream.
        xSemaphoreTake(s_rtp_stopped, pdMS_TO_TICKS(500));
        rtp_task = NULL;
    }
    status_led_set(LED_CONNECTED);
    close(client_fd);
    if (s_rtp.udp_sock >= 0) { close(s_rtp.udp_sock); s_rtp.udp_sock = -1; }
    ESP_LOGI(TAG, "client disconnected");
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
    listen(server_fd, 1);

    ESP_LOGI(TAG, "RTSP listening on port %d", RTSP_PORT);

    char server_ip[16] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(server_ip, sizeof(server_ip), IPSTR, IP2STR(&ip_info.ip));
    }
    ESP_LOGI(TAG, "stream URL → rtsp://%s/audio", server_ip);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "accept error: errno %d", errno);
            continue;
        }
        // Disable Nagle algorithm so RTP frames are sent immediately,
        // and cap send() blocking time to avoid stalling the audio pipeline.
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval sndtv = { .tv_sec = 0, .tv_usec = 20000 }; // 20 ms
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &sndtv, sizeof(sndtv));
        handle_rtsp_client(client_fd, server_ip);
    }

    vTaskDelete(NULL);
}

esp_err_t rtsp_server_start(void)
{
    s_rtp_stopped = xSemaphoreCreateBinary();

    BaseType_t ok = xTaskCreatePinnedToCore(
        rtsp_listener_task, "rtsp_ctrl",
        TASK_RTSP_STACK, NULL,
        TASK_RTSP_PRIORITY, NULL,
        TASK_RTSP_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

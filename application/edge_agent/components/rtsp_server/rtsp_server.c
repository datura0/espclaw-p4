/*
 * Minimal RTSP + RTP/H264 server — lwIP sockets, single-task, up to 2 clients
 *
 * Protocol support:
 *   - RTSP: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN
 *   - RTP over UDP (RFC 3550 + RFC 6184 for H264)
 *   - SDP generation
 *
 * Runs a FreeRTOS task that listens on TCP port for RTSP, and sends RTP via UDP.
 */
#include "rtsp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include <errno.h>

static const char *TAG = "RTSP";

/* ---- H264 NAL types ---- */
#define NAL_TYPE_SPS         7
#define NAL_TYPE_PPS         8
#define NAL_TYPE_IDR         5
#define NAL_TYPE_NON_IDR     1
#define NAL_TYPE_SEI         6
#define NAL_TYPE_FU_A       28

/* ---- RTP constants ---- */
#define RTP_VERSION           2
#define RTP_PAYLOAD_H264     96
#define RTP_HDR_SIZE         12
#define RTP_MAX_PAYLOAD      (1400 - RTP_HDR_SIZE)

/* ---- RTSP states ---- */
typedef enum {
    RTSP_STATE_INIT,
    RTSP_STATE_READY,
    RTSP_STATE_PLAYING,
} rtsp_client_state_t;

typedef struct rtsp_client {
    int                     sock;      /* RTSP TCP */
    int                     rtp_sock;  /* RTP UDP */
    struct sockaddr_in      rtp_addr;
    uint16_t                rtp_port;
    uint16_t                rtcp_port;
    uint32_t                ssrc;
    uint16_t                seq;
    uint32_t                timestamp;
    rtsp_client_state_t     state;
    char                    session[32];
} rtsp_client_t;

struct rtsp_server {
    rtsp_config_t           cfg;
    int                     listen_sock;
    rtsp_client_t           clients[2];
    int                     client_count;
    TaskHandle_t            task;
    uint8_t                *sps_nal;
    size_t                  sps_len;
    uint8_t                *pps_nal;
    size_t                  pps_len;
};

/* ---- SDP description ---- */
static const char *sdp_template =
    "v=0\r\n"
    "o=- %"PRIu32" %"PRIu32" IN IP4 0.0.0.0\r\n"
    "s=ESP32-P4 RTSP Stream\r\n"
    "t=0 0\r\n"
    "a=control:*\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1;profile-level-id=42001E;sprop-parameter-sets=";

/* ---- RTP header write ---- */
static inline int rtp_write_header(uint8_t *buf, uint8_t pt, uint8_t marker,
                                    uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    buf[0]  = (uint8_t)((RTP_VERSION << 6) | 0x00);  /* V=2, P=0, X=0, CC=0 */
    buf[1]  = (uint8_t)((marker << 7) | (pt & 0x7F));
    buf[2]  = (uint8_t)(seq >> 8);
    buf[3]  = (uint8_t)(seq & 0xFF);
    buf[4]  = (uint8_t)(ts >> 24);
    buf[5]  = (uint8_t)(ts >> 16);
    buf[6]  = (uint8_t)(ts >> 8);
    buf[7]  = (uint8_t)(ts & 0xFF);
    buf[8]  = (uint8_t)(ssrc >> 24);
    buf[9]  = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >> 8);
    buf[11] = (uint8_t)(ssrc & 0xFF);
    return RTP_HDR_SIZE;
}

/* ---- Send RTP single NAL unit ---- */
static void rtp_send_single(rtsp_client_t *c, const uint8_t *nal, size_t len, uint32_t ts)
{
    if (len <= (size_t)RTP_MAX_PAYLOAD) {
        uint8_t buf[1400];
        int off = rtp_write_header(buf, RTP_PAYLOAD_H264, (len > 0 && (nal[0] & 0x1F) == NAL_TYPE_IDR) ? 1 : 0,
                                   c->seq++, ts, c->ssrc);
        memcpy(buf + off, nal, len);
        int sent = sendto(c->rtp_sock, buf, off + len, 0, (struct sockaddr *)&c->rtp_addr, sizeof(c->rtp_addr));
        if (sent < 0) ESP_LOGW(TAG, "RTP single sendto failed: %d", errno);
    }
}

/* ---- Send RTP Fragmentation Unit-A (FU-A) for large NALs ---- */
static void rtp_send_fua(rtsp_client_t *c, const uint8_t *nal, size_t len, uint32_t ts)
{
    uint8_t nalu_hdr = nal[0];
    uint8_t fu_indicator = (uint8_t)((nalu_hdr & 0xE0) | 28);  /* F+NRI+type=FU-A */
    uint8_t fu_header_start  = (uint8_t)(0x80 | (nalu_hdr & 0x1F));
    uint8_t fu_header_mid    = (uint8_t)(0x00 | (nalu_hdr & 0x1F));
    uint8_t fu_header_end    = (uint8_t)(0x40 | (nalu_hdr & 0x1F));

    const uint8_t *payload = nal + 1;
    size_t remaining = len - 1;
    bool first = true;

    while (remaining > 0) {
        size_t chunk = remaining > (size_t)(RTP_MAX_PAYLOAD - 2) ? (size_t)(RTP_MAX_PAYLOAD - 2) : remaining;
        bool final = (chunk == remaining);
        uint8_t buf[1400];
        int off = rtp_write_header(buf, RTP_PAYLOAD_H264, final ? 1 : 0, c->seq++, ts, c->ssrc);
        buf[off++] = fu_indicator;
        if (first) { buf[off++] = fu_header_start; first = false; }
        else if (final) { buf[off++] = fu_header_end; }
        else { buf[off++] = fu_header_mid; }
        memcpy(buf + off, payload, chunk);
        int sent = sendto(c->rtp_sock, buf, off + chunk, 0, (struct sockaddr *)&c->rtp_addr, sizeof(c->rtp_addr));
        if (sent < 0) ESP_LOGW(TAG, "RTP fua sendto failed: %d", errno);
        payload += chunk;
        remaining -= chunk;
        if (remaining > 0) vTaskDelay(1);  /* throttle to avoid UDP burst loss */
    }
}

/* ---- Send H264 NAL as RTP ---- */
static void rtp_send_nal(rtsp_client_t *c, const uint8_t *nal, size_t len, uint32_t ts)
{
    if (len > (size_t)RTP_MAX_PAYLOAD) {
        rtp_send_fua(c, nal, len, ts);
    } else {
        rtp_send_single(c, nal, len, ts);
    }
}

/* ---- Find H264 start code (00 00 00 01 or 00 00 01) ---- */
static const uint8_t* find_start_code(const uint8_t *data, size_t len, size_t *consumed)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == 0 && data[i+1] == 0) {
            if (data[i+2] == 1) {
                *consumed = i + 3;
                return data + i + 3;
            }
            if (data[i+2] == 0 && data[i+3] == 1) {
                *consumed = i + 4;
                return data + i + 4;
            }
        }
    }
    *consumed = len;
    return NULL;
}

/* ---- Store SPS/PPS for SDP ---- */
static void store_sps_pps(rtsp_server_t *srv, const uint8_t *nal, size_t len)
{
    uint8_t type = nal[0] & 0x1F;
    if (type == NAL_TYPE_SPS) {
        free(srv->sps_nal);
        srv->sps_nal = malloc(len);
        memcpy(srv->sps_nal, nal, len);
        srv->sps_len = len;
    } else if (type == NAL_TYPE_PPS) {
        free(srv->pps_nal);
        srv->pps_nal = malloc(len);
        memcpy(srv->pps_nal, nal, len);
        srv->pps_len = len;
    }
}

/* ---- Build SDP response ---- */
static int build_sdp(char *buf, size_t buf_size, rtsp_server_t *srv, uint32_t ssrc)
{
    uint32_t ntp = (uint32_t)(esp_timer_get_time() / 1000000ULL + 2208988800ULL);
    int off = snprintf(buf, buf_size, sdp_template, ntp, ntp);

    /* Append sprop-parameter-sets (base64 of SPS+PPS) */
    if (srv->sps_nal && srv->pps_nal) {
        /* Simple hex encoding for sprop */
        for (size_t i = 0; i < srv->sps_len && off < (int)buf_size - 5; i++) {
            off += snprintf(buf + off, buf_size - off, "%02X", srv->sps_nal[i]);
        }
        off += snprintf(buf + off, buf_size - off, ",");
        for (size_t i = 0; i < srv->pps_len && off < (int)buf_size - 5; i++) {
            off += snprintf(buf + off, buf_size - off, "%02X", srv->pps_nal[i]);
        }
    }
    off += snprintf(buf + off, buf_size - off, "\r\n");
    off += snprintf(buf + off, buf_size - off, "a=control:trackID=0\r\n");
    return off;
}

/* ---- RTSP response helper ---- */
static int rtsp_respond(int sock, int cseq, const char *extra)
{
    char buf[2048];
    int len = snprintf(buf, sizeof(buf),
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %d\r\n"
        "%s"
        "\r\n", cseq, extra ? extra : "");
    return send(sock, buf, len, 0);
}

/* ---- RTSP task —— one client at a time (simple) ---- */
static void rtsp_task(void *arg)
{
    rtsp_server_t *srv = (rtsp_server_t *)arg;
    ESP_LOGI(TAG, "RTSP server listening on port %d", srv->cfg.port);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(srv->cfg.port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    srv->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(srv->listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(srv->listen_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv->listen_sock, 2);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int csock = accept(srv->listen_sock, (struct sockaddr *)&client_addr, &clen);
        if (csock < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        ESP_LOGI(TAG, "RTSP client connected");
        rtsp_client_t *c = NULL;
        for (int i = 0; i < srv->cfg.max_clients; i++) {
            if (srv->clients[i].state == RTSP_STATE_INIT) {
                c = &srv->clients[i];
                c->sock = csock;
                c->rtp_sock = -1;
                c->state = RTSP_STATE_READY;
                c->ssrc = (uint32_t)(esp_random() & 0xFFFFFFFF);
                c->seq  = (uint16_t)(esp_random() & 0xFFFF);
                c->timestamp = 0;
                snprintf(c->session, sizeof(c->session), "%08"PRIX32, (uint32_t)esp_random());
                break;
            }
        }
        if (!c) { close(csock); continue; }

        char buf[2048];
        int cseq = 0;
        while (1) {
            int n = recv(csock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';

            /* Parse CSeq */
            char *p = strstr(buf, "CSeq:");
            if (p) cseq = atoi(p + 5);

            if (strncmp(buf, "OPTIONS", 7) == 0) {
                rtsp_respond(csock, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");
            }
            else if (strncmp(buf, "DESCRIBE", 8) == 0) {
                char sdp[2048];
                int sdplen = build_sdp(sdp, sizeof(sdp), srv, c->ssrc);
                /* Get actual STA IP for Content-Base (fallback to client addr) */
                char rtsp_ip[16] = "0.0.0.0";
                esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
                        esp_ip4addr_ntoa(&ip_info.ip, rtsp_ip, sizeof(rtsp_ip));
                    }
                }
                char extra[256];
                snprintf(extra, sizeof(extra),
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: %d\r\n"
                    "Content-Base: rtsp://%s:%d/stream\r\n",
                    sdplen, rtsp_ip, srv->cfg.port);
                rtsp_respond(csock, cseq, extra);
                send(csock, sdp, sdplen, 0);
            }
            else if (strncmp(buf, "SETUP", 5) == 0) {
                char *tp = strstr(buf, "Transport:");
                uint16_t rtp_port = 0;
                if (tp) {
                    char *pp = strstr(tp, "client_port=");
                    if (pp) {
                        rtp_port = (uint16_t)atoi(pp + 12);
                    }
                }
                c->rtp_addr = client_addr;
                c->rtp_addr.sin_port = htons(rtp_port);
                c->rtp_port = rtp_port;

                /* Create UDP socket for RTP */
                c->rtp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (c->rtp_sock >= 0) {
                    int bufsize = 512 * 1024;
                    setsockopt(c->rtp_sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
                } else {
                    ESP_LOGE(TAG, "Failed to create RTP UDP socket");
                }

                char extra[512];
                snprintf(extra, sizeof(extra),
                    "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=0-0\r\n"
                    "Session: %s\r\n",
                    rtp_port, rtp_port + 1, c->session);
                rtsp_respond(csock, cseq, extra);
            }
            else if (strncmp(buf, "PLAY", 4) == 0) {
                c->state = RTSP_STATE_PLAYING;
                char extra[256];
                snprintf(extra, sizeof(extra), "Session: %s\r\nRange: npt=0.000-\r\n", c->session);
                rtsp_respond(csock, cseq, extra);
                ESP_LOGI(TAG, "Client PLAYING, RTP→%u", c->rtp_port);
            }
            else if (strncmp(buf, "TEARDOWN", 8) == 0) {
                char extra[128];
                snprintf(extra, sizeof(extra), "Session: %s\r\n", c->session);
                rtsp_respond(csock, cseq, extra);
                break;
            }
        }
        close(csock);
        if (c->rtp_sock >= 0) { close(c->rtp_sock); c->rtp_sock = -1; }
        c->state = RTSP_STATE_INIT;
        ESP_LOGI(TAG, "RTSP client disconnected");
    }
}

/* ---- Public API ---- */
esp_err_t rtsp_server_init(rtsp_server_t **out, const rtsp_config_t *cfg)
{
    rtsp_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return ESP_ERR_NO_MEM;
    srv->cfg = *cfg;
    xTaskCreate(rtsp_task, "rtsp_srv", 8192, srv, 5, NULL);
    *out = srv;
    return ESP_OK;
}

esp_err_t rtsp_server_deinit(rtsp_server_t *srv)
{
    if (!srv) return ESP_ERR_INVALID_ARG;
    if (srv->task) vTaskDelete(srv->task);
    close(srv->listen_sock);
    free(srv->sps_nal);
    free(srv->pps_nal);
    free(srv);
    return ESP_OK;
}

esp_err_t rtsp_server_send_video(rtsp_server_t *srv, const uint8_t *h264_data, size_t len, bool is_keyframe)
{
    if (!srv) return ESP_ERR_INVALID_ARG;

    /* Parse NAL units from H264 stream (with start codes) */
    size_t consumed = 0;
    while (consumed < len) {
        size_t adv = 0;
        const uint8_t *nal_start = find_start_code(h264_data + consumed, len - consumed, &adv);
        if (adv == 0) break;
        consumed += adv;
        if (!nal_start) break;

        /* Find end of this NAL (next start code or end of data) */
        size_t remaining = len - consumed;
        size_t nal_len = 0;
        size_t next = 0;
        const uint8_t *next_start = find_start_code(h264_data + consumed, remaining, &next);
        if (next_start) {
            nal_len = (size_t)(next_start - nal_start);
        } else {
            nal_len = remaining + adv;
        }

        /* Store SPS/PPS */
        store_sps_pps(srv, nal_start, nal_len);

        /* Send to all playing clients */
        uint32_t ts = (uint32_t)(esp_timer_get_time() * 90 / 1000); /* 90kHz clock */
        for (int i = 0; i < srv->cfg.max_clients; i++) {
            if (srv->clients[i].state == RTSP_STATE_PLAYING) {
                rtp_send_nal(&srv->clients[i], nal_start, nal_len, ts);
            }
        }
    }
    return ESP_OK;
}

int rtsp_server_client_count(rtsp_server_t *srv)
{
    if (!srv) return 0;
    int count = 0;
    for (int i = 0; i < srv->cfg.max_clients; i++) {
        if (srv->clients[i].state == RTSP_STATE_PLAYING) count++;
    }
    return count;
}

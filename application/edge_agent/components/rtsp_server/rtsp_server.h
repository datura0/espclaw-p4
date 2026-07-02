/*
 * Minimal RTSP + RTP/H264 server for ESP32-P4
 * 
 * Usage:
 *   rtsp_server_t *srv = rtsp_server_init(8554);
 *   rtsp_server_send_video(srv, h264_data, len, is_keyframe);
 *   rtsp_server_deinit(srv);
 *
 * Clients connect via: rtsp://<ip>:8554/stream
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtsp_server rtsp_server_t;

typedef struct {
    uint16_t    port;
    uint8_t     max_clients;
    uint16_t    mtu;
} rtsp_config_t;

#define RTSP_CONFIG_DEFAULT() { \
    .port = 8554, \
    .max_clients = 2, \
    .mtu = 1400, \
}

esp_err_t     rtsp_server_init(rtsp_server_t **srv, const rtsp_config_t *cfg);
esp_err_t     rtsp_server_deinit(rtsp_server_t *srv);
esp_err_t     rtsp_server_send_video(rtsp_server_t *srv, const uint8_t *h264_nal, size_t len, bool is_keyframe);
int           rtsp_server_client_count(rtsp_server_t *srv);

#ifdef __cplusplus
}
#endif

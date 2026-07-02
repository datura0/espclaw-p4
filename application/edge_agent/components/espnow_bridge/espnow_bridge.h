/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-NOW Bridge — UART link to external ESP32-S3
 *
 * Protocol (length-prefixed frames):
 *   [2-byte payload length, little-endian][N-byte payload]
 *
 * Usage:
 *   1. Call espnow_bridge_init() once during startup.
 *   2. Register a receive callback with espnow_bridge_register_rx_callback().
 *   3. Send frames to S3 with espnow_bridge_send().
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Protocol constants (shared with S3 firmware) ---- */
#define ESPNOW_BRIDGE_CMD_SEND        0x01
#define ESPNOW_BRIDGE_CMD_ADD_PEER    0x02
#define ESPNOW_BRIDGE_CMD_REMOVE_PEER 0x03
#define ESPNOW_BRIDGE_CMD_SET_CHANNEL 0x04
#define ESPNOW_BRIDGE_CMD_RECV        0x81
#define ESPNOW_BRIDGE_CMD_STATUS      0xF0

#define ESPNOW_BRIDGE_STATUS_OK          0x00
#define ESPNOW_BRIDGE_STATUS_ERR_SEND    0x01
#define ESPNOW_BRIDGE_STATUS_ERR_PEER    0x02
#define ESPNOW_BRIDGE_STATUS_ERR_PARSE   0x03
#define ESPNOW_BRIDGE_STATUS_ERR_FRAME   0x04
#define ESPNOW_BRIDGE_STATUS_PEER_FULL   0x05

/**
 * @brief Parsed ESP-NOW receive event from S3 bridge.
 */
typedef struct {
    uint8_t  src_mac[6];    /**< Source MAC address */
    int8_t   rssi;          /**< RSSI in dBm */
    uint8_t *data;          /**< Pointer into original buffer (valid during callback) */
    uint16_t data_len;      /**< Length of payload data */
} espnow_bridge_recv_t;

/**
 * @brief Parsed status response from S3 bridge.
 */
typedef struct {
    uint8_t code;           /**< Status code (STATUS_OK, STATUS_ERR_SEND, etc.) */
} espnow_bridge_status_t;

/**
 * @brief Callback type for parsed events from the S3 bridge.
 *
 * @param type    Event type: ESPNOW_BRIDGE_CMD_RECV or ESPNOW_BRIDGE_CMD_STATUS.
 * @param recv    Valid when type == ESPNOW_BRIDGE_CMD_RECV (NULL otherwise).
 * @param status  Valid when type == ESPNOW_BRIDGE_CMD_STATUS (NULL otherwise).
 * @param user_ctx  User context pointer passed during registration.
 */
typedef void (*espnow_bridge_event_cb_t)(uint8_t type,
                                         const espnow_bridge_recv_t *recv,
                                         const espnow_bridge_status_t *status,
                                         void *user_ctx);

/**
 * @brief Initialize the UART bridge to the external ESP32-S3.
 *
 * Configures UART pins, baud rate, and starts the RX task.
 * Must be called once after NVS / basic hardware is ready.
 *
 * @return ESP_OK on success.
 */
esp_err_t espnow_bridge_init(void);

/**
 * @brief Register a callback for parsed ESP-NOW events from the S3.
 *
 * Only one callback is supported at a time.  Call with cb=NULL to unregister.
 *
 * @param cb        Event callback (or NULL to unregister).
 * @param user_ctx  Opaque pointer passed back to the callback.
 */
void espnow_bridge_register_event_callback(espnow_bridge_event_cb_t cb,
                                           void *user_ctx);

/**
 * @brief Send raw bytes to the S3 bridge over UART.
 *
 * @param data  Payload buffer.
 * @param len   Payload length (must be <= CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE).
 * @return ESP_OK on success.
 */
esp_err_t espnow_bridge_send_raw(const uint8_t *data, size_t len);

/**
 * @brief Send an ESP-NOW message via the S3 bridge.
 *
 * Constructs a CMD_SEND payload and transmits it over UART.
 *
 * @param dst_mac  6-byte destination MAC address.
 * @param data     Payload to send over ESP-NOW.
 * @param len      Payload length (max ESP-NOW limit minus header overhead).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if len too large.
 */
esp_err_t espnow_bridge_send_espnow(const uint8_t dst_mac[6],
                                    const uint8_t *data, size_t len);

/**
 * @brief Add an ESP-NOW peer on the S3 bridge.
 *
 * @param peer_mac  6-byte peer MAC address.
 * @return ESP_OK on success.
 */
esp_err_t espnow_bridge_add_peer(const uint8_t peer_mac[6]);

/**
 * @brief Remove an ESP-NOW peer on the S3 bridge.
 *
 * @param peer_mac  6-byte peer MAC address.
 * @return ESP_OK on success.
 */
esp_err_t espnow_bridge_remove_peer(const uint8_t peer_mac[6]);

/**
 * @brief Set the ESP-NOW channel on the S3 bridge.
 *
 * @param channel  Wi-Fi channel (1-14).
 * @return ESP_OK on success.
 */
esp_err_t espnow_bridge_set_channel(uint8_t channel);

#ifdef __cplusplus
}
#endif

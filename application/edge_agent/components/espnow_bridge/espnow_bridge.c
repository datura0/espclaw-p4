/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-NOW Bridge — UART link to external ESP32-S3
 *
 * Frame protocol:
 *   [2-byte payload length, little-endian][N-byte payload]
 *
 * The RX state machine reads the 2-byte length header, then waits for the
 * complete payload before dispatching it via the registered callback.
 */

#include "espnow_bridge.h"

#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "espnow_br";

/* ---- RX state machine states ---- */
typedef enum {
    BR_STATE_IDLE,   /* Waiting for the 2-byte length header */
    BR_STATE_DATA,   /* Accumulating payload bytes */
} bridge_rx_state_t;

/* ---- Module state ---- */
static espnow_bridge_event_cb_t s_event_cb;
static void                   *s_event_cb_ctx;
static TaskHandle_t            s_rx_task;

/* ---- Parsed event dispatcher ---- */
static void bridge_dispatch_payload(const uint8_t *payload, uint16_t len)
{
    if (!s_event_cb || len < 1) return;

    uint8_t cmd = payload[0];
    const uint8_t *data = payload + 1;
    uint16_t dlen = len - 1;

    if (cmd == ESPNOW_BRIDGE_CMD_RECV) {
        /* RECV: MAC(6) + RSSI(1) + data(remaining) */
        if (dlen < 7) return;
        espnow_bridge_recv_t recv;
        memcpy(recv.src_mac, data, 6);
        recv.rssi = (int8_t)data[6];
        recv.data = (uint8_t *)(data + 7);  /* mutable cast; caller treats as const */
        recv.data_len = dlen - 7;
        s_event_cb(ESPNOW_BRIDGE_CMD_RECV, &recv, NULL, s_event_cb_ctx);
    } else if (cmd == ESPNOW_BRIDGE_CMD_STATUS) {
        /* STATUS: code(1) */
        if (dlen < 1) return;
        espnow_bridge_status_t status = { .code = data[0] };
        s_event_cb(ESPNOW_BRIDGE_CMD_STATUS, NULL, &status, s_event_cb_ctx);
    }
}

/* ---- Helper: ensure the UART driver is installed exactly once ---- */
static esp_err_t bridge_uart_init(void)
{
    static bool installed = false;
    if (installed) {
        return ESP_OK;
    }

    const uart_config_t uart_cfg = {
        .baud_rate  = CONFIG_ESPNOW_BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(CONFIG_ESPNOW_BRIDGE_UART_PORT,
                                      &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(CONFIG_ESPNOW_BRIDGE_UART_PORT,
                       CONFIG_ESPNOW_BRIDGE_UART_TX_PIN,
                       CONFIG_ESPNOW_BRIDGE_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(CONFIG_ESPNOW_BRIDGE_UART_PORT,
                              CONFIG_ESPNOW_BRIDGE_UART_RX_BUFFER_SIZE,
                              0,    /* TX buffer — not used (we send synchronously) */
                              0,    /* no event queue */
                              NULL, /* no event queue handle */
                              0);   /* no interrupt allocation flags */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    installed = true;
    ESP_LOGI(TAG, "UART%d init OK  TX=GPIO%d RX=GPIO%d  baud=%d",
             CONFIG_ESPNOW_BRIDGE_UART_PORT,
             CONFIG_ESPNOW_BRIDGE_UART_TX_PIN,
             CONFIG_ESPNOW_BRIDGE_UART_RX_PIN,
             CONFIG_ESPNOW_BRIDGE_UART_BAUD);
    return ESP_OK;
}

/* ---- RX task: frame reader ---- */
static void bridge_rx_task(void *arg)
{
    (void)arg;

    bridge_rx_state_t state = BR_STATE_IDLE;
    /* Buffer large enough for worst-case frame: header + max payload */
    uint8_t  frame[sizeof(uint16_t) + CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE];
    uint16_t payload_len = 0;
    uint16_t bytes_needed = sizeof(uint16_t); /* first thing to read: header */
    uint16_t buf_offset = 0;

    while (1) {
        int read_len = uart_read_bytes(CONFIG_ESPNOW_BRIDGE_UART_PORT,
                                       frame + buf_offset,
                                       bytes_needed,
                                       pdMS_TO_TICKS(100));
        if (read_len <= 0) {
            continue; /* timeout, try again */
        }

        buf_offset   += (uint16_t)read_len;
        bytes_needed -= (uint16_t)read_len;

        if (bytes_needed > 0) {
            continue; /* Need more bytes for the current stage */
        }

        /* ---- Stage complete ---- */
        if (state == BR_STATE_IDLE) {
            /* Finished reading the 2-byte length header */
            payload_len = frame[0] | ((uint16_t)frame[1] << 8);

            if (payload_len == 0 || payload_len > CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE) {
                ESP_LOGW(TAG, "Bad frame len=%u, byte-level resync", payload_len);
                /* Byte-level resync: discard only 1 byte, slide the 2nd byte
                 * into position 0 as the potential first byte of the real header.
                 * This guarantees recovery even for fixed-size frames where
                 * 2-byte-at-a-time discard would loop forever. */
                frame[0]     = frame[1];
                state        = BR_STATE_IDLE;
                buf_offset   = 1;    /* already have 1 byte of the next candidate header */
                bytes_needed = 1;    /* need 1 more to complete a 2-byte header */
                continue;
            }

            /* Prepare to read the payload */
            state        = BR_STATE_DATA;
            bytes_needed = payload_len;
            /* buf_offset stays; payload will be written starting at frame[2] */
            continue;
        }

        /* BR_STATE_DATA: complete payload received */
        bridge_dispatch_payload(frame + sizeof(uint16_t), payload_len);

        /* Reset for next frame */
        state        = BR_STATE_IDLE;
        buf_offset   = 0;
        bytes_needed = sizeof(uint16_t);
    }
}

/* ---- Public API ---- */

esp_err_t espnow_bridge_init(void)
{
    esp_err_t err = bridge_uart_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ret = xTaskCreate(bridge_rx_task,
                                 "espnow_rx",
                                 CONFIG_ESPNOW_BRIDGE_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_ESPNOW_BRIDGE_TASK_PRIORITY,
                                 &s_rx_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Bridge RX task started");
    return ESP_OK;
}

void espnow_bridge_register_event_callback(espnow_bridge_event_cb_t cb,
                                           void *user_ctx)
{
    s_event_cb     = cb;
    s_event_cb_ctx = user_ctx;
}

/* ---- Convenience: build a CMD payload and send ---- */
static esp_err_t bridge_send_cmd(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    size_t total = 1 + data_len;
    if (total > CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE];
    buf[0] = cmd;
    if (data_len) memcpy(buf + 1, data, data_len);

    uint8_t header[2];
    header[0] = (uint8_t)(total & 0xFF);
    header[1] = (uint8_t)((total >> 8) & 0xFF);

    int sent = uart_write_bytes(CONFIG_ESPNOW_BRIDGE_UART_PORT, header, 2);
    if (sent != 2) return ESP_FAIL;
    sent = uart_write_bytes(CONFIG_ESPNOW_BRIDGE_UART_PORT, buf, total);
    if (sent != (int)total) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t espnow_bridge_send_raw(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build the framed packet: 2-byte length header + payload */
    uint8_t header[2];
    header[0] = (uint8_t)(len & 0xFF);
    header[1] = (uint8_t)((len >> 8) & 0xFF);

    int sent = uart_write_bytes(CONFIG_ESPNOW_BRIDGE_UART_PORT,
                                header, sizeof(header));
    if (sent != sizeof(header)) {
        ESP_LOGE(TAG, "Header send failed: %d/%d", sent, (int)sizeof(header));
        return ESP_FAIL;
    }

    sent = uart_write_bytes(CONFIG_ESPNOW_BRIDGE_UART_PORT, data, len);
    if (sent != (int)len) {
        ESP_LOGE(TAG, "Payload send failed: %d/%d", sent, (int)len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t espnow_bridge_send_espnow(const uint8_t dst_mac[6],
                                    const uint8_t *data, size_t len)
{
    /* Build CMD_SEND payload: MAC(6) + data */
    uint8_t payload[CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE];
    payload[0] = ESPNOW_BRIDGE_CMD_SEND;
    memcpy(payload + 1, dst_mac, 6);
    if (len) memcpy(payload + 7, data, len);
    return bridge_send_cmd(ESPNOW_BRIDGE_CMD_SEND, payload + 1, 6 + len);
}

esp_err_t espnow_bridge_add_peer(const uint8_t peer_mac[6])
{
    return bridge_send_cmd(ESPNOW_BRIDGE_CMD_ADD_PEER, peer_mac, 6);
}

esp_err_t espnow_bridge_remove_peer(const uint8_t peer_mac[6])
{
    return bridge_send_cmd(ESPNOW_BRIDGE_CMD_REMOVE_PEER, peer_mac, 6);
}

esp_err_t espnow_bridge_set_channel(uint8_t channel)
{
    return bridge_send_cmd(ESPNOW_BRIDGE_CMD_SET_CHANNEL, &channel, 1);
}

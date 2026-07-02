/*
 * ESP-NOW Slave Device Firmware (s3test / 从机1)
 *
 * Listens for ESP-NOW commands and executes them.
 *
 * Protocol (payload inside ESP-NOW data):
 *   Request:  [CMD(1B)][params...]
 *   Response: [CMD(1B)|0x80][status(1B)][data...]
 *
 * Built-in commands:
 *   CMD_ECHO     0x01  — 回显：收到什么回什么 (用于连通性测试)
 *   CMD_PING     0x02  — 心跳：返回 MAC + uptime
 *   CMD_GPIO_WR  0x10  — 写 GPIO: pin(1B) + level(1B)
 *   CMD_GPIO_RD  0x11  — 读 GPIO: pin(1B) → pin(1B) + level(1B)
 *   CMD_INFO     0x20  — 设备信息：返回 MAC + 固件版本
 *
 * Add custom commands in slave_handle_cmd() below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "slave";

#define ESPNOW_CHANNEL    1

/* ---- Command IDs ---- */
#define CMD_ECHO     0x01
#define CMD_PING     0x02
#define CMD_GPIO_WR  0x10
#define CMD_GPIO_RD  0x11
#define CMD_INFO     0x20

#define RESP_MASK    0x80    /* OR with cmd to mark as response */

/* Status codes for responses */
#define STATUS_OK    0x00
#define STATUS_ERR   0x01

/* ---- Firmware version ---- */
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0

/* ================================================================
 *  ESP-NOW callbacks
 * ================================================================ */

static void slave_send_resp(const uint8_t *dst_mac, uint8_t cmd,
                            const uint8_t *data, uint8_t len)
{
    uint8_t buf[250];
    buf[0] = cmd | RESP_MASK;
    buf[1] = STATUS_OK;
    if (data && len) memcpy(buf + 2, data, len);
    uint8_t total = 2 + (data ? len : 0);

    esp_err_t err = esp_now_send(dst_mac, buf, total);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Send resp failed: %d", err);
    }
}

static void slave_send_err(const uint8_t *dst_mac, uint8_t cmd)
{
    uint8_t buf[2] = { cmd | RESP_MASK, STATUS_ERR };
    esp_now_send(dst_mac, buf, 2);
}

/* ---- Command handlers ---- */

static void slave_handle_cmd(const uint8_t *src_mac,
                             const uint8_t *data, uint8_t len)
{
    if (len < 1) return;
    uint8_t cmd = data[0];
    const uint8_t *params = data + 1;
    uint8_t plen = len - 1;

    ESP_LOGI(TAG, "CMD 0x%02x from %02x:%02x:%02x:%02x:%02x:%02x len=%u",
             cmd, src_mac[0],src_mac[1],src_mac[2],
             src_mac[3],src_mac[4],src_mac[5], len);

    switch (cmd) {

    /* ---- Echo: reply with same data ---- */
    case CMD_ECHO:
        slave_send_resp(src_mac, CMD_ECHO, params, plen);
        break;

    /* ---- Ping: reply with MAC + uptime ---- */
    case CMD_PING: {
        uint8_t my_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, my_mac);
        int64_t uptime_us = esp_timer_get_time();
        uint8_t resp[14];
        memcpy(resp, my_mac, 6);
        resp[6]  = (uptime_us >>  0) & 0xFF;
        resp[7]  = (uptime_us >>  8) & 0xFF;
        resp[8]  = (uptime_us >> 16) & 0xFF;
        resp[9]  = (uptime_us >> 24) & 0xFF;
        resp[10] = (uptime_us >> 32) & 0xFF;
        resp[11] = (uptime_us >> 40) & 0xFF;
        resp[12] = (uptime_us >> 48) & 0xFF;
        resp[13] = (uptime_us >> 56) & 0xFF;
        slave_send_resp(src_mac, CMD_PING, resp, sizeof(resp));
        break;
    }

    /* ---- GPIO Write: pin(1B) + level(1B) ---- */
    case CMD_GPIO_WR: {
        if (plen < 2) { slave_send_err(src_mac, CMD_GPIO_WR); return; }
        uint8_t pin = params[0];
        uint8_t level = params[1] ? 1 : 0;
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, level);
        ESP_LOGI(TAG, "GPIO%d = %d", pin, level);
        slave_send_resp(src_mac, CMD_GPIO_WR, NULL, 0);
        break;
    }

    /* ---- GPIO Read: pin(1B) → pin(1B) + level(1B) ---- */
    case CMD_GPIO_RD: {
        if (plen < 1) { slave_send_err(src_mac, CMD_GPIO_RD); return; }
        uint8_t pin = params[0];
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        uint8_t level = gpio_get_level(pin);
        uint8_t resp[2] = { pin, level };
        slave_send_resp(src_mac, CMD_GPIO_RD, resp, sizeof(resp));
        break;
    }

    /* ---- Device Info: MAC + firmware version ---- */
    case CMD_INFO: {
        uint8_t my_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, my_mac);
        uint8_t resp[8];
        memcpy(resp, my_mac, 6);
        resp[6] = FW_VERSION_MAJOR;
        resp[7] = FW_VERSION_MINOR;
        slave_send_resp(src_mac, CMD_INFO, resp, sizeof(resp));
        break;
    }

    /* ---- Unknown: don't reply (avoids noise) ---- */
    default:
        ESP_LOGW(TAG, "Unknown CMD 0x%02x", cmd);
        break;
    }
}

/* ================================================================
 *  ESP-NOW receive callback
 * ================================================================ */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (len <= 0) return;
    slave_handle_cmd(info->src_addr, data, (uint8_t)len);
}

/* ================================================================
 *  Init
 * ================================================================ */

static void wifi_init(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

static void espnow_init(void)
{
    esp_now_init();
    esp_now_register_recv_cb(espnow_recv_cb);

    /* Add broadcast as peer so we can send responses to anyone */
    esp_now_peer_info_t peer = {0};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

/* ================================================================
 *  Main
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-NOW Slave v%d.%d ===", FW_VERSION_MAJOR, FW_VERSION_MINOR);

    wifi_init();
    espnow_init();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Slave MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    ESP_LOGI(TAG, "Waiting for ESP-NOW commands on channel %d...", ESPNOW_CHANNEL);

    /* All work is done in espnow_recv_cb */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

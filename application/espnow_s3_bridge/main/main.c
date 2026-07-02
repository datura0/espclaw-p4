/* ================================================================
 *  ESP-NOW UART Bridge — ESP32-S3 固件
 * ================================================================
 * 功能: 在 ESP-NOW 无线消息 与 UART 串口之间做桥接
 *       使 ESP32-P4 主机能通过串口命令控制 ESP-NOW 从机设备
 *
 * 硬件接线:
 *   S3 GPIO18 (TX) → P4 IO30 (RX)
 *   S3 GPIO17 (RX) → P4 IO31 (TX)
 *   S3 GND         → P4 GND
 *
 * 系统架构:
 *   ┌──────────┐  UART (921600-8N1)  ┌──────────┐  ESP-NOW   ┌──────────┐
 *   │ ESP32-P4 │◄──────────────────►│ ESP32-S3 │◄─────────►│  从机们   │
 *   │  (主机)   │                    │ (桥接器) │  Ch.1      │          │
 *   └──────────┘                    └──────────┘            └──────────┘
 *
 * UART 帧协议（与 P4 espnow_bridge 组件一致）:
 *   [2字节 payload 长度(小端序)][N字节 payload]
 *   最大 payload: 250 字节
 *
 * Payload 命令协议（帧内的数据部分）:
 *   Byte 0 = 命令字
 *   Bytes 1..N-1 = 命令参数
 *
 *   P4→S3 命令:
 *     CMD 0x01 — SEND:         MAC(6B) + 数据(剩余字节)
 *     CMD 0x02 — ADD_PEER:     MAC(6B)
 *     CMD 0x03 — REMOVE_PEER:  MAC(6B)
 *     CMD 0x04 — SET_CHANNEL:  信道(1B)
 *
 *   S3→P4 事件:
 *     CMD 0x81 — RECV:         MAC(6B) + RSSI(1B) + 数据
 *     CMD 0xF0 — STATUS:       状态码(1B)
 *       状态码: 0x00=成功  0x01=发送失败  0x02=Peer错误
 *               0x03=解析错误  0x04=帧错误  0x05=Peer表满
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "nvs_flash.h"

/* ================================================================
 *  硬件 / 协议配置宏
 * ================================================================ */
#define UART_NUM          UART_NUM_1    // 使用 UART1（UART0 留给系统日志）
#define UART_TX_PIN       18            // TX → P4 IO30 (RX)
#define UART_RX_PIN       17            // RX ← P4 IO31 (TX)
#define UART_BAUD         921600        // 波特率（与 P4 espnow_bridge 一致）
#define UART_BUF_SIZE     (2048)        // UART 收发缓冲区大小
#define MAX_FRAME_SIZE    250           // 单帧最大 payload（与 P4 约定一致）

#define ESPNOW_MAX_PEERS  10            // ESP-NOW 最多可注册的 peer 数量
#define ESPNOW_SEND_LEN   (MAX_FRAME_SIZE - 7)          // 单次发送最大数据量 = 250-1-6 = 243 字节

static const char *TAG = "espnow_br";   // 日志标签
static uint8_t s_espnow_channel = CONFIG_ESPNOW_BRIDGE_CHANNEL;  // 运行时信道（可被 CMD_SET_CHANNEL 更新）

/* ================================================================
 *  命令字定义
 * ================================================================ */
/* P4→S3 命令 */
#define CMD_SEND          0x01         // 发送数据到从机
#define CMD_ADD_PEER      0x02         // 注册从机 peer
#define CMD_REMOVE_PEER   0x03         // 移除从机 peer
#define CMD_SET_CHANNEL   0x04         // 切换 Wi-Fi 信道

/* S3→P4 事件 */
#define CMD_RECV          0x81         // 收到从机消息（CMD | 0x80 表示响应）
#define CMD_STATUS        0xF0         // 操作结果状态

/* 状态码 */
#define STATUS_OK          0x00         // 成功
#define STATUS_ERR_SEND    0x01         // ESP-NOW 发送失败
#define STATUS_ERR_PEER    0x02         // Peer 操作失败
#define STATUS_ERR_PARSE   0x03         // 命令解析错误
#define STATUS_ERR_FRAME   0x04         // 帧格式错误（保留）
#define STATUS_PEER_FULL   0x05         // Peer 表已满（最多 10 个）

/* ================================================================
 *  UART 发送工具函数
 * ================================================================ */

/* ── 发送一帧数据给 P4 ──
 * 帧格式: [2字节长度(小端)][N字节payload]
 * 这是与 P4 通信的基础封装，所有 S3→P4 的数据都通过此函数发出 */
static void bridge_send_frame(const uint8_t *payload, uint16_t len)
{
    uint8_t header[2];
    header[0] = len & 0xFF;         // 长度低字节
    header[1] = (len >> 8) & 0xFF;  // 长度高字节
    uart_write_bytes(UART_NUM, header, 2);   // 先发 2 字节帧头
    uart_write_bytes(UART_NUM, payload, len); // 再发 payload 数据
}

/* ── 向 P4 发送操作结果状态 ──
 * payload: [CMD_STATUS(0xF0)][状态码(1B)]
 * 所有命令执行完毕后都通过此函数反馈结果 */
static void bridge_send_status(uint8_t code)
{
    uint8_t payload[2] = { CMD_STATUS, code };
    bridge_send_frame(payload, sizeof(payload));
}

/* ── 向 P4 转发从机的 ESP-NOW 消息 ──
 * 将从机发来的原始数据封装为 RECV 事件
 * payload: [CMD_RECV(0x81)][源MAC(6B)][信号强度RSSI(1B)][数据(NB)] */
static void bridge_send_recv(const uint8_t *mac, int8_t rssi,
                             const uint8_t *data, uint16_t dlen)
{
    uint16_t plen = 1 + 6 + 1 + dlen;          // 总长度
    if (plen > MAX_FRAME_SIZE) {
        dlen = MAX_FRAME_SIZE - 8;              // 超出则截断数据
        plen = MAX_FRAME_SIZE;
    }
    uint8_t payload[MAX_FRAME_SIZE];
    payload[0] = CMD_RECV;                      // 事件类型
    memcpy(payload + 1, mac, 6);                // 源 MAC
    payload[7] = (uint8_t)rssi;                 // 信号强度（负数表示信号好）
    if (dlen) memcpy(payload + 8, data, dlen);  // 数据内容
    bridge_send_frame(payload, plen);
}

/* ================================================================
 *  ESP-NOW 回调函数
 * ================================================================ */

/* ── 接收回调：从机发来 ESP-NOW 消息时，ESP-NOW 协议栈自动调用 ── */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (len < 0) len = 0;
    if (len > (int)(MAX_FRAME_SIZE - 8)) len = MAX_FRAME_SIZE - 8;
    /* 将从机消息封装为 RECV 事件（0x81），通过 UART 转发给 P4 */
    bridge_send_recv(info->src_addr, info->rx_ctrl->rssi, data, (uint16_t)len);
}

/* ── 发送完成回调：esp_now_send() 的异步结果在此返回 ── */
static void espnow_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    /* 将发送成功/失败转换为 STATUS 事件（0xF0）通知 P4 */
    bridge_send_status(status == ESP_NOW_SEND_SUCCESS
                       ? STATUS_OK : STATUS_ERR_SEND);
}

/* ================================================================
 *  命令分发器 — 解析 P4 发来的 payload 并执行对应操作
 * ================================================================
 * 这是 Bridge 的核心函数，所有 P4→S3 的命令都在此处理
 * payload 格式: [命令字(1B)][参数(NB)] */
static void process_payload(const uint8_t *payload, uint16_t len)
{
    if (len < 1) return;
    uint8_t cmd = payload[0];          // 第 0 字节 = 命令字
    const uint8_t *data = payload + 1; // 后续字节 = 命令参数
    uint16_t dlen = len - 1;           // 参数长度

    switch (cmd) {
    case CMD_SEND: {
        /* P4 发来 SEND 命令，要求向指定从机发送数据
         * payload 格式: [CMD(1B)=0x01][目标MAC(6B)][要发送的数据(NB)]
         */
        if (dlen < 6) { bridge_send_status(STATUS_ERR_PARSE); return; }
        const uint8_t *mac       = data;          // 目标从机的 MAC 地址
        const uint8_t *send_data = data + 6;      // 实际要发送的数据部分
        uint16_t send_len = dlen - 6;             // 数据长度
        if (send_len > ESPNOW_SEND_LEN) send_len = ESPNOW_SEND_LEN;  // 截断超长数据

        /* 智能 peer 管理：先 mod 再 add，避免重复添加填满 peer 表 */
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = s_espnow_channel;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t peer_err = esp_now_mod_peer(&peer);
        if (peer_err == ESP_ERR_ESPNOW_NOT_FOUND) {
            /* Peer 不在表中，尝试添加 */
            peer_err = esp_now_add_peer(&peer);
            if (peer_err == ESP_ERR_ESPNOW_FULL) {
                /* 表满了，随便删一个旧 peer 腾空间（保留自己需要的） */
                esp_now_del_peer(mac);  /* 尝试删除自己 */
                peer_err = esp_now_add_peer(&peer);
            }
        }

        /* ★ 核心操作：通过 ESP-NOW 无线发送数据给从机 */
        esp_err_t err = esp_now_send(mac, send_data, send_len);

        if (err != ESP_OK) {
            /* 发送入队失败（常见原因：信道不匹配、peer 表满等） */
            ESP_LOGW(TAG, "esp_now_send err=%d", err);
            bridge_send_status(STATUS_ERR_SEND);   // 立即反馈失败
        }
        /* 注意：成功时不在此处回复 P4
         * 真正的发送 ACK 由 espnow_send_cb 异步回调返回 STATUS_OK */
        break;
    }
    /* ── 手动注册从机 peer ── */
    case CMD_ADD_PEER: {
        /* payload: [CMD(1B)=0x02][MAC(6B)] */
        if (dlen < 6) { bridge_send_status(STATUS_ERR_PARSE); return; }
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, data, 6);
        peer.channel = s_espnow_channel;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err == ESP_ERR_ESPNOW_EXIST) {
            /* peer 已在表中，改用 mod_peer 更新信道等信息 */
            err = esp_now_mod_peer(&peer);
        }
        uint8_t code = (err == ESP_OK) ? STATUS_OK : STATUS_ERR_PEER;
        if (err == ESP_ERR_ESPNOW_FULL) code = STATUS_PEER_FULL;  // 最多 10 个 peer
        bridge_send_status(code);
        ESP_LOGI(TAG, "Add peer %02x:%02x:%02x:%02x:%02x:%02x -> %d",
                 data[0],data[1],data[2],data[3],data[4],data[5], code);
        break;
    }

    /* ── 移除从机 peer ── */
    case CMD_REMOVE_PEER: {
        /* payload: [CMD(1B)=0x03][MAC(6B)] */
        if (dlen < 6) { bridge_send_status(STATUS_ERR_PARSE); return; }
        esp_err_t err = esp_now_del_peer(data);
        bridge_send_status((err == ESP_OK) ? STATUS_OK : STATUS_ERR_PEER);
        break;
    }

    /* ── 切换 Wi-Fi 信道 ── */
    case CMD_SET_CHANNEL: {
        /* payload: [CMD(1B)=0x04][信道(1B)]  合法范围: 1~14 */
        if (dlen < 1) { bridge_send_status(STATUS_ERR_PARSE); return; }
        uint8_t ch = data[0];
        if (ch < 1 || ch > 14) { bridge_send_status(STATUS_ERR_PARSE); return; }
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_espnow_channel = ch;  /* update runtime tracking */
        ESP_LOGI(TAG, "Channel switched to %d", ch);
        /* Update all existing peers to the new channel */
        esp_now_peer_info_t p;
        for (bool from_head = true; esp_now_fetch_peer(from_head, &p) == ESP_OK; from_head = false) {
            p.channel = ch;
            esp_now_mod_peer(&p);
        }
        bridge_send_status(STATUS_OK);
        break;
    }
    default:
        bridge_send_status(STATUS_ERR_PARSE);
        break;
    }
}

/* ================================================================
 *  UART 接收任务 — FreeRTOS 后台线程，逐字节解析 P4 发来的帧
 * ================================================================
 * 使用三态状态机解析变长帧:
 *   S_LEN_LO → 读长度低字节
 *   S_LEN_HI → 读长度高字节，拼出完整长度并校验
 *   S_DATA   → 读 payload 数据，完成后调用 process_payload 处理
 * 遇到非法长度时自动重置状态机，重新同步 */

/* 接收状态机状态 */
typedef enum { S_LEN_LO, S_LEN_HI, S_DATA } rx_state_t;

static void uart_rx_task(void *arg)
{
    (void)arg;
    rx_state_t state = S_LEN_LO;                 // 初始状态：等待长度低字节
    uint8_t  frame[MAX_FRAME_SIZE + 2];           // 帧缓冲区（帧头 + 最大payload）
    uint16_t payload_len = 0;                     // 解析出的 payload 长度
    uint16_t idx = 0;                             // 当前写入位置
    uint16_t need = 1;                            // 还需读取的字节数

    while (1) {
        uint8_t byte;
        /* 阻塞读取 1 字节，超时 200ms 后重试 */
        int n = uart_read_bytes(UART_NUM, &byte, 1, pdMS_TO_TICKS(200));
        if (n != 1) continue;                    // 超时或无数据，继续等待

        frame[idx++] = byte;
        need--;

        if (need > 0) continue;                  // 该状态还没收完，继续读

        /* ── 当前状态已完成，执行状态转换 ── */
        switch (state) {
        case S_LEN_LO:
            payload_len = byte;                  // 记录长度低字节
            state = S_LEN_HI;                    // 转去读长度高字节
            need = 1;
            break;
        case S_LEN_HI:
            payload_len |= ((uint16_t)byte << 8); // 拼出完整长度
            if (payload_len == 0 || payload_len > MAX_FRAME_SIZE) {
                /* 长度非法，重置状态机重新同步 */
                ESP_LOGW(TAG, "Bad len=%u, re-sync", payload_len);
                state = S_LEN_LO;
                idx = 0; need = 1;
                break;
            }
            state = S_DATA;                      // 转去读 payload
            need = payload_len;
            idx = 0;                             // 复用 frame 缓冲区存放 payload
            break;
        case S_DATA:
            /* payload 收完，交给命令分发器处理 */
            process_payload(frame, payload_len);
            state = S_LEN_LO;                    // 回到初始状态，准备下一帧
            idx = 0; need = 1;
            break;
        }
    }
}

/* ================================================================
 *  初始化函数
 * ================================================================ */

/* ── Wi-Fi 初始化 ──
 * 步骤: NVS → 网络接口 → 事件循环 → Wi-Fi 驱动 → STA 模式 → 锁定信道
 * ESP-NOW 基于 Wi-Fi 协议栈，但不连接 AP，仅在 STA 模式下使用 raw 802.11 帧 */
static void wifi_init(void)
{
    nvs_flash_init();                              // 初始化 NVS 闪存（Wi-Fi 校准数据需要）
    esp_netif_init();                              // 初始化 TCP/IP 网络栈
    esp_event_loop_create_default();               // 创建默认事件循环
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);                           // 初始化 Wi-Fi 驱动
    esp_wifi_set_storage(WIFI_STORAGE_RAM);        // Wi-Fi 配置存 RAM（不写 NVS，更快）
    esp_wifi_set_mode(WIFI_MODE_STA);              // STA 模式（ESP-NOW 要求）
    esp_wifi_start();                              // 启动 Wi-Fi
    esp_wifi_set_channel(s_espnow_channel, WIFI_SECOND_CHAN_NONE); // 锁定到指定信道
    ESP_LOGI(TAG, "Wi-Fi STA started, channel=%d", s_espnow_channel);
}

/* ── ESP-NOW 协议初始化 ──
 * 注册两个核心回调:
 *   espnow_recv_cb — 收到从机消息时触发
 *   espnow_send_cb — 发送完成（ACK/失败）时触发 */
static void espnow_init(void)
{
    esp_now_init();                                // 启动 ESP-NOW 协议栈
    esp_now_register_recv_cb(espnow_recv_cb);      // 注册接收回调
    esp_now_register_send_cb(espnow_send_cb);      // 注册发送完成回调
    ESP_LOGI(TAG, "ESP-NOW init OK");
}

/* ── UART 初始化 ──
 * 配置 UART1: 921600bps, 8N1, 无流控
 * TX=GPIO18 → P4 IO30(RX), RX=GPIO17 ← P4 IO31(TX) */
static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,                    // 921600 bps
        .data_bits = UART_DATA_8_BITS,             // 8 位数据
        .parity    = UART_PARITY_DISABLE,          // 无校验
        .stop_bits = UART_STOP_BITS_1,             // 1 位停止
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,     // 无硬件流控
        .source_clk = UART_SCLK_DEFAULT,           // 默认时钟源
    };
    uart_param_config(UART_NUM, &cfg);
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, UART_BUF_SIZE, UART_BUF_SIZE,
                        0, NULL, 0);               // 安装驱动，TX/RX 各 2048 字节缓冲区
    ESP_LOGI(TAG, "UART%d: TX=IO%d RX=IO%d @ %d baud",
             UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_BAUD);
}

/* ================================================================
 *  主函数
 * ================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP-NOW Bridge (S3) ===");

    /* 初始化顺序: UART → Wi-Fi → ESP-NOW（顺序不能乱） */
    uart_init();                                   // 1. 串口就绪，日志可输出
    wifi_init();                                   // 2. Wi-Fi 启动，信道锁定
    espnow_init();                                 // 3. ESP-NOW 启动，注册回调

    /* 打印本机 MAC（P4 侧需要知道这个地址以区分多个 Bridge） */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Bridge MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    /* 创建 UART 接收任务，在后台持续监听 P4 发来的命令 */
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);

    /* 主线程空闲 — 所有工作由中断回调 + uart_rx_task 完成 */
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); }
}

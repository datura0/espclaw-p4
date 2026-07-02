/*
 * IMU Angle Sensor — JY61P / JY901 UART Reader
 *
 * JY61P protocol (9600-8-N-1):
 *   Packet: 0x55 [TYPE(1B)] [DATA(8B)] [SUM(1B)]  = 11 bytes
 *   TYPE 0x53 = angle:  RollL RollH PitchL PitchH YawL YawH TL TH
 *   TYPE 0x59 = quaternion: Q0L Q0H Q1L Q1H Q2L Q2H Q3L Q3H
 *
 * Angle conversion: raw / 32768.0f * 180.0f
 * Quaternion: q_float = raw / 32768.0f, then quat→Euler
 */
#include "sensor_imu.h"

#include <inttypes.h>
#include <string.h>
#include <math.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";

#define UART_NUM      UART_NUM_2    /* IO4=TX, IO5=RX */
#define UART_TX_PIN   4
#define UART_RX_PIN   5
#define UART_BAUD     9600
#define UART_BUF_SIZE 1024

/* ---- Shared state ---- */
static imu_attitude_t s_attitude = {0};
static imu_data_cb_t  s_cb;
static void          *s_cb_ctx;
static bool           s_valid = false;

/* ---- Quaternion state (anti-gimbal-lock) ---- */
static float q0=1,q1=0,q2=0,q3=0;
static float prev_r=0,prev_p=0,prev_y=0;
static bool  quat_mode=false;

/* ---- Helpers ---- */
static inline float raw_to_deg(int16_t raw) {
    return (float)raw / 32768.0f * 180.0f;
}
static inline float raw_to_norm(int16_t raw) {
    return (float)raw / 32768.0f;
}

/* Quaternion → Euler (ZYX), with anti-flip */
static void quat_to_euler(imu_attitude_t *att) {
    float a=q0,b=q1,c=q2,d=q3;
    att->roll  = atan2f(2*(a*b+c*d), 1-2*(b*b+c*c)) * 57.29578f;
    att->pitch = asinf(2*(a*c-d*b)) * 57.29578f;
    att->yaw   = atan2f(2*(a*d+b*c), 1-2*(c*c+d*d)) * 57.29578f;
    while(att->roll  - prev_r > 180) att->roll  -= 360;
    while(att->roll  - prev_r < -180) att->roll  += 360;
    while(att->yaw   - prev_y > 180) att->yaw   -= 360;
    while(att->yaw   - prev_y < -180) att->yaw   += 360;
    prev_r=att->roll; prev_p=att->pitch; prev_y=att->yaw;
}

/* Parse an 11-byte JY61P packet */
static bool parse_packet(const uint8_t *pkt, imu_attitude_t *att) {
    uint8_t sum = 0;
    for (int i = 0; i < 10; i++) sum += pkt[i];
    if (sum != pkt[10]) return false;

    if (pkt[1] == 0x59) {  // 四元数包
        q0=raw_to_norm((int16_t)(pkt[2]|(pkt[3]<<8)));
        q1=raw_to_norm((int16_t)(pkt[4]|(pkt[5]<<8)));
        q2=raw_to_norm((int16_t)(pkt[6]|(pkt[7]<<8)));
        q3=raw_to_norm((int16_t)(pkt[8]|(pkt[9]<<8)));
        quat_to_euler(att);
        quat_mode=true;
        return true;
    }
    if (pkt[1] == 0x53) {  // 角度包 (回退)
        if (quat_mode) return false;  // 四元数模式下跳过角度包
        att->roll  = raw_to_deg((int16_t)(pkt[2]|(pkt[3]<<8)));
        att->pitch = raw_to_deg((int16_t)(pkt[4]|(pkt[5]<<8)));
        att->yaw   = raw_to_deg((int16_t)(pkt[6]|(pkt[7]<<8)));
        // 防翻转
        while(att->roll - prev_r > 180) att->roll -= 360;
        while(att->roll - prev_r < -180) att->roll += 360;
        while(att->yaw  - prev_y > 180) att->yaw  -= 360;
        while(att->yaw  - prev_y < -180) att->yaw  += 360;
        prev_r=att->roll; prev_p=att->pitch; prev_y=att->yaw;
        return true;
    }
    return false;
}

/* ---- UART RX task ---- */
static void imu_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[11];
    int idx = 0;
    uint32_t total_bytes = 0;
    uint32_t valid_frames = 0;
    uint32_t bad_checksum = 0;
    uint32_t bad_type = 0;
    uint32_t header_seen = 0;       /* count 0x55 bytes found */
    uint32_t frames_attempted = 0;  /* count idx reaching 11 */
    bool first_frame_logged = false;
    TickType_t last_report = xTaskGetTickCount();

    while (1) {
        uint8_t byte;
        int n = uart_read_bytes(UART_NUM, &byte, 1, pdMS_TO_TICKS(50));
        if (n != 1) {
            /* Periodic status report (every 5s) */
            TickType_t now = xTaskGetTickCount();
            if (now - last_report >= pdMS_TO_TICKS(5000)) {
                last_report = now;
                ESP_LOGI(TAG, "IMU UART stats: bytes=%"PRIu32" hdr=%"PRIu32
                         " frames=%"PRIu32" valid=%"PRIu32
                         " chk=%"PRIu32" type=%"PRIu32" mode=%s",
                         total_bytes, header_seen, frames_attempted,
                         valid_frames, bad_checksum, bad_type,
                         quat_mode?"QUAT":"ANGLE");
                if (valid_frames == 0 && total_bytes == 0) {
                    ESP_LOGW(TAG, "IMU UART: NO DATA received! Check wiring (IO4=TX→IMU RX, IO5=RX→IMU TX) and baud=115200");
                }
            }
            continue;
        }
        total_bytes++;

        /* Raw hex dump of first 20 bytes to diagnose baud/wiring */
        if (total_bytes <= 20) {
            ESP_LOGI(TAG, "raw[%d]=0x%02X", (int)(total_bytes - 1), byte);
        }

        /* Sync to 0x55 header at frame start */
        if (idx == 0) {
            if (byte != 0x55) continue;
            header_seen++;
        }

        buf[idx++] = byte;

        if (idx == 11) {
            idx = 0;
            frames_attempted++;
            imu_attitude_t att;
            if (parse_packet(buf, &att)) {
                s_attitude = att;
                s_valid = true;
                valid_frames++;
                if (!first_frame_logged) {
                    first_frame_logged = true;
                    ESP_LOGI(TAG, "First IMU frame [%s]: roll=%.2f pitch=%.2f yaw=%.2f",
                             quat_mode?"QUAT":"ANGLE",
                             (double)att.roll, (double)att.pitch, (double)att.yaw);
                }
                if (s_cb) s_cb(&att, s_cb_ctx);
            } else {
                /* Diagnose why parse failed */
                uint8_t sum = 0;
                for (int i = 0; i < 10; i++) sum += buf[i];
                if (sum != buf[10]) {
                    bad_checksum++;
                    if (bad_checksum <= 3) {
                        ESP_LOGW(TAG, "Bad checksum: calc=0x%02X got=0x%02X buf=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                                 sum, buf[10],
                                 buf[0],buf[1],buf[2],buf[3],buf[4],
                                 buf[5],buf[6],buf[7],buf[8],buf[9],buf[10]);
                    }
                } else if (buf[1] != 0x53 && buf[1] != 0x59) {
                    bad_type++;
                    if (bad_type <= 3) {
                        ESP_LOGW(TAG, "Unexpected packet type: 0x%02X (expect 0x53)", buf[1]);
                    }
                }
            }
        }
    }
}

/* ---- Public API ---- */

esp_err_t imu_sensor_init(imu_data_cb_t cb, void *user_ctx)
{
    s_cb     = cb;
    s_cb_ctx = user_ctx;
    s_valid  = false;

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreate(imu_rx_task, "imu_rx", 5120, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU RX task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "JY61P IMU init OK: UART%d TX=IO%d RX=IO%d baud=%d",
             UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_BAUD);
    return ESP_OK;
}

esp_err_t imu_sensor_get_attitude(imu_attitude_t *att)
{
    if (!att || !s_valid) return ESP_ERR_NOT_FOUND;
    *att = s_attitude;
    return ESP_OK;
}

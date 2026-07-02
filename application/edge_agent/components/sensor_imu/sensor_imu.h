/*
 * IMU Angle Sensor — JY901 / WT901 UART Reader
 *
 * Reads roll/pitch/yaw from a JY901-series IMU over UART.
 *
 * Wiring:
 *   P4 IO4 (TX) → IMU RX
 *   P4 IO5 (RX) → IMU TX
 *   P4 3.3V     → IMU VCC
 *   P4 GND      → IMU GND
 *
 * Protocol (JY901):
 *   Baud: 115200 (match sensor DIP setting)
 *   Packet: 0x55 0x53 [RollL RollH PitchL PitchH YawL YawH TL TH] SUM
 *   Angle:  signed short / 32768 * 180°  (0.0055° resolution)
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Attitude data in degrees */
typedef struct {
    float roll;    /**< Roll  angle (°) */
    float pitch;   /**< Pitch angle (°) */
    float yaw;     /**< Yaw   angle (°) */
} imu_attitude_t;

/** @brief Callback when new attitude data is available */
typedef void (*imu_data_cb_t)(const imu_attitude_t *att, void *user_ctx);

/**
 * @brief Initialize the IMU sensor UART and start reading.
 *
 * @param cb        Callback invoked with each new attitude sample.
 * @param user_ctx  User pointer passed to callback.
 * @return ESP_OK on success.
 */
esp_err_t imu_sensor_init(imu_data_cb_t cb, void *user_ctx);

/**
 * @brief Get the most recent attitude reading (non-blocking).
 *
 * @param[out] att  Pointer to attitude struct to fill.
 * @return ESP_OK if valid data available, ESP_ERR_NOT_FOUND if no data yet.
 */
esp_err_t imu_sensor_get_attitude(imu_attitude_t *att);

#ifdef __cplusplus
}
#endif

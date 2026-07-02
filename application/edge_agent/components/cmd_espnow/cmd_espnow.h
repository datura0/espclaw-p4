/*
 * ESP-NOW Bridge Console Commands
 *
 * Usage (from P4 serial console):
 *   espnow send <MAC> <hex_data>     Send ESP-NOW message via S3 bridge
 *   espnow peer add <MAC>            Add ESP-NOW peer on S3
 *   espnow peer del <MAC>            Remove ESP-NOW peer on S3
 *
 * Examples:
 *   espnow send ff:ff:ff:ff:ff:ff 68656c6c6f
 *   espnow peer add a0:b1:c2:d3:e4:f5
 */

#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the 'espnow' console command.
 *
 * Call once after the console subsystem is initialized.
 *
 * @return ESP_OK on success.
 */
esp_err_t register_espnow_command(void);

#ifdef __cplusplus
}
#endif

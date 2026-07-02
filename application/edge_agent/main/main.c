/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "claw_ramfs.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wifi_manager.h"
#include "wear_levelling.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
/* Forward declarations for cap_im_wechat (avoiding direct header dependency
   so that main doesn't need cap_im_platform in its REQUIRES list) */
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qr_data_url[256];
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} cap_im_wechat_qr_login_status_t;

esp_err_t cap_im_wechat_qr_login_start(const char *account_id, bool force);
esp_err_t cap_im_wechat_qr_login_get_status(cap_im_wechat_qr_login_status_t *out_status);
esp_err_t cap_im_wechat_qr_login_cancel(void);
esp_err_t cap_im_wechat_qr_login_mark_persisted(void);
#endif
#include "app_config.h"
#include "espnow_bridge.h"
#include "cmd_espnow.h"
#include "sensor_imu.h"
#include "esp_wifi.h"
#include "driver/gpio.h"

#define APP_ENABLE_MEM_LOG        (0)

#define APP_FATFS_PARTITION_LABEL "storage"
#define APP_RAMFS_BASE_PATH       "/ramfs"
#define APP_RAMFS_MAX_FILES       (8)
#define APP_RAMFS_MAX_BYTES       (512 * 1024)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static app_claw_storage_paths_t *s_claw_paths;

static const char *app_fatfs_base_path = "/fatfs";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }
    if (!s_claw_paths) {
        s_claw_paths = calloc(1, sizeof(*s_claw_paths));
    }

    if (!s_config || !s_claw_config || !s_claw_paths) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_paths);
    s_claw_paths = NULL;

    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    /* Disable WiFi power save to prevent AP disconnection (PS_MIN_MODEM sleeps too long) */
    if (connected) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "Wi-Fi power save disabled (WIFI_PS_NONE)");
    }

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_claw_init_storage_paths(app_claw_storage_paths_t *paths)
{
    if (!paths || !app_fatfs_base_path || app_fatfs_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));

    if (strlcpy(paths->fatfs_base_path, app_fatfs_base_path, sizeof(paths->fatfs_base_path)) >= sizeof(paths->fatfs_base_path) ||
        snprintf(paths->memory_session_root, sizeof(paths->memory_session_root), "%s/sessions", app_fatfs_base_path) >= sizeof(paths->memory_session_root) ||
        snprintf(paths->memory_root_dir, sizeof(paths->memory_root_dir), "%s/memory", app_fatfs_base_path) >= sizeof(paths->memory_root_dir) ||
        snprintf(paths->skills_root_dir, sizeof(paths->skills_root_dir), "%s/skills", app_fatfs_base_path) >= sizeof(paths->skills_root_dir) ||
        snprintf(paths->lua_root_dir, sizeof(paths->lua_root_dir), "%s/scripts", app_fatfs_base_path) >= sizeof(paths->lua_root_dir) ||
        snprintf(paths->router_rules_path, sizeof(paths->router_rules_path), "%s/router_rules/router_rules.json", app_fatfs_base_path) >= sizeof(paths->router_rules_path) ||
        snprintf(paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path), "%s/scheduler/schedules.json", app_fatfs_base_path) >= sizeof(paths->scheduler_rules_path) ||
        snprintf(paths->im_attachment_root, sizeof(paths->im_attachment_root), "%s/inbox", app_fatfs_base_path) >= sizeof(paths->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_config_validate_wifi(config, NULL);
    if (err != ESP_OK) {
        return err;
    }

    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    cap_im_wechat_qr_login_status_t *raw = NULL;
    esp_err_t err;

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    raw = calloc(1, sizeof(*raw));
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_qr_login_get_status(raw);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));
    free(raw);
    return ESP_OK;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(app_fatfs_base_path,
                                           APP_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(app_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_ramfs(void)
{
    claw_ramfs_config_t config = {
        .base_path = APP_RAMFS_BASE_PATH,
        .max_files = APP_RAMFS_MAX_FILES,
        .max_bytes = APP_RAMFS_MAX_BYTES,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    esp_err_t err = claw_ramfs_register(&config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount RAMFS at %s: %s", APP_RAMFS_BASE_PATH, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RAMFS mounted at %s max_files=%u max_bytes=%u",
             APP_RAMFS_BASE_PATH,
             (unsigned int)APP_RAMFS_MAX_FILES,
             (unsigned int)APP_RAMFS_MAX_BYTES);

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

/* ---- ESP-NOW Bridge (S3 via UART) event callback ---- */
static void on_espnow_bridge_event(uint8_t type,
                                   const espnow_bridge_recv_t *recv,
                                   const espnow_bridge_status_t *status,
                                   void *user_ctx)
{
    (void)user_ctx;
    static uint32_t last_recv=0,last_stat=0;
    static uint8_t  last_code=0xFF;
    uint32_t now=xTaskGetTickCount()*portTICK_PERIOD_MS;
    if (type == ESPNOW_BRIDGE_CMD_RECV && recv) {
        if(now-last_recv>=500){
            last_recv=now;
            ESP_LOGI(TAG, "ESP-NOW recv from %02x:%02x:%02x:%02x:%02x:%02x, rssi=%d, len=%u",
                     recv->src_mac[0],recv->src_mac[1],recv->src_mac[2],
                     recv->src_mac[3],recv->src_mac[4],recv->src_mac[5],
                     (int)recv->rssi,(unsigned)recv->data_len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, recv->data, recv->data_len, ESP_LOG_INFO);
        }
    } else if (type == ESPNOW_BRIDGE_CMD_STATUS && status) {
        if(status->code!=last_code||now-last_stat>=30000){
            last_stat=now;last_code=status->code;
            ESP_LOGI(TAG, "ESP-NOW bridge status: 0x%02x", status->code);
        }
    }
}

/* ---- Button & IMU Lock ---- */
#define BTN_LOCK_GPIO      GPIO_NUM_20   /* press to lock current IMU attitude */
#define MOTOR_CMD_POSITION 0x40          /* motor control: 1B cmd + 1B mode + 12B angle_diffs */

static bool           s_locked = false;
static imu_attitude_t s_lock_ref;        /* reference attitude at lock moment */

/* ---- IMU Attitude Dispatch Task ---- */

#define IMU_CMD_ATTITUDE  0x30   /* roll(4B) + pitch(4B) + yaw(4B) */

static uint8_t s_roll_mac[6], s_pitch_mac[6], s_yaw_mac[6];
static const uint8_t s_remote_mac[6] = {0x44,0x1b,0xf6,0xd7,0x99,0x04};  // 遥控器

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_mac_string(const char *s, uint8_t mac[6])
{
    int idx = 0;
    for (int i = 0; s[i] && idx < 6; i++) {
        if (s[i] == ':' || s[i] == '-') continue;
        int hi = hex_digit(s[i]);
        if (hi < 0 || !s[i+1]) return false;
        int lo = hex_digit(s[i+1]);
        if (lo < 0) return false;
        mac[idx++] = (uint8_t)((hi << 4) | lo);
        i++;
    }
    return (idx == 6);
}

static void imu_dispatch_task(void *arg)
{
    (void)arg;

    /* Parse three slave MACs */
    if (!parse_mac_string(CONFIG_MOTOR_ROLL_MAC, s_roll_mac)) {
        ESP_LOGE(TAG, "Invalid ROLL_MAC, using broadcast");
        memset(s_roll_mac, 0xFF, 6);
    }
    if (!parse_mac_string(CONFIG_MOTOR_PITCH_MAC, s_pitch_mac)) {
        ESP_LOGE(TAG, "Invalid PITCH_MAC, using broadcast");
        memset(s_pitch_mac, 0xFF, 6);
    }
    if (!parse_mac_string(CONFIG_MOTOR_YAW_MAC, s_yaw_mac)) {
        ESP_LOGE(TAG, "Invalid YAW_MAC, using broadcast");
        memset(s_yaw_mac, 0xFF, 6);
    }
    ESP_LOGI(TAG, "IMU dispatch every %d ms to 3 motors", CONFIG_IMU_SEND_INTERVAL_MS);

    while (1) {
        imu_attitude_t att;
        esp_err_t err = imu_sensor_get_attitude(&att);
        if (err == ESP_OK) {
            if (s_locked) {
                /* Per-axis: send single-axis command to each motor */
                float roll_diff  = att.roll  - s_lock_ref.roll;
                float pitch_diff = att.pitch - s_lock_ref.pitch;
                float yaw_diff   = att.yaw   - s_lock_ref.yaw;
                if (yaw_diff >  180.0f) yaw_diff -= 360.0f;
                if (yaw_diff < -180.0f) yaw_diff += 360.0f;

                uint8_t pkt[7];
                pkt[0] = 0x41;      /* MOTOR_CMD_AXIS */
                pkt[2] = 0x01;      /* position mode */

                pkt[1] = 0;         /* x = roll */
                memcpy(pkt + 3, &roll_diff, 4);
                espnow_bridge_send_espnow(s_roll_mac, pkt, sizeof(pkt));

                pkt[1] = 1;         /* y = pitch */
                memcpy(pkt + 3, &pitch_diff, 4);
                espnow_bridge_send_espnow(s_pitch_mac, pkt, sizeof(pkt));

                pkt[1] = 2;         /* z = yaw */
                memcpy(pkt + 3, &yaw_diff, 4);
                espnow_bridge_send_espnow(s_yaw_mac, pkt, sizeof(pkt));
                /* Log every 20th packet to confirm diffs are changing */
                static int cnt = 0;
                if (++cnt % 20 == 0) {
                    ESP_LOGI(TAG, "MOTOR→ yaw=%+.2f° roll=%+.2f° pitch=%+.2f°",
                             (double)yaw_diff, (double)roll_diff, (double)pitch_diff);
                }
            } else {
                /* IMU attitude to all three motors */
                uint8_t pkt[13];
                pkt[0] = IMU_CMD_ATTITUDE;
                memcpy(pkt + 1, &att.roll, 4);
                memcpy(pkt + 5, &att.pitch, 4);
                memcpy(pkt + 9, &att.yaw, 4);
                espnow_bridge_send_espnow(s_roll_mac, pkt, sizeof(pkt));
                espnow_bridge_send_espnow(s_pitch_mac, pkt, sizeof(pkt));
                espnow_bridge_send_espnow(s_yaw_mac, pkt, sizeof(pkt));
                espnow_bridge_send_espnow(s_remote_mac, pkt, sizeof(pkt));  // 遥控器
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_IMU_SEND_INTERVAL_MS));
    }
}

/* ---- Button Lock Task: monitor IO20, lock IMU reference on press ---- */
static void btn_lock_task(void *arg)
{
    (void)arg;
    gpio_set_direction(BTN_LOCK_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_LOCK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_21, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "IO20=lock / IO21=unlock");

    bool last20 = true, last21 = true;
    while (1) {
        bool now20 = gpio_get_level(BTN_LOCK_GPIO);
        bool now21 = gpio_get_level(GPIO_NUM_21);
        if (last20 && !now20) {  /* IO20 press: lock */
            vTaskDelay(pdMS_TO_TICKS(30));
            if (!gpio_get_level(BTN_LOCK_GPIO)) {
                imu_attitude_t att;
                if (imu_sensor_get_attitude(&att) == ESP_OK) {
                    s_lock_ref = att;
                    s_locked = true;
                    ESP_LOGI(TAG, "*** LOCKED: roll=%.2f pitch=%.2f yaw=%.2f ***",
                             (double)att.roll, (double)att.pitch, (double)att.yaw);
                } else {
                    ESP_LOGW(TAG, "Lock failed: IMU not ready");
                }
            }
        }
        if (last21 && !now21) {  /* IO21 press: unlock */
            vTaskDelay(pdMS_TO_TICKS(30));
            if (!gpio_get_level(GPIO_NUM_21)) {
                s_locked = false;
                ESP_LOGI(TAG, "*** UNLOCKED — back to speed mode ***");
            }
        }
        last20 = now20;
        last21 = now21;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- IMU data callback (runs from UART RX task, keep it fast) ---- */
static void on_imu_data(const imu_attitude_t *att, void *user_ctx)
{
    (void)user_ctx;
    static uint32_t last_log=0;
    uint32_t now=xTaskGetTickCount()*portTICK_PERIOD_MS;
    if(now-last_log>=2000){
        last_log=now;
        ESP_LOGI(TAG,"IMU: R=%.1f P=%.1f Y=%.1f",(double)att->roll,(double)att->pitch,(double)att->yaw);
    }
}

/* ---- Lua motor control module (registered via cap_lua) ---- */
#include "cap_lua.h"
#include "lauxlib.h"

static int lua_motor_axis(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    const char *ax      = luaL_checkstring(L, 2);
    lua_Number deg      = luaL_checknumber(L, 3);
    uint8_t mac[6]; int idx = 0;
    for (int i = 0; mac_str[i] && idx < 6; i++) {
        if (mac_str[i] == ':' || mac_str[i] == '-') continue;
        int hi, lo;
        if (mac_str[i] >= '0' && mac_str[i] <= '9') hi = mac_str[i] - '0';
        else if (mac_str[i] >= 'a' && mac_str[i] <= 'f') hi = mac_str[i] - 'a' + 10;
        else if (mac_str[i] >= 'A' && mac_str[i] <= 'F') hi = mac_str[i] - 'A' + 10;
        else continue;
        if (!mac_str[i+1]) break;
        if (mac_str[i+1] >= '0' && mac_str[i+1] <= '9') lo = mac_str[i+1] - '0';
        else if (mac_str[i+1] >= 'a' && mac_str[i+1] <= 'f') lo = mac_str[i+1] - 'a' + 10;
        else if (mac_str[i+1] >= 'A' && mac_str[i+1] <= 'F') lo = mac_str[i+1] - 'A' + 10;
        else break;
        mac[idx++] = (uint8_t)((hi << 4) | lo); i++;
    }
    uint8_t axis = 2;
    char c = ax[0]; if (c >= 'A' && c <= 'Z') c += 32;
    if (c == 'x') axis = 0; else if (c == 'y') axis = 1;
    float val = (float)deg;
    uint8_t pkt[7] = {0x41, axis, 0x01};
    memcpy(pkt + 3, &val, 4);
    lua_pushboolean(L, espnow_bridge_send_espnow(mac, pkt, 7) == ESP_OK);
    return 1;
}
static int lua_motor_send3(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    float r = (float)luaL_checknumber(L, 2);
    float p = (float)luaL_checknumber(L, 3);
    float y = (float)luaL_checknumber(L, 4);
    uint8_t mac[6]; int idx = 0;
    for (int i = 0; mac_str[i] && idx < 6; i++) {
        if (mac_str[i] == ':' || mac_str[i] == '-') continue;
        int hi, lo;
        if (mac_str[i] >= '0' && mac_str[i] <= '9') hi = mac_str[i] - '0';
        else if (mac_str[i] >= 'a' && mac_str[i] <= 'f') hi = mac_str[i] - 'a' + 10;
        else if (mac_str[i] >= 'A' && mac_str[i] <= 'F') hi = mac_str[i] - 'A' + 10;
        else continue;
        if (!mac_str[i+1]) break;
        if (mac_str[i+1] >= '0' && mac_str[i+1] <= '9') lo = mac_str[i+1] - '0';
        else if (mac_str[i+1] >= 'a' && mac_str[i+1] <= 'f') lo = mac_str[i+1] - 'a' + 10;
        else if (mac_str[i+1] >= 'A' && mac_str[i+1] <= 'F') lo = mac_str[i+1] - 'A' + 10;
        else break;
        mac[idx++] = (uint8_t)((hi << 4) | lo); i++;
    }
    uint8_t pkt[14] = {0x40, 0x01};
    memcpy(pkt + 2,  &r, 4);
    memcpy(pkt + 6,  &p, 4);
    memcpy(pkt + 10, &y, 4);
    lua_pushboolean(L, espnow_bridge_send_espnow(mac, pkt, 14) == ESP_OK);
    return 1;
}
int luaopen_espnow_motor(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_motor_axis);  lua_setfield(L, -2, "axis");
    lua_pushcfunction(L, lua_motor_send3); lua_setfield(L, -2, "send3");
    return 1;
}

/* ---- MJPEG Camera stream ---- */
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "face_detect.h"
#include <errno.h>

#define CAM_W 800
#define CAM_H 800

/* ---- Minimal 3x5 bitmap font for digits 0-9 (3 cols, 5 rows, MSB=left) ---- */
static const uint8_t s_font_3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, /* 0 */
    {0b010, 0b110, 0b010, 0b010, 0b111}, /* 1 */
    {0b111, 0b001, 0b111, 0b100, 0b111}, /* 2 */
    {0b111, 0b001, 0b111, 0b001, 0b111}, /* 3 */
    {0b101, 0b101, 0b111, 0b001, 0b001}, /* 4 */
    {0b111, 0b100, 0b111, 0b001, 0b111}, /* 5 */
    {0b111, 0b100, 0b111, 0b101, 0b111}, /* 6 */
    {0b111, 0b001, 0b001, 0b001, 0b001}, /* 7 */
    {0b111, 0b101, 0b111, 0b101, 0b111}, /* 8 */
    {0b111, 0b101, 0b111, 0b001, 0b111}, /* 9 */
};

/* Draw FPS digits onto UYVY buffer at top-left.
   UYVY: 4 bytes per 2 pixels = [U, Y0, V, Y1].
   White text (Y=235, U=V=128) on black bg (Y=16, U=V=128). */
static void draw_fps_on_frame(uint8_t *uyvy, int w, int h, int fps)
{
    const int scale = 3;
    const int cw = 3 * scale;
    const int ch = 5 * scale;
    const int x0 = 4, y0 = 4;

    /* FPS color: green (>25) / yellow (15-25) / red (<15) — Y-only, U/V neutral */
    uint8_t fg_y;
    if (fps >= 25)      fg_y = 220;  /* bright green-ish luma */
    else if (fps >= 15) fg_y = 180;  /* yellow-ish luma */
    else                 fg_y = 100;  /* red/dark luma */

    char buf[4];
    int nd = snprintf(buf, sizeof(buf), "%d", fps);
    int total_w = nd * (cw + scale);

    /* Black background rectangle */
    for (int py = y0 - 2; py < y0 + ch + 2 && py < h; py++) {
        for (int px = x0 - 2; px < x0 + total_w + 2 && px < w; px += 2) {
            uint8_t *p = uyvy + (py * w + px) * 2;
            p[0] = 128; p[1] = 16;   /* U=128, Y0=16 (black) */
            p[2] = 128; p[3] = 16;   /* V=128, Y1=16 (black) */
        }
    }

    /* Draw each digit */
    for (int d = 0; d < nd; d++) {
        int digit = buf[d] - '0';
        if (digit < 0 || digit > 9) continue;
        const uint8_t *rows = s_font_3x5[digit];
        for (int row = 0; row < 5; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < 3; col++) {
                if (!(bits & (1 << (2 - col)))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x0 + d * (cw + scale) + col * scale + sx;
                        int py = y0 + row * scale + sy;
                        if (px < w && py < h) {
                            uint8_t *p = uyvy + (py * w + px) * 2;
                            if (px & 1) {
                                /* odd pixel: update Y1, keep U/V from neighbor */
                                p[-1] = 128; p[0] = fg_y;  /* V=128, Y1=fg */
                            } else {
                                /* even pixel: update U, Y0 */
                                p[0] = 128; p[1] = fg_y;  /* U=128, Y0=fg */
                            }
                        }
                    }
                }
            }
        }
    }
}

static uint8_t *s_jpg_buf = NULL;  /* camera's private encode buffer */
static uint32_t s_jpg_size = 0;
static uint32_t s_jpg_max = 0;
static uint32_t s_jpg_seq = 0;
static SemaphoreHandle_t s_jpg_mutex = NULL;
static uint8_t *s_jpg_send = NULL;  /* shared: camera copies here under mutex, handler sends from here */

static void camera_capture_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "MJPEG: starting");

    int cam_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (cam_fd < 0) { ESP_LOGE(TAG, "Camera not found: %s (errno=%d)", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno); goto cleanup; }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ioctl(cam_fd, VIDIOC_G_FMT, &fmt);
    fmt.fmt.pix.width = CAM_W; fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;  /* ISP native YUV422, no conversion needed for JPEG */
    ioctl(cam_fd, VIDIOC_S_FMT, &fmt);
    ESP_LOGI(TAG, "Camera: %ux%u pixfmt=0x%X", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

    /* Set framerate to 50fps */
    struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(cam_fd, VIDIOC_G_PARM, &parm) == 0) {
        ESP_LOGI(TAG, "Camera default fps: %lu/%lu",
            (unsigned long)parm.parm.capture.timeperframe.denominator,
            (unsigned long)parm.parm.capture.timeperframe.numerator);
    }
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 25;  /* match WiFi bandwidth */
    if (ioctl(cam_fd, VIDIOC_S_PARM, &parm) == 0) {
        ESP_LOGI(TAG, "Camera framerate set to 25fps");
    } else {
        ESP_LOGW(TAG, "Camera framerate set failed (errno=%d)", errno);
    }

    /* Brightness & exposure tweaks (best-effort, ignore failures) */
    struct v4l2_control ctrl;
    ctrl.id = V4L2_CID_BRIGHTNESS;
    ctrl.value = 60;  /* bump brightness (default ~0, range -128..127 or 0..255) */
    if (ioctl(cam_fd, VIDIOC_S_CTRL, &ctrl) == 0) ESP_LOGI(TAG, "Brightness: %d", ctrl.value);

    ctrl.id = V4L2_CID_CONTRAST;
    ctrl.value = 55;  /* slight contrast boost (default ~32-64, range depends on sensor) */
    if (ioctl(cam_fd, VIDIOC_S_CTRL, &ctrl) == 0) ESP_LOGI(TAG, "Contrast: %d", ctrl.value);

    ctrl.id = V4L2_CID_SATURATION;
    ctrl.value = 60;  /* boost saturation */
    if (ioctl(cam_fd, VIDIOC_S_CTRL, &ctrl) == 0) ESP_LOGI(TAG, "Saturation: %d", ctrl.value);

    /* Enable auto white balance */
    ctrl.id = V4L2_CID_AUTO_WHITE_BALANCE;
    ctrl.value = 1;
    ioctl(cam_fd, VIDIOC_S_CTRL, &ctrl);

    /* Auto exposure: 0=manual, 1=auto, 2=shutter-priority, 3=aperture-priority */
    ctrl.id = V4L2_CID_EXPOSURE_AUTO;
    ctrl.value = V4L2_EXPOSURE_AUTO;  /* auto exposure */
    ioctl(cam_fd, VIDIOC_S_CTRL, &ctrl);

    struct v4l2_requestbuffers req = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .count = 6 };
    ioctl(cam_fd, VIDIOC_REQBUFS, &req);
    uint8_t *cam_bufs[6];
    for (int i = 0; i < 6; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        ioctl(cam_fd, VIDIOC_QUERYBUF, &b);
        cam_bufs[i] = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, cam_fd, b.m.offset);
        ioctl(cam_fd, VIDIOC_QBUF, &b);
    }
    { int t = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(cam_fd, VIDIOC_STREAMON, &t); }
    ESP_LOGI(TAG, "Camera streaming started");

    /* Initialize face detection (best-effort, works only if esp-who is installed) */
    face_detect_init(CAM_W, CAM_H);

    /* ---- ESP32-P4 Hardware JPEG Encoder ---- */
    jpeg_encoder_handle_t jpeg = NULL;
    jpeg_encode_engine_cfg_t eng_cfg = { .timeout_ms = 5000 };
    esp_err_t jret = jpeg_new_encoder_engine(&eng_cfg, &jpeg);
    if (jret != ESP_OK) {
        ESP_LOGE(TAG, "HW JPEG encoder engine failed: %s", esp_err_to_name(jret));
        goto cleanup;
    }

    jpeg_encode_cfg_t jcfg = {
        .width = CAM_W, .height = CAM_H,
        .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 30,  /* ~20KB JPEG → send-avg ~30ms, matches 25fps */
    };

    /* Allocate HW-aligned output buffer */
    size_t jpg_out_sz = 0;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint8_t *jpg_hw_buf = (uint8_t *)jpeg_alloc_encoder_mem(s_jpg_max, &mem_cfg, &jpg_out_sz);
    if (!jpg_hw_buf) {
        ESP_LOGE(TAG, "HW JPEG output buffer alloc failed");
        jpeg_del_encoder_engine(jpeg);
        goto cleanup;
    }
    ESP_LOGI(TAG, "HW JPEG encoder OK, out_buf=%zu bytes", jpg_out_sz);

    uint32_t fps_count = 0;    /* resets every 2s for FPS calc */
    uint32_t total_frames = 0; /* monotonic, for heartbeat log */
    int64_t fps_start_us = esp_timer_get_time();
    int s_fps = 0;  /* smoothed FPS for overlay */

    /* Timing diagnostics */
    int64_t last_dqbuf_us = 0;
    int64_t total_dqbuf_wait_us = 0;
    int64_t total_encode_us = 0;

    while (1) {
        struct v4l2_buffer vbuf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };

        if (ioctl(cam_fd, VIDIOC_DQBUF, &vbuf) != 0) { vTaskDelay(1); continue; }
        int64_t t1 = esp_timer_get_time();

        /* FPS: calculate every 2 seconds */
        fps_count++;
        total_frames++;
        int64_t now_us = t1;
        int64_t elapsed_us = now_us - fps_start_us;
        if (elapsed_us >= 2000000LL) {
            s_fps = (int)(fps_count * 1000000LL / elapsed_us);
            /* Print timing breakdown */
            if (fps_count > 0) {
                ESP_LOGI(TAG, "FPS:%d  DQBUF-wait-avg:%lldus  encode-avg:%lldus  jpg-sz:%d",
                    s_fps,
                    (long long)(total_dqbuf_wait_us / fps_count),
                    (long long)(total_encode_us / fps_count),
                    (int)s_jpg_size);
            }
            fps_count = 0;
            fps_start_us = now_us;
            total_dqbuf_wait_us = 0;
            total_encode_us = 0;
        }

        /* DQBUF wait time (time since last DQBUF return ≈ frame interval) */
        if (last_dqbuf_us > 0) {
            total_dqbuf_wait_us += (t1 - last_dqbuf_us);
        }
        last_dqbuf_us = t1;

        /* Draw FPS overlay on UYVY buffer before JPEG encoding */
        draw_fps_on_frame(cam_bufs[vbuf.index], CAM_W, CAM_H, s_fps);

        /* Face detection: run every 5th frame, draw boxes on current frame */
        {
            static face_box_t s_faces[FACE_DETECT_MAX_FACES];
            static int s_face_count = 0;
            if (total_frames % 10 == 0) {
                s_face_count = face_detect_run(cam_bufs[vbuf.index], CAM_W, CAM_H,
                                               s_faces, FACE_DETECT_MAX_FACES);
            }
            if (s_face_count > 0) {
                face_draw_boxes(cam_bufs[vbuf.index], CAM_W, CAM_H,
                                s_faces, s_face_count);
            }
        }

        /* HW JPEG encode: cam_bufs → jpg_hw_buf (HW-aligned output) */
        uint32_t sz32 = 0;
        int64_t te0 = esp_timer_get_time();
        esp_err_t jr = jpeg_encoder_process(jpeg, &jcfg,
            cam_bufs[vbuf.index], CAM_W * CAM_H * 2,
            jpg_hw_buf, (uint32_t)jpg_out_sz, &sz32);
        int64_t te1 = esp_timer_get_time();
        total_encode_us += (te1 - te0);
        int sz = (int)sz32;

        ioctl(cam_fd, VIDIOC_QBUF, &vbuf);

        if (jr == ESP_OK && sz > 0 && sz < (int)s_jpg_max) {
            xSemaphoreTake(s_jpg_mutex, portMAX_DELAY);
            memcpy(s_jpg_buf, jpg_hw_buf, (size_t)sz);  /* publish to s_jpg_buf (shared) */
            s_jpg_size = (uint32_t)sz;
            s_jpg_seq++;
            xSemaphoreGive(s_jpg_mutex);
        }
        if (total_frames % 50 == 0) {
            ESP_LOGI(TAG, "Cam: %lu frames, last jpg=%d ret=%d", (unsigned long)total_frames, sz, (int)jr);
        }
        vTaskDelay(1);
    }

    free(jpg_hw_buf);
    jpeg_del_encoder_engine(jpeg);
    close(cam_fd);
cleanup:
    ESP_LOGW(TAG, "MJPEG task exiting");
    vTaskDelete(NULL);
}

/* HTTP handler: /mjpeg — copy under mutex, send without mutex (camera never blocked) */
static esp_err_t mjpeg_handler(httpd_req_t *req)
{
    esp_err_t res;

    /* Set Content-Type for MJPEG multipart stream */
    res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "MJPEG: httpd_resp_set_type failed: %s", esp_err_to_name(res));
        return res;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (!s_jpg_send) {
        ESP_LOGE(TAG, "MJPEG: s_jpg_send is NULL");
        return ESP_FAIL;
    }

    char part[160];
    const char *boundary_marker = "\r\n--frame\r\n";
    int blen = (int)strlen(boundary_marker);
    int hl;

    /* Wait for first frame, copy to s_jpg_send under mutex */
    uint32_t last_seq = 0;
    uint32_t sz = 0;
    while (1) {
        xSemaphoreTake(s_jpg_mutex, portMAX_DELAY);
        if (s_jpg_seq != last_seq && s_jpg_size > 0 && s_jpg_size < s_jpg_max) {
            last_seq = s_jpg_seq;
            sz = s_jpg_size;
            memcpy(s_jpg_send, s_jpg_buf, (size_t)sz);  /* copy to local send buffer */
        }
        xSemaphoreGive(s_jpg_mutex);
        if (sz > 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Send first frame (no mutex held — camera can publish next frame) */
    hl = snprintf(part, sizeof(part), "Content-Type: image/jpeg\r\nContent-Length: %"PRIu32"\r\n\r\n", sz);
    res = httpd_resp_send_chunk(req, boundary_marker, blen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hl);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)s_jpg_send, (int)sz);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "MJPEG: first frame send failed");
        return res;
    }
    ESP_LOGI(TAG, "MJPEG stream started (sz=%"PRIu32" seq=%"PRIu32")", sz, last_seq);

    uint32_t skip_count = 0;
    uint32_t send_count = 0;
    int64_t send_total_us = 0;
    while (1) {
        /* Wait for new frame, atomically copy to s_jpg_send */
        uint32_t cur_seq = 0;
        sz = 0;
        while (1) {
            xSemaphoreTake(s_jpg_mutex, portMAX_DELAY);
            if (s_jpg_seq != last_seq && s_jpg_size > 0 && s_jpg_size < s_jpg_max) {
                cur_seq = s_jpg_seq;
                sz = s_jpg_size;
                memcpy(s_jpg_send, s_jpg_buf, (size_t)sz);
            }
            xSemaphoreGive(s_jpg_mutex);
            if (sz > 0) break;
            vTaskDelay(pdMS_TO_TICKS(5));  /* tighter poll for lower latency */
        }

        /* Detect skipped frames */
        if (cur_seq != last_seq + 1) {
            skip_count += (cur_seq - last_seq - 1);
        }

        /* Send WITHOUT mutex */
        int64_t ts0 = esp_timer_get_time();
        hl = snprintf(part, sizeof(part), "Content-Type: image/jpeg\r\nContent-Length: %"PRIu32"\r\n\r\n", sz);
        res = httpd_resp_send_chunk(req, boundary_marker, blen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hl);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)s_jpg_send, (int)sz);
        int64_t ts1 = esp_timer_get_time();
        send_total_us += (ts1 - ts0);
        send_count++;

        if (res != ESP_OK) {
            ESP_LOGI(TAG, "MJPEG client disconnected (sent=%"PRIu32" skipped=%"PRIu32")", send_count, skip_count);
            break;
        }

        /* Periodic stats: every ~2s log send-avg and skip rate */
        if (send_count % 50 == 0) {
            ESP_LOGI(TAG, "MJPEG: sent=%"PRIu32" skip=%"PRIu32" send-avg=%lldus",
                send_count, skip_count, (long long)(send_total_us / send_count));
        }

        last_seq = cur_seq;
    }

    return ESP_OK;
}

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(espnow_bridge_init());
    espnow_bridge_register_event_callback(on_espnow_bridge_event, NULL);
    ESP_ERROR_CHECK(imu_sensor_init(on_imu_data, NULL));
    xTaskCreate(imu_dispatch_task, "imu_disp", 3072, NULL, 5, NULL);
    xTaskCreate(btn_lock_task, "btn_lock", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(init_ramfs());
    if (wifi_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed (C6 offline?), skipping WiFi...");
        goto skip_wifi;
    }
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fatfs_base_path,
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            if (wifi_manager_wait_connected(30000) == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);

                /* Auto-sync Bridge ESP-NOW channel to match WiFi AP,
                 * so s3_test (which follows AP channel) stays reachable
                 * even when the hotspot changes channel after reboot. */
                uint8_t wifi_ch;
                wifi_second_chan_t wifi_sc;
                if (esp_wifi_get_channel(&wifi_ch, &wifi_sc) == ESP_OK) {
                    ESP_LOGI(TAG, "Syncing ESP-NOW bridge to Wi-Fi channel %d", wifi_ch);
                    espnow_bridge_set_channel(wifi_ch);
                }
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);

        }
    }

skip_wifi:
    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));
    esp_err_t err = app_claw_start(s_claw_config, s_claw_paths);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "app_claw_start failed: %s (non-fatal, continuing)", esp_err_to_name(err));
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();
    register_espnow_command();
    cap_lua_register_module("espnow_motor", luaopen_espnow_motor);

    /* MJPEG camera stream */
    s_jpg_mutex = xSemaphoreCreateMutex();
    s_jpg_max = CAM_W * CAM_H * 2;
    s_jpg_buf = malloc(s_jpg_max);
    s_jpg_send = malloc(s_jpg_max);
    xTaskCreate(camera_capture_task, "cam_jpg", 8192, NULL, 4, NULL);

    /* Register MJPEG HTTP handler on existing server */
    httpd_handle_t hd = http_server_get_handle();
    if (hd) {
        httpd_uri_t uri = { .uri = "/mjpeg", .method = HTTP_GET, .handler = mjpeg_handler, .user_ctx = NULL };
        esp_err_t reg_err = httpd_register_uri_handler(hd, &uri);
        if (reg_err == ESP_OK) {
            ESP_LOGI(TAG, "MJPEG: http://<ip>/mjpeg");
        } else {
            ESP_LOGE(TAG, "MJPEG: register /mjpeg FAILED: %s", esp_err_to_name(reg_err));
        }
    } else {
        ESP_LOGE(TAG, "MJPEG: http_server_get_handle returned NULL, cannot register /mjpeg");
    }

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}

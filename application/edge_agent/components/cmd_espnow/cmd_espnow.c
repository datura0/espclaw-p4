/*
 * ESP-NOW Bridge Console Commands
 */
#include "cmd_espnow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "espnow_bridge.h"

static const char *TAG = "CMD_ESPNOW";

/* ---- hex decode helper ---- */
static int hex_decode(const char *hex, uint8_t *out, int max_len)
{
    int len = 0;
    while (*hex && len < max_len) {
        while (*hex == ' ' || *hex == ':') hex++;
        if (!hex[0] || !hex[1]) break;
        char hi = tolower((unsigned char)hex[0]);
        char lo = tolower((unsigned char)hex[1]);
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) break;
        int h = (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
        int l = (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
        out[len++] = (uint8_t)((h << 4) | l);
        hex += 2;
    }
    return len;
}

/* ---- "espnow send" command ---- */
static int cmd_espnow_send(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: espnow send <MAC> <hex_data>\n");
        printf("  MAC:      xx:xx:xx:xx:xx:xx or ff:ff:ff:ff:ff:ff (broadcast)\n");
        printf("  hex_data: hex bytes to send (e.g. 68656c6c6f for \"hello\")\n");
        return 1;
    }

    uint8_t mac[6];
    if (hex_decode(argv[1], mac, 6) != 6) {
        printf("ERROR: Invalid MAC '%s'. Use xx:xx:xx:xx:xx:xx\n", argv[1]);
        return 1;
    }

    uint8_t data[CONFIG_ESPNOW_BRIDGE_MAX_FRAME_SIZE - 7];
    int dlen = hex_decode(argv[2], data, sizeof(data));
    if (dlen == 0) {
        printf("ERROR: No valid hex data in '%s'\n", argv[2]);
        return 1;
    }

    esp_err_t err = espnow_bridge_send_espnow(mac, data, dlen);
    if (err != ESP_OK) {
        printf("ERROR: Send failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("OK: Sent %d bytes to %02x:%02x:%02x:%02x:%02x:%02x\n",
           dlen, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    ESP_LOGI(TAG, "Sent %d bytes to %02x:%02x:%02x:%02x:%02x:%02x",
             dlen, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 0;
}

/* ---- "espnow peer" command ---- */
static int cmd_espnow_peer(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: espnow peer add <MAC>\n");
        printf("       espnow peer del <MAC>\n");
        return 1;
    }

    uint8_t mac[6];
    if (hex_decode(argv[2], mac, 6) != 6) {
        printf("ERROR: Invalid MAC '%s'\n", argv[2]);
        return 1;
    }

    esp_err_t err;
    if (strcmp(argv[1], "add") == 0) {
        err = espnow_bridge_add_peer(mac);
    } else if (strcmp(argv[1], "del") == 0) {
        err = espnow_bridge_remove_peer(mac);
    } else {
        printf("Usage: espnow peer add|del <MAC>\n");
        return 1;
    }

    if (err != ESP_OK) {
        printf("ERROR: %s peer failed: %s\n", argv[1], esp_err_to_name(err));
        return 1;
    }
    printf("OK: peer %s %02x:%02x:%02x:%02x:%02x:%02x\n",
           argv[1], mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 0;
}

/* ---- "espnow slave" — convenience commands for slave devices ---- */

#define SLAVE_CMD_ECHO    0x01
#define SLAVE_CMD_PING    0x02
#define SLAVE_CMD_GPIO_WR 0x10
#define SLAVE_CMD_GPIO_RD 0x11
#define SLAVE_CMD_INFO    0x20

static int cmd_espnow_slave(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: espnow slave <MAC> ping                  — heartbeat test\n");
        printf("       espnow slave <MAC> echo <hex>             — echo test\n");
        printf("       espnow slave <MAC> info                   — device info\n");
        printf("       espnow slave <MAC> gpio write <pin> <0|1> — set GPIO\n");
        printf("       espnow slave <MAC> gpio read  <pin>       — read GPIO\n");
        return 1;
    }

    uint8_t mac[6];
    if (hex_decode(argv[1], mac, 6) != 6) {
        printf("ERROR: Invalid MAC '%s'\n", argv[1]);
        return 1;
    }

    if (strcmp(argv[2], "ping") == 0) {
        uint8_t pkt[1] = { SLAVE_CMD_PING };
        espnow_bridge_send_espnow(mac, pkt, 1);
        printf("OK: ping sent to slave\n");
    } else if (strcmp(argv[2], "echo") == 0) {
        if (argc < 4) { printf("Usage: espnow slave <MAC> echo <hex>\n"); return 1; }
        uint8_t buf[200];
        buf[0] = SLAVE_CMD_ECHO;
        int dlen = hex_decode(argv[3], buf + 1, sizeof(buf) - 1);
        if (dlen == 0) { printf("ERROR: invalid hex\n"); return 1; }
        espnow_bridge_send_espnow(mac, buf, 1 + dlen);
        printf("OK: echo sent (%d bytes)\n", dlen);
    } else if (strcmp(argv[2], "info") == 0) {
        uint8_t pkt[1] = { SLAVE_CMD_INFO };
        espnow_bridge_send_espnow(mac, pkt, 1);
        printf("OK: info request sent\n");
    } else if (strcmp(argv[2], "gpio") == 0) {
        if (argc < 5) { printf("Usage: espnow slave <MAC> gpio write|read ...\n"); return 1; }
        int pin = atoi(argv[4]);
        if (pin < 0 || pin > 48) { printf("ERROR: pin out of range\n"); return 1; }
        if (strcmp(argv[3], "write") == 0) {
            if (argc < 6) { printf("Usage: espnow slave <MAC> gpio write <pin> <0|1>\n"); return 1; }
            int level = atoi(argv[5]) ? 1 : 0;
            uint8_t pkt[3] = { SLAVE_CMD_GPIO_WR, (uint8_t)pin, (uint8_t)level };
            espnow_bridge_send_espnow(mac, pkt, 3);
            printf("OK: GPIO%d = %d\n", pin, level);
        } else if (strcmp(argv[3], "read") == 0) {
            uint8_t pkt[2] = { SLAVE_CMD_GPIO_RD, (uint8_t)pin };
            espnow_bridge_send_espnow(mac, pkt, 2);
            printf("OK: GPIO%d read request sent\n", pin);
        } else {
            printf("Usage: espnow slave <MAC> gpio write|read ...\n");
            return 1;
        }
    } else {
        printf("Unknown subcommand '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}

/* ---- "espnow_channel" — set S3 bridge WiFi channel ---- */
static int cmd_espnow_channel(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: espnow_channel <1-14>\n");
        printf("  Set the S3 bridge ESP-NOW channel to match your WiFi AP.\n");
        return 1;
    }
    int ch = atoi(argv[1]);
    if (ch < 1 || ch > 14) {
        printf("ERROR: channel must be 1-14\n");
        return 1;
    }
    esp_err_t err = espnow_bridge_set_channel((uint8_t)ch);
    if (err != ESP_OK) {
        printf("ERROR: set channel failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: S3 bridge channel set to %d\n", ch);
    return 0;
}

/* ---- "motor_3axis" — send 3-axis motor position command ---- */
static int cmd_motor_3axis(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: motor_3axis <MAC> <roll> <pitch> <yaw>\n");
        printf("  Send 3-axis motor position command to slave\n");
        printf("  Each axis in degrees (float), relative to lock point\n");
        return 1;
    }

    uint8_t mac[6];
    if (hex_decode(argv[1], mac, 6) != 6) {
        printf("ERROR: Invalid MAC '%s'\n", argv[1]);
        return 1;
    }

    float roll  = atof(argv[2]);
    float pitch = atof(argv[3]);
    float yaw   = atof(argv[4]);

    uint8_t pkt[14];
    pkt[0] = 0x40;
    pkt[1] = 0x01;
    memcpy(pkt + 2,  &roll,  4);
    memcpy(pkt + 6,  &pitch, 4);
    memcpy(pkt + 10, &yaw,   4);

    esp_err_t err = espnow_bridge_send_espnow(mac, pkt, sizeof(pkt));
    if (err != ESP_OK) {
        printf("ERROR: send failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: 3-axis → roll=%.2f pitch=%.2f yaw=%.2f\n",
           (double)roll, (double)pitch, (double)yaw);
    return 0;
}

/* ---- "motor_axis" — single-axis motor command ---- */
static int cmd_motor_axis(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: motor_axis <MAC> <x|y|z> <degrees>\n");
        printf("  x=roll  y=pitch  z=yaw\n");
        return 1;
    }
    uint8_t mac[6];
    if (hex_decode(argv[1], mac, 6) != 6) {
        printf("ERROR: Invalid MAC '%s'\n", argv[1]); return 1;
    }
    uint8_t axis = 2; /* default z/yaw */
    if (argv[2][0] == 'x' || argv[2][0] == 'X') axis = 0;
    else if (argv[2][0] == 'y' || argv[2][0] == 'Y') axis = 1;
    else if (argv[2][0] == 'z' || argv[2][0] == 'Z') axis = 2;
    float val = atof(argv[3]);

    uint8_t pkt[7];
    pkt[0] = 0x41;
    pkt[1] = axis;
    pkt[2] = 0x01;
    memcpy(pkt + 3, &val, 4);
    esp_err_t err = espnow_bridge_send_espnow(mac, pkt, sizeof(pkt));
    if (err != ESP_OK) {
        printf("ERROR: send failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK: axis %c → %.2f°\n", "xyz"[axis], (double)val);
    return 0;
}

/* ---- Register ---- */
esp_err_t register_espnow_command(void)
{
    const esp_console_cmd_t cmd_send = {
        .command = "espnow_send",
        .help = "Send raw ESP-NOW: espnow_send <MAC> <hex>",
        .hint = NULL,
        .func = &cmd_espnow_send,
    };
    const esp_console_cmd_t cmd_peer = {
        .command = "espnow_peer",
        .help = "Manage peers: espnow_peer add|del <MAC>",
        .hint = NULL,
        .func = &cmd_espnow_peer,
    };
    const esp_console_cmd_t cmd_slave = {
        .command = "espnow_slave",
        .help = "Control slave: ping|echo|info|gpio ...",
        .hint = NULL,
        .func = &cmd_espnow_slave,
    };
    const esp_console_cmd_t cmd_channel = {
        .command = "espnow_channel",
        .help = "Set S3 bridge ESP-NOW channel (1-14)",
        .hint = NULL,
        .func = &cmd_espnow_channel,
    };
    const esp_console_cmd_t cmd_motor = {
        .command = "motor_3axis",
        .help = "Motor 3-axis: motor_3axis <MAC> <roll> <pitch> <yaw>",
        .hint = NULL,
        .func = &cmd_motor_3axis,
    };
    const esp_console_cmd_t cmd_axis = {
        .command = "motor_axis",
        .help = "Motor 1-axis: motor_axis <MAC> <x|y|z> <degrees>",
        .hint = NULL,
        .func = &cmd_motor_axis,
    };

    esp_err_t err = esp_console_cmd_register(&cmd_send);
    if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&cmd_peer);
    if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&cmd_slave);
    if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&cmd_channel);
    if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&cmd_motor);
    if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&cmd_axis);
    return err;
}

# ESP-NOW UART Bridge — ESP32-S3 Firmware
#
# Wiring:
#   S3 GPIO17 (TX) → P4 IO30 (RX)
#   S3 GPIO18 (RX) → P4 IO31 (TX)
#   S3 GND         → P4 GND
#
# Protocol (binary framed, little-endian):
#   Frame: | 0xAA | CMD(1B) | LEN(2B) | PAYLOAD(LEN) | CRC8(1B) |
#
#   P4→S3 commands:
#     CMD 0x01 — ESP-NOW SEND:  MAC(6B) + data(NB)
#     CMD 0x02 — ADD PEER:      MAC(6B)
#     CMD 0x03 — REMOVE PEER:   MAC(6B)
#
#   S3→P4 responses:
#     CMD 0x81 — ESP-NOW RECV:  MAC(6B) + rssi(1B) + data(NB)
#     CMD 0xF0 — ACK/STATUS:    code(1B)  (0=OK, 1=ERR)
#

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espnow_s3_bridge)

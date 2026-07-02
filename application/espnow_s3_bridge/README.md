# ESP-NOW UART Bridge — ESP32-S3 Firmware

Bridges ESP-NOW ⇄ UART for the ESP32-P4 Edge Agent.

```
┌──────────┐  UART (115200-8N1)  ┌──────────┐  ESP-NOW   ┌──────────┐
│ ESP32-P4 │◄──────────────────►│ ESP32-S3 │◄─────────►│  Peers   │
│  (host)  │  IO30←TX18  RX17→31│ (bridge) │  Ch.1      │          │
└──────────┘                    └──────────┘            └──────────┘
```

## Wiring

| S3 Pin | Direction | P4 Pin |
|--------|-----------|--------|
| GPIO18 | TX →      | IO30   |
| GPIO17 | RX ←      | IO31   |
| GND    | ---       | GND    |

## Flash

```bash
cd d:\esp_test\esp-claw\application\espnow_s3_bridge
idf.py set-target esp32s3
idf.py build
idf.py -p COMxx flash monitor
```

## Protocol (matches P4 espnow_bridge component)

- **Frame:** `[2-byte LE length][N-byte payload]`
- **Max payload:** 250 bytes

### P4 → S3 commands (payload byte 0)

| CMD  | Name       | Payload                   |
|------|------------|---------------------------|
| 0x01 | SEND       | MAC(6B) + data(NB)        |
| 0x02 | ADD_PEER   | MAC(6B)                   |
| 0x03 | REMOVE_PEER| MAC(6B)                   |

### S3 → P4 events (payload byte 0)

| CMD  | Name   | Payload                     |
|------|--------|-----------------------------|
| 0x81 | RECV   | MAC(6B) + RSSI(1B) + data   |
| 0xF0 | STATUS | code(1B)                    |

Status codes: 0x00=OK, 0x01=ERR_SEND, 0x02=ERR_PEER, 0x03=ERR_PARSE, 0x05=PEER_FULL

# ESP-NOW 多机控制平台 — 使用说明

```
┌──────────────┐   UART      ┌──────────────┐  ESP-NOW    ┌──────────────┐
│   ESP32-P4   │◄──────────►│   ESP32-S3   │◄──────────►│  s3test (从机) │
│  (主控中心)  │ IO31→RX17   │   (桥接模块) │   2.4GHz   │  (电机+OTA)  │
│              │ IO30←TX18   │             │   Ch.同WiFi │              │
│              │             │             │            │              │
│ IO4←TX       │             │             │            │              │
│ IO5→RX       │             │             │            │              │
│   JY61P IMU  │             │             │            │              │
└──────────────┘             └──────────────┘           └──────────────┘
```

---

## 一、三个设备一览

| 设备 | 路径 | 芯片 | 作用 |
|------|------|------|------|
| P4 主控 | `application/edge_agent` | ESP32-P4 | 读 IMU → 姿态下发 → 控制台命令 |
| S3 桥接 | `application/espnow_s3_bridge` | ESP32-S3 | UART ↔ ESP-NOW 中转 |
| s3test | `d:\esp_test\s3_test` | ESP32-S3 | 收姿态 → 驱电机，WiFi OTA |

---

## 二、接线

### P4 ↔ S3
```
P4 IO31 (TX) ──→ S3 GPIO17 (RX)
P4 IO30 (RX) ──← S3 GPIO18 (TX)
P4 GND      ──── S3 GND
```

### P4 ↔ JY61P 传感器
```
P4 IO4 (TX)  ──→ JY61P RX
P4 IO5 (RX)  ──← JY61P TX
P4 3.3V      ──── JY61P VCC
P4 GND       ──── JY61P GND
```

---

## 三、构建 & 烧录

### 首次使用 S3 桥接
```bash
cd application/espnow_s3_bridge
idf.py set-target esp32s3
idf.py build
idf.py -p COMxx flash
```

### 日常使用

```bash
# 烧录 P4
cd application/edge_agent
idf.py build && idf.py -p COMxx flash monitor

# 烧录 s3test
cd d:\esp_test\s3_test
idf.py build && idf.py -p COMxx flash monitor
```

---

## 四、⚠️ 信道对齐（必做！）

ESP-NOW 只能在 **2.4GHz** 工作。S3 桥接和 s3test **必须在同一信道**。

### ① 查 Datura 热点信道
电脑连上 Datura 热点后，在 **PowerShell** 运行：
```powershell
netsh wlan show interfaces | findstr "信道"
```
输出如 `信道 : 6`

### ② P4 控制台切信道
连接 P4 串口后输入：
```bash
espnow_channel 6
```

---

## 五、P4 控制台命令大全

P4 的 `idf.py monitor` 支持键盘输入：

| 命令 | 示例 | 说明 |
|------|------|------|
| `espnow_channel <1-14>` | `espnow_channel 6` | 设置 S3 桥接信道 |
| `espnow_peer add <MAC>` | `espnow_peer add aa:bb:cc:dd:ee:ff` | 添加 ESP-NOW 节点 |
| `espnow_slave <MAC> ping` | — | 心跳测试，看通不通 |
| `espnow_slave <MAC> info` | — | 查从机固件版本 |
| `espnow_slave <MAC> echo <hex>` | `espnow_slave ... echo 48656c6c6f` | 回显测试 |
| `espnow_slave <MAC> gpio write 2 1` | — | 控制从机 GPIO |
| `espnow_send <MAC> <hex>` | `espnow_send ff:ff:ff:ff:ff:ff 010203` | 发送原始数据 |
| `help` | — | 列出所有命令 |

---

## 六、姿态数据自动下发

P4 上电后自动：
1. 从 JY61P 读取 roll / pitch / yaw
2. 每 **50ms** 通过 ESP-NOW 广播姿态包
3. s3test 收到后打印角度，可接入电机控制

**改目标 MAC：** `idf.py menuconfig` → App Config → IMU & Attitude Dispatch

**s3test 收到格式：** `CMD 0x30 | roll(4B) | pitch(4B) | yaw(4B)` 共 13 字节

---

## 七、首测步骤

```bash
# 1. 电脑连 Datura → PowerShell 查信道
netsh wlan show interfaces | findstr "信道"

# 2. 烧录三块板子

# 3. P4 monitor:
espnow_channel <查到的信道号>
espnow_peer add <s3test启动时打印的MAC>

# 4. 验证
espnow_slave <MAC> ping        # 收到 STATUS 0x00 就通了
espnow_slave <MAC> info        # 收到 MAC + 版本号

# 5. 姿态自动下发中，s3test 串口会持续打印:
# I (xxx) esp_now: Attitude: roll=-1.23 pitch=5.67 yaw=89.01
```

---

## 八、FAQ

**Q: s3test 收不到 ESP-NOW？**
A: ① Datura 必须是 2.4GHz（不是 5GHz） ② 信道已对齐 ③ `espnow_peer` 已添加

**Q: idf.py monitor 怎么输入命令？**
A: 直接键盘敲，回车发送，`Ctrl+]` 退出

**Q: S3 桥接怎么供电？**
A: 用 P4 的 3.3V + GND 即可，功耗很低

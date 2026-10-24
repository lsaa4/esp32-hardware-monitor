# ESP32-S3 电脑状态副屏

[![build](https://github.com/lsaa4/esp32-hardware-monitor/actions/workflows/build.yml/badge.svg)](https://github.com/lsaa4/esp32-hardware-monitor/actions/workflows/build.yml)

一块 ESP32-S3 + ST7789 SPI 彩屏，把电脑的 CPU / GPU 温度、负载、频率、功耗，以及内存、硬盘、网速、电池做成一块**每 10 秒自动翻页的实时副屏**。

数据来自 PC 上的 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)（下称 LHM）的 Web 接口，通过局域网 HTTP 拉取，**PC 端不用装任何客户端或驱动**。

> 仓库内不含任何 WiFi 账号或密码。首次使用请复制 `secrets.example.h` 为 `secrets.h` 填入自己的 WiFi，或烧录后用 `wifi_manager.html` 串口配网。

---

## 功能

- **5 页轮播**，每页 10 秒，右上角圆点指示当前页
- **1 秒刷新**，页脚实时显示 HTTP 往返延迟（RTT）
- **自动发现数据源**：PC 换 IP、换机器都不用重新烧录
- **串口配网**：Chrome / Edge 打开 `wifi_manager.html`，扫描 → 选网 → 输密码，账号存进 NVS
- **差量绘制**：同一页内只重绘变化的数字/进度条，不整屏刷新，不闪
- **状态灯**（板载 WS2812，GPIO48）：无 WiFi 蓝色呼吸 / 有 WiFi 无数据红色慢闪 / 正常按 CPU 温度绿→黄→红呼吸 / 翻页白闪
- **不写死硬件型号**：CPU / GPU 名字从 LHM 的传感器树里读，AMD / Intel、NVIDIA / AMD 都能用

## 五个页面

| 页 | 标题 | 内容 |
|---|---|---|
| 1 | PC MONITOR | CPU / GPU 温度、负载、内存占用、CPU / GPU 封装功耗 |
| 2 | CPU DETAIL | 每线程负载柱状图（最多 32）、每核频率柱状图、平均频率、封装功耗、CPU 型号 |
| 3 | GPU DETAIL | 核心温度 / 热点 / 显存温度、GPU 负载、显存占用、核心频率、功耗、GPU 型号 |
| 4 | STORAGE / RAM | 最多 2 块硬盘的温度 / 占用 / 读写速率、内存用量、内存条温度 |
| 5 | NET / POWER | 下行 / 上行速率、电池电量 / 电压 / 健康度 |

## 硬件

| 物料 | 说明 |
|---|---|
| ESP32-S3 开发板 | 16 MB Flash + 8 MB OPI PSRAM（N16R8），板载 CH343 串口 |
| SPI 彩屏 | ST7789，320×240，8 针（VCC / GND / SCL / SDA / RES / DC / CS / BLK） |
| 杜邦线 | 8 根 |

### 接线

| 屏幕引脚 | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL / SCLK | GPIO21 |
| SDA / MOSI | GPIO47 |
| RES / RST | GPIO45 |
| DC | GPIO40 |
| CS | GPIO41 |
| BLK / BL | GPIO42（高电平点亮） |

- 走硬件 SPI（FSPI），**20 MHz**。杜邦线上 40 MHz 会花屏，不建议提速。
- 屏幕原生 240×320 竖屏，代码里 `setRotation(3)` 转成 320×240 横屏。

## 快速开始

### 1. PC 端：启动数据源

1. 从 [LHM Releases](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases) 下载并解压（不要下载源码包）。
2. 把本仓库 `pc/` 下的三个文件复制进 LHM 解压目录：
   ```
   start-monitor.bat
   lhm_beacon.ps1
   lhm_beacon.vbs
   ```
3. 右键 `start-monitor.bat` → **以管理员身份运行**。它会：
   - 把 TCP 8085 加进 Windows 防火墙白名单
   - 以管理员权限启动 `LibreHardwareMonitor.exe`（读 CPU 温度等传感器必须要管理员）
   - 后台启动 UDP 8099 广播（供 ESP32 自动发现）
4. **首次使用**需要在 LHM 菜单里打开 Web 服务：
   ```
   Options → Remote Web Server → Run        （端口 8085）
   ```
   浏览器访问 <http://127.0.0.1:8085/data.json>，能看到一大段 JSON 就成功了。

### 2. 编译

Arduino IDE 或 arduino-cli 均可。板子参数**必须完全一致**：

```
开发板     : ESP32S3 Dev Module
Flash Size : 16MB
PSRAM      : OPI PSRAM
Partition  : 16M Flash (3MB APP/9.9MB FATFS)   → app3M_fat9M_16MB
USB CDC On Boot : Disabled（默认值，千万别开）
```

`USB CDC On Boot` 开了以后 `Serial` 会被重定向到原生 USB 口，CH343 口将看不到任何日志。

依赖只有 **ArduinoJson v7**。显示部分用的是本仓库自带的 `TFT_hal.h`，**不需要装 TFT_eSPI**。

```powershell
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install ArduinoJson

arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" `
  firmware/esp32_monitor_hal
```

不想在本地装工具链也可以：每次 push 后 CI 会编译一遍，产物在 Actions 页面对应的 run 里（Artifacts → `firmware-<sha>.zip`，含 app / bootloader / partitions 三个 bin）。

WiFi 二选一：

```powershell
# 方式 A：编译期写死（文件不会被提交）
copy firmware\esp32_monitor_hal\secrets.example.h firmware\esp32_monitor_hal\secrets.h
# 然后编辑 secrets.h 填入 SSID / 密码
```

方式 B 见下面的「配网」一节。

### 3. 烧录

Arduino IDE：`项目 → 导出已编译的二进制文件`，产物在 `build/esp32.esp32.esp32s3/` 下。

**只更新程序**（约 2 秒，保留 NVS 里已配好的 WiFi）：

```powershell
esptool --chip esp32s3 --port COM5 --baud 921600 write-flash 0x10000 esp32_monitor_hal.ino.bin
```

**整片刷写**（16 MB 全量，会清掉 NVS，刷完要重新配网）：

```powershell
esptool --chip esp32s3 --port COM5 --baud 921600 write-flash 0x0 esp32_monitor_hal.ino.merged.bin
```

> 串口要用板子上 **CH343** 那个 Type-C 口（设备管理器里显示 `USB-SERIAL CH343`），不是标着 USB 的原生口。
> esptool v4 的命令是 `write_flash`（下划线），v5 是 `write-flash`。

### 4. 配网（不用重新烧录）

Chrome / Edge 打开 `firmware/esp32_monitor_hal/wifi_manager.html`：

1. 点「连接串口」→ 选 CH343 的 COM 口（115200）
2. 点「扫描」，等约 3 秒
3. 选中目标 WiFi → 输密码 → 「连接」
4. 成功会显示 `已连接 IP=192.168.x.x`

板子把账号存进 NVS，下次开机自动连。换 WiFi 重复上面步骤即可。

> 没有 `secrets.h` 时，开机后有 12 秒的配网窗口；错过了也没关系，界面出来后随时可以配。

> 双击 html 如果提示浏览器不支持 Web Serial，在该目录执行 `python -m http.server 8000`，
> 再打开 <http://localhost:8000/wifi_manager.html>。

## 工作原理

```
┌──────────────┐   HTTP GET /data.json    ┌───────────────┐
│ LibreHardware│ ◄─────────────────────── │  ESP32-S3     │
│ Monitor :8085│                          │  (每 1 秒)    │
└──────┬───────┘                          └───────┬───────┘
       │                                          │
       │  UDP 广播 :8099                          │ SPI 20MHz
       │  "LHM|<ip>|8085|<主机名>"  每 2 秒        ▼
       └─────────────────────────────────►  ST7789 320×240
```

**数据解析**：LHM 的 `data.json` 是一棵树（根 → 电脑名 → 硬件 → 分组 → 传感器）。代码只按传感器的 `Text` 名字和 `Value` 的单位（`°C` / `%` / `MHz` / `W` / `B/s`）匹配，不依赖数组下标，所以换 CPU / 显卡不用改代码。

>`data.json` 大约 30–60 KB，ArduinoJson 默认嵌套上限 10 层会报 `TooDeep`，代码里放宽到了 40 层。

**数据源自动发现**，按顺序尝试：

1. NVS 里上次成功的 IP（探测 1.5 秒）
2. 编译时的兜底地址 `PC_HOST`，默认 `192.168.137.1`（Windows 移动热点 / ICS 的固定网关，探测 1.2 秒）
3. 监听 UDP 8099 广播，最长 8 秒，同网段优先，收到后探测 2.5 秒

找到后写入 NVS。连续 5 次拉取失败（约 5 秒）会重新走一遍发现流程，所以 PC 换 IP 也能自己恢复。

**为什么不用 TFT_eSPI**：TFT_eSPI 直接操作 SPI 外设寄存器，而 arduino-esp32 3.x 的 `SPIClass::begin()` 会延迟清零这些寄存器，实测 `tft.init()` 卡死、或初始化成功但推不出像素（白屏）。`TFT_hal.h` 保留了 TFT_eSPI 的类名和方法签名，底层改用 `SPIClass::writeBytes()`，再配一套 4 bit 抗锯齿字模。

## 串口配网协议

`wifi_manager.html` 通过 Web Serial 发命令，板子用 `#TAG {json}` 回包，115200 8N1。

| PC → 板子 | 板子 → PC |
|---|---|
| `STATUS` | `#STATUS {"connected":true,"ssid":"...","ip":"...","rssi":-59}` |
| `SCAN` | `#SCAN {"nets":[{"s":"SSID","r":-62,"e":1,"a":3}]}` |
| `WIFI <ssid>\t<密码>` | `#OK {"ip":"192.168.1.23"}` 或 `#ERR {"msg":"connect failed"}` |
| `FORGET` | `#OK {"msg":"forgotten"}` |

`SCAN` 返回的 `e`：0 = 开放网络，1 = 加密；`a` 是 `wifi_auth_mode_t` 原始值。

## 目录结构

```
.
├─ README.md
├─ LICENSE
├─ .gitignore
├─ firmware/
│  └─ esp32_monitor_hal/          # Arduino 要求目录名与 .ino 同名
│     ├─ esp32_monitor_hal.ino    # 主程序：抓取、解析、5 页 UI、配网
│     ├─ TFT_hal.h                # ST7789 驱动 + 4bit 抗锯齿文字（替代 TFT_eSPI）
│     ├─ font_aa.h                # 字模数据（ASCII 32–126，4 个字号）
│     ├─ wifi_manager.html        # Web Serial 配网页
│     └─ secrets.example.h        # WiFi 配置模板，复制为 secrets.h 使用
├─ pc/
│  ├─ start-monitor.bat           # 提权、放行防火墙、启动 LHM + 广播
│  ├─ lhm_beacon.ps1              # UDP 8099 广播本机 IP
│  └─ lhm_beacon.vbs              # 无窗口启动上面的 ps1
└─ .github/workflows/build.yml    # push / tag 时自动编译固件
```

## 常见问题

**屏幕显示 PC OFFLINE**

1. LHM 在运行吗？菜单里 Web 服务开了吗？本机 <http://127.0.0.1:8085/data.json> 能打开吗？
2. 板子和 PC 在同一个路由器 / 同一个热点下吗？访客网络和 AP 隔离会导致互相访问不了。
3. 防火墙放行了吗？换端口后要重跑一次 `start-monitor.bat`。
4. 关掉 LHM 里用不到的传感器（主板、部分硬盘、网卡）能让 `data.json` 小很多，解析更快。

**扫不到 WiFi / 连不上**

- ESP32-S3 **只支持 2.4 GHz**，5 GHz 热点扫不到也连不上。
- Windows 移动热点：认证方式是 WPA2/WPA3 混合，代码里已按「先 WPA2 关 PMF，失败再 WPA3」处理；另外热点没有设备连接时会自动关闭，先确认它还开着。
- 密码输错也会写进 NVS，之后开机会一直重试失败。用网页上的「忘记此 WiFi」清掉再配。

**白屏 / 花屏**

- 四个板子参数必须和上面完全一致（16 MB / OPI PSRAM / app3M_fat9M_16MB / CDC 关）。
- 检查接线，尤其是 RST(45)、DC(40)、CS(41) 有没有接错。
- SPI 降到 20 MHz（`TFT_hal.h` 里的 `TFT_FREQ`），杜邦线太长也会花屏。

**串口没有日志**

用的是 CH343 口吗？`USB CDC On Boot` 是不是被打开了？波特率 115200。

**中文显示成问号**

字模只覆盖 ASCII 32–126，界面文字必须用英文。要中文得自己换 `font_aa.h` 里的字模数据。

## 已知限制

- 只支持 2.4 GHz WiFi。
- 界面仅支持 ASCII（字模是 ASCII 95 字符）。
- 最多显示 2 块硬盘、2 根内存条。
- 全量刷写 `merged.bin` 会清空 NVS，WiFi 需要重新配。
- LHM 读传感器需要管理员权限，普通权限下很多温度值是空的。
- 仅在 Windows + LibreHardwareMonitor 上验证过；LHM 的 `data.json` 结构变了就需要同步改解析。

## 许可证

代码以 [MIT](LICENSE) 协议开源。

第三方资源：

- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — MIT
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) — MPL-2.0，**不随本仓库分发**，请从上游下载
- `font_aa.h` 的字模由 Segoe UI Bold 转换而来，属于微软字体衍生资源，不在 MIT 授权范围内；商用或再分发请自行替换为开源字体（如 Inter、Noto Sans）重新生成字模

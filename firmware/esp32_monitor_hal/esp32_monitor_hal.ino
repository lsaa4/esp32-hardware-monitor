/* ============================================================
 *  ESP32-S3 实时设备信息屏  v2（自动发现数据源）
 *
 *  数据源：PC 上的 LibreHardwareMonitor → http://<PC_IP>:8085/data.json
 *
 *  自动发现机制（不用手填 IP）：
 *    PC 端 lhm_beacon.ps1 每 2 秒向局域网 UDP 8099 广播
 *      "LHM|<PC_IP>|8085|<主机名>"
 *    ESP32 开机后按 缓存 NVS → 兜底宏 PC_HOST → 监听广播 的顺序找，
 *    找到就把 IP 写进 NVS，下次开机 1 秒内连上。
 *    PC 换 IP / 换机器也无需重新编译烧录。
 *
 *  【PC 端，只做一次】
 *    把 pc/ 下的三个文件放进 LibreHardwareMonitor 目录，
 *    右键 start-monitor.bat → 以管理员身份运行（放行 8085 + 启动广播）
 *    浏览器打开 http://127.0.0.1:8085/data.json 能看到 JSON = 成功
 *
 *  【Arduino 端】
 *    - 库：只装 ArduinoJson v7（显示走本目录 TFT_hal.h，不需要 TFT_eSPI）
 *    - 板：ESP32S3 Dev Module，Flash 16MB，PSRAM OPI PSRAM，USB CDC On Boot Disabled
 *    - WiFi：复制 secrets.example.h 为 secrets.h 填自己的账号（不会被提交），
 *            或者烧录后打开 wifi_manager.html 用串口配网
 * ============================================================ */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "TFT_hal.h"
#include <Preferences.h>
// ================= 配置 =================
// WiFi 账号不要写进代码：复制 secrets.example.h 为 secrets.h 再填，
// secrets.h 已在 .gitignore 里，不会被提交。
// 没有 secrets.h 时用空字符串，开机直接进配网流程（wifi_manager.html）。
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID   ""
  #define WIFI_PASS   ""
#endif

// 数据源 IP 兜底值：正常情况下靠 UDP 广播自动发现，不用改这里。
// 192.168.137.1 是 Windows 移动热点 / ICS 的固定网关，热点场景下开机可直接命中；
// 连普通路由器时这一步会失败约 1 秒，随后由广播自动纠正。
#define PC_HOST     "192.168.137.1"

// 刷新间隔（毫秒）。经验值：
//   1000 = 推荐，跟得上 LHM 的采集节奏，数字也看得清
//    500 = 上限附近，LHM 侧会开始重复返回同一批值
//   2000 = 省电/稳定优先
#define REFRESH_MS  1000
#define PC_PORT     8085

// 开机 SPI 扫频自检：1 = 打开（会闪 4 下白屏并打印各频率耗时），0 = 关闭
#define PERF_SWEEP  0

// 自动发现：PC 端 lhm_beacon.ps1 每 2 秒向本端口广播 "LHM|IP|端口|主机名"
#define DISCOVERY_PORT 8099
// ==========================================

TFT_eSPI tft = TFT_eSPI();

// 运行时真正使用的主机（自动发现会改写它）
String   gHost = PC_HOST;
uint16_t gPort = PC_PORT;

WiFiUDP     udp;
Preferences prefs;

#define MAX_CORE 32

struct Metrics {
  bool  ok        = false;

  // ---- CPU ----
  float cpuTemp   = NAN;
  float cpuLoad   = NAN;
  float cpuClock  = NAN;      // Cores (Average)
  float cpuPower  = NAN;      // Package W
  float ccd1      = NAN;
  float ccd2      = NAN;
  float coreLoad[MAX_CORE];   // 每线程负载 %
  int   nCoreLoad = 0;
  float coreMhz[MAX_CORE];    // 每物理核频率
  int   nCoreMhz  = 0;
  float coreW[MAX_CORE];      // 每核功耗 (SMU)
  int   nCoreW    = 0;
  char  cpuName[40] = "";     // CPU 型号（从 LHM 的硬件节点读，不写死）

  // ---- GPU ----
  float gpuTemp   = NAN;
  float gpuHot    = NAN;
  float gpuMemTemp= NAN;
  float gpuLoad   = NAN;
  float gpuClock  = NAN;
  float gpuMemClk = NAN;
  float gpuPower  = NAN;
  float vramUsed  = NAN;      // MB
  float vramTotal = NAN;      // MB
  char  gpuName[40] = "";     // GPU 型号（从 LHM 的硬件节点读，不写死）

  // ---- 内存 ----
  float memUsed   = NAN;      // %
  float memUsedGB = NAN;
  float memTotalGB= NAN;
  float dimm0     = NAN;
  float dimm1     = NAN;

  // ---- 硬盘 ----
  int   nDisk     = 0;
  float dkT[2]    = {NAN, NAN};
  float dkUse[2]  = {NAN, NAN};
  float dkR[2]    = {NAN, NAN};
  float dkW[2]    = {NAN, NAN};
  char  dkName[2][20] = {"", ""};

  // ---- 网络 ----
  float netDown   = NAN;      // KB/s
  float netUp     = NAN;
  char  netName[20] = "";

  // ---- 电池 ----
  float batPct    = NAN;
  float batV      = NAN;
  float batHealth = NAN;

  int   rttMs     = 0;
};

Metrics m;
uint32_t lastFetch = 0;
uint8_t  failCount = 0;      // 连续失败次数，到 5 就重新自动发现

// 当前要连的 WiFi：boot 时从 NVS / 固件宏读，串口配网时更新
String   gWifiSsid;
String   gWifiPass;
bool     gUserWifi     = false;  // 串口改过 WiFi：boot 的两段式连接不要再用旧账号去试
bool     gNeedDiscover = false;  // WiFi 起来后要补一次数据源发现

// ---------------- JSON 解析 ----------------
// LHM 的 data.json 是树：root → 硬件 → 传感器
// 只按 Text 名字找，不依赖索引，换硬件也不用改代码
static float num(const char* v) {
  if (!v) return NAN;
  return atof(v);          // "45.0 °C" / "62.3 %" 都能直接取到数字
}

void walkSensors(JsonVariant v, Metrics& out, int depth, const char* hw, const char* grp) {
  if (depth > 7 || !v.is<JsonObject>()) return;
  JsonObject o = v.as<JsonObject>();

  const char* text  = o["Text"]  | "";
  const char* value = o["Value"] | "";

  // LHM 树：根 -> 电脑名 -> 硬件(depth2) -> 分组(depth3) -> 传感器
  const char* hwName  = hw;
  const char* grpName = grp;
  if (depth == 2) hwName = text;
  if (depth == 3) grpName = text;

  // 顺手记下真实硬件型号（页脚用）：CPU 节点里必然有 "CPU Total"，GPU 节点里有 "GPU Core"
  if (hwName && hwName[0]) {
    if (out.cpuName[0] == 0 && strcmp(text, "CPU Total") == 0)
      strncpy(out.cpuName, hwName, sizeof(out.cpuName) - 1);
    if (out.gpuName[0] == 0 && strcmp(text, "GPU Core") == 0)
      strncpy(out.gpuName, hwName, sizeof(out.gpuName) - 1);
  }

  // 单位判定：同名传感器在不同分组下单位完全不同，必须带单位
  const bool isC   = strstr(value, "C")   != nullptr;   // °C
  const bool isPct = strstr(value, "%")   != nullptr;
  const bool isMhz = strstr(value, "MHz") != nullptr;
  const bool isW   = strstr(value, " W")  != nullptr;
  const bool isKB  = strstr(value, "B/s") != nullptr;
  const bool inPowers = grpName && strcmp(grpName, "Powers") == 0;

  // ================= CPU =================
  // 温度：AMD "Core (Tctl/Tdie)" / Intel "CPU Package"，必须 °C
  if (isnan(out.cpuTemp) && isC &&
      (strstr(text, "Tctl/Tdie") || strcmp(text, "CPU Package") == 0))
    out.cpuTemp = num(value);

  if (isC && strcmp(text, "CCD1 (Tdie)") == 0) out.ccd1 = num(value);
  if (isC && strcmp(text, "CCD2 (Tdie)") == 0) out.ccd2 = num(value);

  if (isPct && strcmp(text, "CPU Total") == 0) out.cpuLoad = num(value);

  // 频率：AMD 用 Cores (Average)，Intel 用 CPU Core #1
  if (isnan(out.cpuClock) && isMhz && !strstr(text, "Effective") &&
      (strcmp(text, "Cores (Average)") == 0 || strcmp(text, "CPU Core #1") == 0))
    out.cpuClock = num(value);

  // 封装功耗：Powers 组里的 Package（不能和温度组的 CPU Package 混）
  if (isnan(out.cpuPower) && isW && inPowers &&
      (strcmp(text, "Package") == 0 || strcmp(text, "CPU Package") == 0))
    out.cpuPower = num(value);

  // 每线程负载 "CPU Core #N"
  if (isPct && strncmp(text, "CPU Core #", 10) == 0) {
    int idx = atoi(text + 10) - 1;
    if (idx >= 0 && idx < MAX_CORE) {
      out.coreLoad[idx] = num(value);
      if (idx + 1 > out.nCoreLoad) out.nCoreLoad = idx + 1;
    }
  }

  // 每物理核频率 "Core #N"（排除 Effective / SMU）
  if (isMhz && strncmp(text, "Core #", 6) == 0 && !strstr(text, "Effective")) {
    int idx = atoi(text + 6) - 1;
    if (idx >= 0 && idx < MAX_CORE) {
      out.coreMhz[idx] = num(value);
      if (idx + 1 > out.nCoreMhz) out.nCoreMhz = idx + 1;
    }
  }

  // 每核功耗 "Core #N (SMU)"
  if (isW && strstr(text, "(SMU)")) {
    int idx = atoi(text + 6) - 1;
    if (idx >= 0 && idx < MAX_CORE) {
      out.coreW[idx] = num(value);
      if (idx + 1 > out.nCoreW) out.nCoreW = idx + 1;
    }
  }

  // ================= GPU =================
  if (isC   && strcmp(text, "GPU Core") == 0)            out.gpuTemp    = num(value);
  if (isC   && strcmp(text, "GPU Hot Spot") == 0)        out.gpuHot     = num(value);
  // VRAM 温度：NVIDIA 叫 "GPU Memory Junction"，AMD 叫 "GPU Memory"（带 °C 判定）
  if (isC   && (strcmp(text, "GPU Memory Junction") == 0 || strcmp(text, "GPU Memory") == 0))
                                                          out.gpuMemTemp = num(value);
  if (isPct && strcmp(text, "GPU Core") == 0)            out.gpuLoad    = num(value);
  if (isMhz && strcmp(text, "GPU Core") == 0)            out.gpuClock   = num(value);
  if (isMhz && strcmp(text, "GPU Memory") == 0)          out.gpuMemClk  = num(value);
  if (isW   && strcmp(text, "GPU Package") == 0)         out.gpuPower   = num(value);
  if (strcmp(text, "GPU Memory Used") == 0)              out.vramUsed   = num(value);
  if (strcmp(text, "GPU Memory Total") == 0)             out.vramTotal  = num(value);

  // ================= 内存 =================
  if (hwName && strcmp(hwName, "Total Memory") == 0) {
    if (isPct && strcmp(text, "Memory") == 0)     out.memUsed  = num(value);
    if (strcmp(text, "Memory Used") == 0)         out.memUsedGB = num(value);
    if (strcmp(text, "Memory Available") == 0 && !isnan(out.memUsedGB))
      out.memTotalGB = out.memUsedGB + num(value);
  }

  // 内存条温度 "DIMM #0 / #1"
  if (isC && strncmp(text, "DIMM #", 6) == 0) {
    int idx = atoi(text + 6);
    if (idx == 0)      out.dimm0 = num(value);
    else if (idx == 1) out.dimm1 = num(value);
  }

  // ================= 硬盘 =================
  if (strcmp(text, "Composite Temperature") == 0) {
    if (out.nDisk < 2) {
      const char* nm = hwName ? hwName : "DISK";
      strncpy(out.dkName[out.nDisk], nm, 19);
      out.dkName[out.nDisk][19] = 0;
      out.dkT[out.nDisk] = num(value);
      out.nDisk++;
    }
  } else if (out.nDisk > 0) {
    int d = out.nDisk - 1;
    if (strcmp(text, "Used Space") == 0) out.dkUse[d] = num(value);
    if (strcmp(text, "Read Rate") == 0)  out.dkR[d]   = num(value);
    if (strcmp(text, "Write Rate") == 0) out.dkW[d]   = num(value);
  }

  // ================= 网络 =================
  if (isKB && strcmp(text, "Download Speed") == 0) {
    float nv = num(value);
    if (isnan(out.netDown) || nv > out.netDown) {
      out.netDown = nv;
      if (hwName) {
        strncpy(out.netName, hwName, 19);
        out.netName[19] = 0;
      }
    }
  }
  if (isKB && strcmp(text, "Upload Speed") == 0) {
    float nv = num(value);
    if (isnan(out.netUp) || nv > out.netUp) out.netUp = nv;
  }

  // ================= 电池 =================
  if (hwName && strcmp(hwName, "BAT") == 0) {
    if (isPct && strcmp(text, "Charge Level") == 0)      out.batPct    = num(value);
    if (isPct && strcmp(text, "Degradation Level") == 0) out.batHealth = num(value);
    if (strcmp(text, "Voltage") == 0)                    out.batV      = num(value);
  }

  for (JsonVariant child : o["Children"].as<JsonArray>())
    walkSensors(child, out, depth + 1, hwName, grpName);
}


// ---------------- 主机自动发现 ----------------
// 三层策略：NVS 缓存 → 代码兜底 IP → 监听 UDP 广播

static void serialPoll();   // 见「串口配网协议」：发现期间也要能响应配网命令

// 探测某个地址上是不是 LHM 的 data.json（HTTP 200 就算成功）
static bool probeHost(const String& ip, uint16_t port, uint32_t timeoutMs) {
  WiFiClient c;
  if (!c.connect(ip.c_str(), port, timeoutMs)) return false;

  String req = String("GET /data.json HTTP/1.0\r\nHost: ") + ip +
               "\r\nUser-Agent: esp32-monitor\r\nConnection: close\r\n\r\n";
  c.print(req);

  uint32_t t0 = millis();
  while (!c.available() && millis() - t0 < timeoutMs) delay(10);

  bool ok = false;
  if (c.available()) {
    String status = c.readStringUntil('\n');      // "HTTP/1.1 200 OK"
    ok = status.indexOf("200") > 0;
  }
  c.stop();
  return ok;
}

static void cacheHost(const String& ip, uint16_t port) {
  gHost = ip;
  gPort = port;
  prefs.putString("host", ip);
  prefs.putUShort("port", port);
  Serial.printf("[发现] 数据源 = %s:%u （已写入 NVS）\n", ip.c_str(), port);
}

// 监听广播，同网段候选优先（多网卡 PC 会同时播 VMware/WSL 等虚拟网卡）
static bool listenBeacon(uint32_t waitMs, String& outIp, uint16_t& outPort) {
  IPAddress self = WiFi.localIP();
  String   bestIp;
  uint16_t bestPort = 8085;
  bool     bestSame = false;

  udp.begin(DISCOVERY_PORT);
  Serial.printf("[发现] 监听 UDP %d，最长 %lu ms，本机 %s\n",
                DISCOVERY_PORT, (unsigned long)waitMs, self.toString().c_str());

  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    serialPoll();          // 别让 STATUS / 忘记此 WiFi 在发现期间卡满 8 秒
    int sz = udp.parsePacket();
    if (sz > 0) {
      char buf[96];
      int n = udp.read(buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        // 协议：LHM|<ip>|<port>|<hostname>
        if (strncmp(buf, "LHM|", 4) == 0) {
          String s  = String(buf + 4);
          int    p1 = s.indexOf('|');
          int    p2 = s.indexOf('|', p1 + 1);
          String ip = (p1 > 0) ? s.substring(0, p1) : s;
          uint16_t port = (p2 > p1) ? (uint16_t)s.substring(p1 + 1, p2).toInt() : 8085;
          if (port == 0) port = 8085;
          ip.trim();

          IPAddress cand;
          if (cand.fromString(ip)) {
            bool same = (cand[0] == self[0] && cand[1] == self[1] && cand[2] == self[2]);
            Serial.printf("[发现] 广播 %s:%u 同网段=%d\n", ip.c_str(), port, same ? 1 : 0);
            if (bestIp.length() == 0 || (same && !bestSame)) {
              bestIp = ip; bestPort = port; bestSame = same;
            }
            if (same) break;               // 同网段的就是它，不用再等
          }
        }
      }
    }
    delay(10);
  }
  udp.stop();

  if (bestIp.length() == 0) return false;
  outIp   = bestIp;
  outPort = bestPort;
  return true;
}

static bool discoverHost() {
  // 1) 上次成功过的地址
  String cached = prefs.getString("host", "");
  if (cached.length() > 0) {
    uint16_t p = prefs.getUShort("port", PC_PORT);
    if (probeHost(cached, p, 1500)) { gHost = cached; gPort = p; return true; }
    Serial.println("[发现] 缓存地址失效，重新找");
  }

  // 2) 代码里的兜底地址
  if (probeHost(PC_HOST, PC_PORT, 1200)) { cacheHost(PC_HOST, PC_PORT); return true; }

  // 3) 听 PC 端广播
  String   ip;
  uint16_t port = PC_PORT;
  if (listenBeacon(8000, ip, port) && probeHost(ip, port, 2500)) {
    cacheHost(ip, port);
    return true;
  }

  Serial.println("[发现] 没找到数据源（PC 端 lhm_beacon 没在跑？）");
  return false;
}

bool fetchMetrics(Metrics& out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(2500);
  http.setConnectTimeout(1500);
  String url = String("http://") + gHost + ":" + String(gPort) + "/data.json";
  if (!http.begin(url)) return false;

  uint32_t t0 = millis();
  int code = http.GET();
  out.rttMs = millis() - t0;

  if (code != 200) {
    http.end();
    out.ok = false;
    return false;
  }

  // 注意：这里曾经用 ArduinoJson 的 Filter 想把 30~60KB 裁到几 KB，
  // 但 filter 里的 Children[0] 只会保留数组第一个元素，而且链深只到
  // 「硬件 → Temperatures/Load 分组」这一层，传感器节点（含 Value）全被丢掉，
  // 导致 walkSensors 一个值都取不到、屏幕永远是 PC OFFLINE。
  // 现在改为全量解析：S3 的 512KB SRAM 放得下 60KB JSON（约 150~250KB 峰值）。
  JsonDocument doc;
  // ArduinoJson 默认嵌套上限是 10 层，LHM 的 data.json（根→电脑名→硬件→分组→传感器→Value）
  // 会超，表现为 TooDeep。放宽到 40 层。
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::NestingLimit(40));
  http.end();

  if (err) {
    Serial.printf("JSON 解析失败: %s（若为 NoMemory，可在 LHM 里关掉主板/硬盘/网卡传感器，data.json 会小很多）\n", err.c_str());
    out.ok = false;
    return false;
  }

  Metrics fresh;
  walkSensors(doc.as<JsonVariant>(), fresh, 0, nullptr, nullptr);

  // 第一次拿到型号时打一行，方便确认读对没有
  static bool infoOnce = false;
  if (!infoOnce && (fresh.cpuName[0] || fresh.gpuName[0])) {
    infoOnce = true;
    Serial.printf("[INFO] CPU=%s | GPU=%s | threads=%d cores=%d\n",
                  fresh.cpuName, fresh.gpuName, fresh.nCoreLoad, fresh.nCoreMhz);
  }
  fresh.ok = !isnan(fresh.cpuTemp) || !isnan(fresh.gpuTemp);
  fresh.rttMs = out.rttMs;
  out = fresh;
  return out.ok;
}

// ---------------- 界面 ----------------
// UI_STYLE: 1 = 原始大字版   2 = 卡片仪表盘   3 = 编辑风（对齐 chipiotto.top）
#define UI_STYLE 4

// 配色（RGB565）
#define C_BG     0x0862   // 深底   #0A0E13
#define C_CARD   0x10C4   // 卡片   #111A22
#define C_LINE   0x1946   // 描边   #1E2B36
#define C_GRID   0x2967   // 网格   #2A333D
#define C_PANEL  0x18E3   // 旧版用
#define C_LABEL  0x7BEF   // 次要文字
#define C_DIM    0x5AEB   // 更暗
#define C_WHITE  0xE71C   // 主文字
#define C_GREEN  0x2F34   // 薄荷绿 #2EE6A0
#define C_YELLOW 0xFDC0   // 琥珀   #FFB300
#define C_RED    0xF9C6   // 珊瑚红 #FF3B30
#define C_CYAN   0x05FF   // 天蓝   #00BFFF

// chipiotto.top 设计令牌（白纸 + 墨黑 + 荧光粉）
#define C_PAPER  0xFFFF   // #FFFFFF 纸白
#define C_INK    0x1082   // #111114 墨黑
#define C_GRAY   0x5AEC   // #5F5F63 次要灰
#define C_GRAY2  0x8C52   // #8A8A90 更浅的灰
#define C_EDGE   0xE73D   // #E7E7EC 细分割线
#define C_ACC    0xF96D   // #FF2D6F 荧光粉（点缀）
#define C_ACC2   0xC80A   // #CF0050 深粉（告警文字）

uint16_t tempColor(float t) {
  if (isnan(t)) return C_DIM;
  if (t >= 85) return C_RED;
  if (t >= 70) return C_YELLOW;
  return C_GREEN;
}

// ---------------- 板载 RGB 灯（WS2812, GPIO48）----------------
#define LED_PIN   48
#define LED_FLASH 250      // 换页白闪时长 ms

static uint32_t gLedFlash   = 0;
static bool     gLedOnline  = false;

static void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(LED_PIN, r, g, b);
}

// 温度 -> 颜色（50°C 绿 / 70°C 黄 / 85°C 红），叠加呼吸
static void ledUpdate(float temp, bool online, bool wifi, uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 40) return;
  last = now;

  if (gLedFlash && now < gLedFlash) { ledWrite(55, 55, 55); return; }

  if (!wifi) {                                  // WiFi 断了：蓝色呼吸
    float ph = (now % 2400) / 2400.0f;
    uint8_t v = (uint8_t)(18 + 30 * (0.5f + 0.5f * sinf(ph * 6.2832f)));
    ledWrite(0, 0, v);
    return;
  }
  if (!online) {                                // 数据断了：红色慢闪
    uint8_t v = ((now / 450) & 1) ? 50 : 0;
    ledWrite(v, 0, 0);
    return;
  }

  // 呼吸
  float ph = (now % 3000) / 3000.0f;
  float br = 0.55f + 0.45f * sinf(ph * 6.2832f);
  if (br < 0.2f) br = 0.2f;

  float r = 0, g = 0;
  if (isnan(temp))      { r = 12; g = 12; }
  else if (temp <= 50)  { r = 0;  g = 65; }
  else if (temp <= 70)  { float k = (temp - 50) / 20.0f; r = 65 * k;       g = 65 * (1 - 0.45f * k); }
  else if (temp <= 85)  { float k = (temp - 70) / 15.0f; r = 65;           g = 36 * (1 - k); }
  else                  { r = 75; g = 0; }

  ledWrite((uint8_t)(r * br), (uint8_t)(g * br), 0);
}

// 胶囊进度条
void drawBar(int x, int y, int w, int h, float pct, uint16_t color) {
  if (isnan(pct)) pct = 0;
  pct = constrain(pct, 0, 100);
  int r = h / 2;
  tft.fillRoundRect(x, y, w, h, r, C_GRID);
  int fw = (int)(w * pct / 100.0f + 0.5f);
  if (fw >= h)      tft.fillRoundRect(x, y, fw, h, r, color);
  else if (fw > 0)  tft.fillRect(x, y, fw, h, color);
}

#if UI_STYLE == 1
// ================= 原始版 =================
void drawStatic() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 26, C_PANEL);
  tft.setTextColor(C_WHITE, C_PANEL);
  tft.drawString("PC MONITOR", 8, 6, 2);
  tft.setTextColor(C_LABEL, C_PANEL);
  tft.drawRightString(gHost, 312, 6, 1);

  tft.drawFastVLine(160, 30, 150, C_PANEL);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("CPU", 10, 34, 2);
  tft.drawString("GPU", 170, 34, 2);

  tft.drawFastHLine(0, 182, 320, C_PANEL);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("MEM", 10, 188, 2);
}

void drawValues() {
  char buf[16];
  if (isnan(m.cpuTemp)) {
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString("--", 14, 56, 7);
  } else {
    tft.setTextColor(tempColor(m.cpuTemp), C_BG);
    tft.drawFloat(m.cpuTemp, 0, 12, 56, 7);
  }
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("C", 108, 76, 4);

  if (isnan(m.gpuTemp)) {
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString("--", 174, 56, 7);
  } else {
    tft.setTextColor(tempColor(m.gpuTemp), C_BG);
    tft.drawFloat(m.gpuTemp, 0, 172, 56, 7);
  }
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("C", 268, 76, 4);

  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("LOAD", 10, 118, 1);
  snprintf(buf, sizeof(buf), "%d%%", isnan(m.cpuLoad) ? 0 : (int)m.cpuLoad);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawRightString(buf, 150, 118, 2);
  drawBar(10, 132, 140, 8, m.cpuLoad, C_GREEN);

  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("LOAD", 170, 118, 1);
  snprintf(buf, sizeof(buf), "%d%%", isnan(m.gpuLoad) ? 0 : (int)m.gpuLoad);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawRightString(buf, 310, 118, 2);
  drawBar(170, 132, 140, 8, m.gpuLoad, C_CYAN);

  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("HotSpot", 170, 148, 1);
  if (isnan(m.gpuHot)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0fC", m.gpuHot);
  tft.setTextColor(tempColor(m.gpuHot), C_BG);
  tft.drawRightString(buf, 310, 146, 2);

  snprintf(buf, sizeof(buf), "%d%%", isnan(m.memUsed) ? 0 : (int)m.memUsed);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawRightString(buf, 310, 188, 2);
  drawBar(10, 206, 300, 10, m.memUsed, C_GREEN);

  tft.fillCircle(310, 13, 3, C_GREEN);
  tft.setTextColor(C_LABEL, C_BG);
  snprintf(buf, sizeof(buf), "%dms", m.rttMs);
  tft.drawString(buf, 252, 6, 1);
}

void drawOffline() {
  tft.fillRect(0, 30, 320, 152, C_BG);
  tft.setTextColor(C_RED, C_BG);
  tft.drawString("PC OFFLINE", 60, 90, 4);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "searching data source..." : "wifi reconnecting", 40, 120, 2);
  tft.fillCircle(310, 13, 3, C_RED);
}

#elif UI_STYLE == 2
// ================= 卡片仪表盘 =================
#define CARD_Y    24
#define CARD_H    106
#define CARD_W    152
#define CARD1_X   4
#define CARD2_X   164
#define HIST_Y    132
#define HIST_X0   8
#define HIST_Y0   146
#define HIST_W    304
#define HIST_H    32

#define HIST_N 64
static float   hCpu[HIST_N];
static float   hGpu[HIST_N];
static uint8_t hIdx  = 0;
static bool    hFull = false;

static void histPush(float c, float g) {
  hCpu[hIdx] = isnan(c) ? -100.0f : c;
  hGpu[hIdx] = isnan(g) ? -100.0f : g;
  hIdx = (uint8_t)((hIdx + 1) % HIST_N);
  if (hIdx == 0) hFull = true;
}

static float hLo = 40, hHi = 80;   // 曲线自适应上下限

static int histPy(float t) {
  if (t < 0) return -999;
  if (t < hLo) t = hLo;
  if (t > hHi) t = hHi;
  return HIST_Y0 + HIST_H - 1 - (int)((t - hLo) / (hHi - hLo) * (HIST_H - 2));
}

// 分段刻度条（仪器感）
static void drawSegBar(int x, int y, int w, int h, float pct, uint16_t color) {
  const int N = 16, gap = 2;
  if (isnan(pct)) pct = 0;
  pct = constrain(pct, 0, 100);
  int sw = (w - gap * (N - 1)) / N;
  int lit = (int)(pct / 100.0f * N + 0.5f);
  for (int i = 0; i < N; i++)
    tft.fillRect(x + i * (sw + gap), y, sw, h, i < lit ? color : C_GRID);
}

static void drawHistory() {
  tft.fillRect(HIST_X0, HIST_Y0, HIST_W, HIST_H, C_CARD);
  for (int g = 1; g <= 3; g++)
    tft.drawFastHLine(HIST_X0, HIST_Y0 + HIST_H * g / 4, HIST_W, C_GRID);

  const int step = HIST_W / (HIST_N - 1);
  int n = hFull ? HIST_N : hIdx;

  // 自适应纵轴：取窗口内极值，至少留 8 度跨度，避免曲线拉成直线
  float lo = 999, hi = -999;
  for (int i = 0; i < n; i++) {
    int k = (hIdx + i) % HIST_N;
    if (hCpu[k] > 0) { if (hCpu[k] < lo) lo = hCpu[k]; if (hCpu[k] > hi) hi = hCpu[k]; }
    if (hGpu[k] > 0) { if (hGpu[k] < lo) lo = hGpu[k]; if (hGpu[k] > hi) hi = hGpu[k]; }
  }
  if (hi - lo < 8) { float mid = (hi + lo) * 0.5f; lo = mid - 4; hi = mid + 4; }
  hLo = lo - 1;  hHi = hi + 1;

  for (int i = 0; i + 1 < n; i++) {
    int ia = (hIdx + i) % HIST_N;
    int ib = (hIdx + i + 1) % HIST_N;
    int xa = HIST_X0 + i * step;
    int xb = xa + step;
    int ya = histPy(hCpu[ia]), yb = histPy(hCpu[ib]);
    if (ya > -900 && yb > -900) tft.drawLine(xa, ya, xb, yb, C_GREEN);
    ya = histPy(hGpu[ia]); yb = histPy(hGpu[ib]);
    if (ya > -900 && yb > -900) tft.drawLine(xa, ya, xb, yb, C_CYAN);
  }
}

static void drawCard(int x, float temp, float load, uint16_t accent) {
  char buf[16];
  uint16_t col = tempColor(temp);

  // 清数字区，避免居中位置变化时残留
  tft.fillRect(x + 2, CARD_Y + 16, CARD_W - 4, 56, C_CARD);

  if (isnan(temp)) {
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString("--", x + CARD_W / 2 - 18, CARD_Y + 18, 6);
  } else {
    snprintf(buf, sizeof(buf), "%.0f", temp);
    int w  = tft.textWidth(buf, 6);
    int tx = x + (CARD_W - w) / 2;
    tft.setTextColor(col, C_CARD);
    tft.drawString(buf, tx, CARD_Y + 18, 6);
  }

  // 温度刻度条（20~100 度映射）
  float tp = isnan(temp) ? 0 : (temp - 20.0f) / 80.0f * 100.0f;
  drawSegBar(x + 10, CARD_Y + 70, CARD_W - 20, 6, tp, col);

  if (isnan(load)) snprintf(buf, sizeof(buf), "--");
  else             snprintf(buf, sizeof(buf), "%d%%", (int)(load + 0.5f));
  tft.setTextColor(C_WHITE, C_CARD);
  tft.drawRightString(buf, x + CARD_W - 10, CARD_Y + 80, 2);

  drawBar(x + 10, CARD_Y + 98, CARD_W - 20, 6, load, accent);
}

void drawStatic() {
  tft.fillScreen(C_BG);

  // 顶栏
  tft.fillRect(0, 0, 320, 20, C_CARD);
  tft.drawFastHLine(0, 20, 320, C_LINE);
  tft.fillCircle(10, 10, 3, C_GREEN);
  tft.setTextColor(C_WHITE, C_CARD);
  tft.drawString("PC MONITOR", 20, 3, 2);

  // 两张卡片
  for (int i = 0; i < 2; i++) {
    int x = i ? CARD2_X : CARD1_X;
    tft.fillRoundRect(x, CARD_Y, CARD_W, CARD_H, 8, C_CARD);
    tft.drawRoundRect(x, CARD_Y, CARD_W, CARD_H, 8, C_LINE);
    tft.fillCircle(x + 12, CARD_Y + 14, 3, i ? C_CYAN : C_GREEN);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString(i ? "GPU TEMP" : "CPU TEMP", x + 21, CARD_Y + 7, 2);
    tft.drawString("LOAD", x + 10, CARD_Y + 82, 1);
  }

  // 温度趋势
  tft.fillRoundRect(4, HIST_Y, 312, 48, 8, C_CARD);
  tft.drawRoundRect(4, HIST_Y, 312, 48, 8, C_LINE);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString("TEMP TREND", 12, HIST_Y + 3, 1);
  tft.fillCircle(250, HIST_Y + 7, 2, C_GREEN);
  tft.drawString("CPU", 256, HIST_Y + 3, 1);
  tft.fillCircle(286, HIST_Y + 7, 2, C_CYAN);
  tft.drawString("GPU", 292, HIST_Y + 3, 1);

  // 内存
  tft.drawFastHLine(0, 183, 320, C_LINE);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("MEMORY", 8, 190, 2);
}

void drawValues() {
  char buf[24];
  histPush(m.cpuTemp, m.gpuTemp);

  drawCard(CARD1_X, m.cpuTemp, m.cpuLoad, C_GREEN);
  drawCard(CARD2_X, m.gpuTemp, m.gpuLoad, C_CYAN);
  drawHistory();

  // 内存
  snprintf(buf, sizeof(buf), "%.0f%%", isnan(m.memUsed) ? 0.0f : m.memUsed);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawRightString(buf, 314, 188, 4);
  drawBar(8, 216, 304, 8, m.memUsed, C_GREEN);

  // 底行：CPU 频率 / GPU 热点
  tft.fillRect(0, 228, 320, 12, C_BG);
  tft.setTextColor(C_DIM, C_BG);
  if (isnan(m.cpuClock)) tft.drawString("CLK --", 8, 229, 1);
  else {
    snprintf(buf, sizeof(buf), "CLK %d MHz", (int)m.cpuClock);
    tft.drawString(buf, 8, 229, 1);
  }
  if (isnan(m.gpuHot)) {
    tft.setTextColor(C_DIM, C_BG);
    tft.drawRightString("HOT --", 312, 229, 1);
  } else {
    snprintf(buf, sizeof(buf), "HOT %dC", (int)m.gpuHot);
    tft.setTextColor(tempColor(m.gpuHot), C_BG);
    tft.drawRightString(buf, 312, 229, 1);
  }

  // 顶栏状态
  tft.fillCircle(10, 10, 3, C_GREEN);
  tft.fillRect(272, 2, 44, 16, C_CARD);
  snprintf(buf, sizeof(buf), "%dms", m.rttMs);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawRightString(buf, 312, 7, 1);
}

void drawOffline() {
  for (int i = 0; i < 2; i++) {
    int x = i ? CARD2_X : CARD1_X;
    tft.fillRect(x + 2, CARD_Y + 16, CARD_W - 4, 56, C_CARD);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawString("--", x + CARD_W / 2 - 18, CARD_Y + 18, 6);
    drawSegBar(x + 10, CARD_Y + 70, CARD_W - 20, 6, 0, C_GRID);
    tft.setTextColor(C_DIM, C_CARD);
    tft.drawRightString("--", x + CARD_W - 10, CARD_Y + 80, 2);
    drawBar(x + 10, CARD_Y + 98, CARD_W - 20, 6, 0, C_GRID);
  }
  tft.fillCircle(10, 10, 3, C_RED);
  tft.fillRect(272, 2, 44, 16, C_CARD);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawRightString("--", 312, 7, 1);

  tft.fillRect(HIST_X0, HIST_Y0, HIST_W, HIST_H, C_CARD);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "searching data source..." : "wifi reconnecting",
                 96, HIST_Y0 + 11, 1);
}
#elif UI_STYLE == 3
// ================= 编辑风 · chipiotto.top =================
// 白纸黑字 + 荧光粉点缀 + 等宽标签 + 编号 + 细分割线
#define E_TOP      22
#define E_COL      160
#define E_TREND_Y  144
#define E_TREND_X0 10
#define E_TREND_Y0 160
#define E_TREND_W  300
#define E_TREND_H  24

#define E_N 64
static float   eCpu[E_N];
static float   eGpu[E_N];
static uint8_t eIdx  = 0;
static bool    eFull = false;
static float   eLo   = 40, eHi = 80;

static void ePush(float c, float g) {
  eCpu[eIdx] = isnan(c) ? -100.0f : c;
  eGpu[eIdx] = isnan(g) ? -100.0f : g;
  eIdx = (uint8_t)((eIdx + 1) % E_N);
  if (eIdx == 0) eFull = true;
}

static int ePy(float t) {
  if (t < 0) return -999;
  if (t < eLo) t = eLo;
  if (t > eHi) t = eHi;
  return E_TREND_Y0 + E_TREND_H - 1 - (int)((t - eLo) / (eHi - eLo) * (E_TREND_H - 2));
}

// 只有异常才上色，平时一律墨黑（克制用色）
static uint16_t heatColor(float t) {
  if (isnan(t)) return C_INK;
  if (t >= 86) return C_ACC2;
  if (t >= 72) return C_ACC;
  return C_INK;
}

static void eBar(int x, int y, int w, int h, float pct, uint16_t color) {
  if (isnan(pct)) pct = 0;
  pct = constrain(pct, 0, 100);
  tft.fillRect(x, y, w, h, C_EDGE);
  int fw = (int)(w * pct / 100.0f + 0.5f);
  if (fw > 0) tft.fillRect(x, y, fw, h, color);
}

static void eTemp(int x, float temp) {
  char buf[16];
  uint16_t col = heatColor(temp);
  tft.fillRect(x + 1, 56, 158, 50, C_PAPER);
  if (isnan(temp)) {
    tft.setTextColor(C_GRAY2, C_PAPER);
    tft.drawString("--", x + 10, 58, 6);
    return;
  }
  snprintf(buf, sizeof(buf), "%.0f", temp);
  int w = tft.textWidth(buf, 6);
  tft.setTextColor(col, C_PAPER);
  tft.drawString(buf, x + 10, 58, 6);
  tft.drawCircle(x + 14 + w, 70, 2, col);       // 度符号
  tft.setTextColor(col, C_PAPER);
  tft.drawString("C", x + 20 + w, 64, 2);
}

static void eTrend() {
  tft.fillRect(E_TREND_X0, E_TREND_Y0, E_TREND_W, E_TREND_H, C_PAPER);
  for (int g = 1; g <= 3; g++)
    tft.drawFastHLine(E_TREND_X0, E_TREND_Y0 + E_TREND_H * g / 4, E_TREND_W, C_EDGE);

  int n = eFull ? E_N : eIdx;
  if (n < 2) return;

  float lo = 999, hi = -999;
  for (int i = 0; i < n; i++) {
    int k = (eIdx + i) % E_N;
    if (eCpu[k] > 0) { if (eCpu[k] < lo) lo = eCpu[k]; if (eCpu[k] > hi) hi = eCpu[k]; }
    if (eGpu[k] > 0) { if (eGpu[k] < lo) lo = eGpu[k]; if (eGpu[k] > hi) hi = eGpu[k]; }
  }
  if (hi - lo < 8) { float mid = (hi + lo) * 0.5f; lo = mid - 4; hi = mid + 4; }
  eLo = lo - 1;  eHi = hi + 1;

  const int step = E_TREND_W / (E_N - 1);
  for (int i = 0; i + 1 < n; i++) {
    int ia = (eIdx + i) % E_N;
    int ib = (eIdx + i + 1) % E_N;
    int xa = E_TREND_X0 + i * step;
    int xb = xa + step;
    int ya = ePy(eCpu[ia]), yb = ePy(eCpu[ib]);
    if (ya > -900 && yb > -900) tft.drawLine(xa, ya, xb, yb, C_INK);
    ya = ePy(eGpu[ia]); yb = ePy(eGpu[ib]);
    if (ya > -900 && yb > -900) tft.drawLine(xa, ya, xb, yb, C_ACC);
  }
}

void drawStatic() {
  tft.fillScreen(C_PAPER);

  // 顶栏
  tft.fillCircle(9, 11, 3, C_INK);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawString("CHIPIOTTO / MONITOR", 18, 3, 2);
  tft.drawFastHLine(0, E_TOP, 320, C_EDGE);

  // 左右分栏
  tft.drawFastVLine(E_COL, 26, 114, C_EDGE);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString("01", 10, 26, 1);
  tft.drawString("02", E_COL + 10, 26, 1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawString("CPU TEMP", 10, 38, 2);
  tft.drawString("GPU TEMP", E_COL + 10, 38, 2);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString("LOAD", 10, 110, 1);
  tft.drawString("LOAD", E_COL + 10, 110, 1);

  // 趋势
  tft.drawFastHLine(0, E_TREND_Y, 320, C_EDGE);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString("03", 10, 148, 1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawString("TEMP TREND", 32, 148, 1);
  tft.fillRect(242, 149, 8, 2, C_INK);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString("CPU", 254, 148, 1);
  tft.fillRect(280, 149, 8, 2, C_ACC);
  tft.drawString("GPU", 292, 148, 1);

  // 底部
  tft.drawFastHLine(0, 188, 320, C_EDGE);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString("04", 10, 192, 1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawString("MEMORY", 32, 192, 2);
}

void drawValues() {
  char buf[24];
  ePush(m.cpuTemp, m.gpuTemp);

  // CPU
  eTemp(0, m.cpuTemp);
  snprintf(buf, sizeof(buf), "%3.0f%%", isnan(m.cpuLoad) ? 0.0f : m.cpuLoad);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawRightString(buf, 152, 108, 2);
  eBar(10, 122, 140, 4, m.cpuLoad, C_INK);

  // GPU
  eTemp(E_COL, m.gpuTemp);
  snprintf(buf, sizeof(buf), "%3.0f%%", isnan(m.gpuLoad) ? 0.0f : m.gpuLoad);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawRightString(buf, E_COL + 152, 108, 2);
  eBar(E_COL + 10, 122, 140, 4, m.gpuLoad, C_ACC);

  // 趋势
  eTrend();

  // 内存
  snprintf(buf, sizeof(buf), "%.0f%%", isnan(m.memUsed) ? 0.0f : m.memUsed);
  tft.fillRect(238, 188, 82, 34, C_PAPER);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawRightString(buf, 312, 190, 4);
  eBar(10, 218, 302, 6, m.memUsed, C_INK);

  // 底行
  tft.fillRect(0, 226, 320, 14, C_PAPER);
  tft.setTextColor(C_GRAY, C_PAPER);
  if (isnan(m.cpuClock)) tft.drawString("CLK --", 10, 228, 1);
  else {
    snprintf(buf, sizeof(buf), "CLK %d MHz", (int)m.cpuClock);
    tft.drawString(buf, 10, 228, 1);
  }
  if (isnan(m.gpuHot)) {
    tft.setTextColor(C_GRAY, C_PAPER);
    tft.drawRightString("HOT --", 312, 228, 1);
  } else {
    snprintf(buf, sizeof(buf), "HOT %.0fC", m.gpuHot);
    tft.setTextColor(heatColor(m.gpuHot), C_PAPER);
    tft.drawRightString(buf, 312, 228, 1);
  }

  // 顶栏状态
  tft.fillCircle(9, 11, 3, C_INK);
  tft.fillRect(272, 2, 44, 16, C_PAPER);
  snprintf(buf, sizeof(buf), "%dms", m.rttMs);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawRightString(buf, 312, 7, 1);
}

void drawOffline() {
  tft.fillRect(0, 24, 320, 116, C_PAPER);
  tft.drawFastVLine(E_COL, 26, 114, C_EDGE);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString("--", 10, 58, 6);
  tft.drawString("--", E_COL + 10, 58, 6);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString("NO SIGNAL", 10, 110, 1);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "SEARCHING DATA SOURCE" : "WIFI RECONNECTING",
                 E_COL + 10, 110, 1);
  eBar(10, 122, 140, 4, 0, C_INK);
  eBar(E_COL + 10, 122, 140, 4, 0, C_ACC);
  tft.fillCircle(9, 11, 3, C_INK);
  tft.fillRect(272, 2, 44, 16, C_PAPER);
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawRightString("--", 312, 7, 1);
}
#endif

// ================= 风格 4：多面板轮播 =================
// 每 10 秒自动换一页，右上角圆点指示当前页
//
// 绘制策略（关键，别再改回"先擦后画"）：
//   · 文字：画完新内容后，把「旧宽度 − 新宽度」那一截补成背景色
//           —— 不会残留旧字符，也不会整块刷白造成闪烁
//   · 进度条/柱子：只重画变化的那一段
#define PAGE_MS 10000
#define NPAGE   5

// 翻页擦除动画：每步前进 WIPE_STEP 像素、停顿 WIPE_DELAY_MS 毫秒
// 320 / 8 × (6+0.6) ≈ 265ms；想更慢就调大 DELAY，想更细就调小 STEP
#define WIPE_STEP     8
#define WIPE_DELAY_MS 6

// 页头/标签用的 12x12 小图标（纯几何图形，不占字模）
#define ICON_MONITOR 0
#define ICON_CPU     1
#define ICON_GPU     2
#define ICON_RAM     3
#define ICON_DISK    4
#define ICON_NET     5
#define ICON_BATT    6
#define ICON_POWER   7

static int      gPage     = 0;
static uint32_t gPageAt   = 0;
static bool     gNeedFull = true;

#define NBAR   96
#define NTSLOT 40
static float   gBarPct[NBAR];
static int16_t gTextW[NTSLOT];

static void pResetAll() {
  for (int i = 0; i < NBAR; i++)    gBarPct[i] = -1.0f;
  for (int i = 0; i < NTSLOT; i++)  gTextW[i]  = 0;
}

// 只覆盖字身 +2px，别把旁边的标签一起擦掉
static int pTextH(uint8_t f) {
  if (f <= 1) return 9;
  if (f <= 3) return 13;
  if (f <= 5) return 21;
  return 42;
}

// 左对齐文字：先擦「新旧宽度的并集」，再画
// （只在必要时擦，擦的面积 = 当前宽度，20MHz 下约几毫秒，肉眼无感）
static void pText(int id, int x, int y, uint8_t f, uint16_t col, const char* s) {
  int w = tft.textWidth(s, f);
  int old = (id >= 0 && id < NTSLOT) ? gTextW[id] : 0;
  int cw = (w > old ? w : old) + 4;
  if (cw > 0) tft.fillRect(x, y, cw, pTextH(f), C_PAPER);
  tft.setTextColor(col, C_PAPER);
  tft.drawString(s, x, y, f);
  if (id >= 0 && id < NTSLOT) gTextW[id] = w;
}

// 右对齐文字：先擦「并集」，再右对齐画
static void pTextR(int id, int rightX, int y, uint8_t f, uint16_t col, const char* s) {
  int w = tft.textWidth(s, f);
  int old = (id >= 0 && id < NTSLOT) ? gTextW[id] : 0;
  int cw = (w > old ? w : old) + 4;
  int x = rightX - w;
  if (cw > 0) tft.fillRect(rightX - cw, y, cw, pTextH(f), C_PAPER);
  tft.setTextColor(col, C_PAPER);
  tft.drawString(s, x, y, f);
  if (id >= 0 && id < NTSLOT) gTextW[id] = w;
}

// 横向进度条：首次画整条轨道，之后只补/削差量
static void pBarSet(int id, int x, int y, int w, int h, float pct, uint16_t col) {
  if (id < 0 || id >= NBAR) return;
  if (isnan(pct)) pct = 0;
  pct = constrain(pct, 0.0f, 100.0f);
  int fw = (int)(w * pct / 100.0f + 0.5f);
  if (gBarPct[id] < 0) {
    tft.fillRoundRect(x, y, w, h, h / 2, C_EDGE);
    if (fw > 0) tft.fillRoundRect(x, y, fw, h, h / 2, col);
    gBarPct[id] = pct;
    return;
  }
  int old = (int)(w * gBarPct[id] / 100.0f + 0.5f);
  if (fw == old) return;
  if (fw > old) tft.fillRect(x + old, y, fw - old, h, col);
  else          tft.fillRect(x + fw, y, old - fw, h, C_EDGE);
  gBarPct[id] = pct;
}

// 竖向柱：同样只画差量
static void pVBarSet(int id, int x, int yTop, int w, int h, float pct, uint16_t col) {
  if (id < 0 || id >= NBAR) return;
  if (isnan(pct)) pct = 0;
  pct = constrain(pct, 0.0f, 100.0f);
  int hh = (int)(h * pct / 100.0f + 0.5f);
  if (gBarPct[id] < 0) {
    if (hh > 0) tft.fillRect(x, yTop + h - hh, w, hh, col);
    gBarPct[id] = pct;
    return;
  }
  int old = (int)(h * gBarPct[id] / 100.0f + 0.5f);
  if (hh == old) return;
  if (hh > old) tft.fillRect(x, yTop + h - hh, w, hh - old, col);
  else          tft.fillRect(x, yTop + h - old, w, old - hh, C_PAPER);
  gBarPct[id] = pct;
}

// 12x12 小图标：用矩形 / 圆 / 线段拼出来，不依赖字模
static void drawIcon(uint8_t k, int x, int y, uint16_t c) {
  switch (k) {
    case ICON_MONITOR:                       // 显示器
      tft.drawRect(x + 1, y + 1, 10, 7, c);
      tft.fillRect(x + 5, y + 9, 2, 2, c);
      tft.fillRect(x + 3, y + 11, 6, 1, c);
      break;
    case ICON_CPU:                           // 芯片：外框 + 芯 + 四边引脚
      tft.drawRect(x + 2, y + 2, 8, 8, c);
      tft.fillRect(x + 5, y + 5, 2, 2, c);
      for (int i = 0; i < 3; i++) {
        int px = x + 3 + i * 3;
        tft.fillRect(px, y, 1, 2, c);
        tft.fillRect(px, y + 10, 1, 2, c);
        int py = y + 3 + i * 3;
        tft.fillRect(x, py, 2, 1, c);
        tft.fillRect(x + 10, py, 2, 1, c);
      }
      break;
    case ICON_GPU:                           // 显卡：卡身 + 风扇
      tft.drawRect(x + 1, y + 3, 10, 6, c);
      tft.drawCircle(x + 8, y + 6, 2, c);
      tft.fillRect(x + 3, y + 5, 3, 1, c);
      tft.fillRect(x + 3, y + 8, 3, 1, c);
      break;
    case ICON_RAM:                           // 内存条：长条 + 颗粒分割 + 金手指
      tft.drawRect(x + 1, y + 3, 10, 5, c);
      for (int i = 0; i < 3; i++) tft.fillRect(x + 3 + i * 3, y + 4, 1, 3, C_PAPER);
      tft.fillRect(x + 2, y + 9, 8, 1, c);
      break;
    case ICON_DISK:                          // 硬盘：盘体 + 主轴
      tft.drawRect(x + 2, y + 1, 8, 10, c);
      tft.drawCircle(x + 5, y + 5, 2, c);
      tft.fillRect(x + 8, y + 9, 1, 1, c);
      break;
    case ICON_NET:                           // 网络：三根信号柱
      tft.fillRect(x + 1, y + 7, 2, 4, c);
      tft.fillRect(x + 5, y + 4, 2, 7, c);
      tft.fillRect(x + 9, y + 1, 2, 10, c);
      break;
    case ICON_BATT:                          // 电池
      tft.drawRect(x + 1, y + 3, 9, 6, c);
      tft.fillRect(x + 10, y + 5, 2, 2, c);
      tft.fillRect(x + 2, y + 4, 5, 4, c);
      break;
    case ICON_POWER:                         // 闪电
      tft.drawLine(x + 7, y + 1, x + 3, y + 6, c);
      tft.drawLine(x + 3, y + 6, x + 6, y + 6, c);
      tft.drawLine(x + 6, y + 6, x + 4, y + 11, c);
      tft.drawLine(x + 4, y + 11, x + 9, y + 5, c);
      tft.drawLine(x + 9, y + 5, x + 6, y + 5, c);
      break;
  }
}

static void pHeader(uint8_t icon, const char* title) {
  tft.fillRect(0, 0, 320, 22, C_PAPER);
  drawIcon(icon, 6, 5, C_INK);
  tft.setTextColor(C_INK, C_PAPER);
  tft.drawString(title, 24, 3, 2);
  for (int i = 0; i < NPAGE; i++) {
    int x = 266 + i * 11;
    if (i == gPage) tft.fillCircle(x, 11, 3, C_ACC);
    else            tft.fillCircle(x, 11, 2, C_EDGE);
  }
  tft.drawFastHLine(0, 22, 320, C_EDGE);
}

static void pFooter(const char* note) {
  tft.fillRect(0, 226, 320, 14, C_PAPER);
  tft.drawFastHLine(0, 226, 320, C_EDGE);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString(note, 8, 230, 1);
  char b[16];
  snprintf(b, sizeof(b), "%dms", m.rttMs);
  tft.drawRightString(b, 312, 230, 1);
}

static void pLabel(int x, int y, const char* s) {
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString(s, x, y, 1);
}

// 大号温度 + 度符号（数字用定宽，度符号位置固定）
static void pBigTemp(int slot, int x, int y, float t) {
  char b[8];
  if (isnan(t)) snprintf(b, sizeof(b), "--");
  else          snprintf(b, sizeof(b), "%.0f", t);
  uint16_t col = tempColor(t);
  pText(slot, x, y, 6, col, b);
  int w = tft.textWidth("888", 6);
  tft.drawCircle(x + w + 8, y + 15, 3, col);
  tft.setTextColor(col, C_PAPER);
  tft.drawString("C", x + w + 14, y + 7, 2);
}

static void p1Frame() {
  pHeader(ICON_MONITOR, "PC MONITOR");
  drawIcon(ICON_CPU, 10, 24, C_GRAY2);
  pLabel(26, 28, "CPU TEMP");
  drawIcon(ICON_GPU, 170, 24, C_GRAY2);
  pLabel(186, 28, "GPU TEMP");
  tft.drawFastVLine(160, 26, 84, C_EDGE);
  pLabel(10, 86, "LOAD");
  pLabel(170, 86, "LOAD");
  tft.drawFastHLine(0, 116, 320, C_EDGE);
  drawIcon(ICON_RAM, 10, 119, C_GRAY2);
  pLabel(26, 122, "MEMORY");
  tft.drawFastHLine(0, 188, 320, C_EDGE);
  drawIcon(ICON_POWER, 10, 191, C_GRAY2);
  pLabel(26, 194, "POWER");
  pFooter("AUTO 10s");
  pResetAll();
}

static void p1Values() {
  char b[24];
  pBigTemp(0, 10, 36, m.cpuTemp);
  pBigTemp(1, 170, 36, m.gpuTemp);

  if (isnan(m.cpuLoad)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f%%", m.cpuLoad);
  pTextR(2, 152, 84, 2, C_GRAY, b);
  pBarSet(0, 10, 102, 140, 6, m.cpuLoad, C_INK);

  if (isnan(m.gpuLoad)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f%%", m.gpuLoad);
  pTextR(3, 312, 84, 2, C_GRAY, b);
  pBarSet(1, 170, 102, 140, 6, m.gpuLoad, C_ACC);

  if (isnan(m.memUsed)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f%%", m.memUsed);
  pTextR(4, 312, 118, 4, C_INK, b);
  pBarSet(2, 10, 150, 300, 8, m.memUsed, C_ACC);

  if (!isnan(m.memUsedGB) && !isnan(m.memTotalGB))
    snprintf(b, sizeof(b), "%.1f / %.1f GB", m.memUsedGB, m.memTotalGB);
  else
    snprintf(b, sizeof(b), "--");
  pText(5, 10, 164, 1, C_GRAY, b);

  if (isnan(m.cpuPower)) snprintf(b, sizeof(b), "CPU  --W");
  else snprintf(b, sizeof(b), "CPU %.0fW", m.cpuPower);
  pText(6, 10, 205, 2, C_GRAY, b);

  if (isnan(m.gpuPower)) snprintf(b, sizeof(b), "GPU  --W");
  else snprintf(b, sizeof(b), "GPU %.0fW", m.gpuPower);
  pTextR(7, 312, 205, 2, C_GRAY, b);
}

static void p2Frame() {
  pHeader(ICON_CPU, "CPU  DETAIL");
  int nt = (m.nCoreLoad > 0) ? m.nCoreLoad : 16;
  int nc = (m.nCoreMhz  > 0) ? m.nCoreMhz  : 8;
  if (nt > MAX_CORE) nt = MAX_CORE;
  if (nc > MAX_CORE) nc = MAX_CORE;

  char lb[28];
  snprintf(lb, sizeof(lb), "THREAD LOAD (%d)", nt);
  pLabel(10, 26, lb);
  tft.drawFastHLine(0, 112, 320, C_EDGE);
  snprintf(lb, sizeof(lb), "CORE CLOCK (%d)", nc);
  pLabel(10, 116, lb);

  // 页脚显示真实 CPU 型号（从 LHM 读，不是写死的笔记本型号）
  pFooter(m.cpuName[0] ? m.cpuName : "CPU");

  pResetAll();
  // 刻度线按实际线程/核心数均分
  for (int i = 0; i < nt; i++) {
    int x = 8 + i * 304 / nt;
    int w = 304 / nt - 2; if (w < 2) w = 2;
    tft.drawFastHLine(x, 104, w, C_EDGE);
  }
  for (int i = 0; i < nc; i++) {
    int x = 8 + i * 304 / nc;
    int w = 304 / nc - 4; if (w < 2) w = 2;
    tft.drawFastHLine(x, 174, w, C_EDGE);
  }
}

static void p2Values() {
  char b[24];
  int nt = (m.nCoreLoad > 0) ? m.nCoreLoad : 16;
  int nc = (m.nCoreMhz  > 0) ? m.nCoreMhz  : 8;
  if (nt > MAX_CORE) nt = MAX_CORE;
  if (nc > MAX_CORE) nc = MAX_CORE;

  if (isnan(m.cpuLoad)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f%%", m.cpuLoad);
  pTextR(8, 312, 24, 2, C_INK, b);

  int slot = 304 / nt;
  int bw   = slot - 2; if (bw < 2) bw = 2;
  for (int i = 0; i < nt; i++) {
    float p = (i < m.nCoreLoad && !isnan(m.coreLoad[i])) ? m.coreLoad[i] : 0.0f;
    uint16_t c = (p > 80) ? C_ACC : (p > 35 ? C_INK : C_GRAY);
    pVBarSet(i, 8 + i * slot, 40, bw, 64, p, c);
  }

  // 频率柱的量程按实测最高频率自适应（不再写死 5200MHz）
  float maxMhz = 3500.0f;
  for (int i = 0; i < nc; i++)
    if (!isnan(m.coreMhz[i]) && m.coreMhz[i] > maxMhz) maxMhz = m.coreMhz[i];
  maxMhz *= 1.08f;

  int cslot = 304 / nc;
  int cw    = cslot - 6; if (cw < 4) cw = 4;
  for (int i = 0; i < nc; i++) {
    float mhz = (i < m.nCoreMhz && !isnan(m.coreMhz[i])) ? m.coreMhz[i] : 0.0f;
    pVBarSet(32 + i, 8 + i * cslot, 130, cw, 44, mhz / maxMhz * 100.0f, C_GRAY2);
  }

  if (isnan(m.cpuClock)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f", m.cpuClock);
  pText(9, 10, 184, 4, C_INK, b);

  if (isnan(m.cpuPower)) snprintf(b, sizeof(b), "-- W");
  else snprintf(b, sizeof(b), "%.0f W", m.cpuPower);
  pTextR(10, 312, 184, 4, C_ACC, b);
}

static void p3Frame() {
  pHeader(ICON_GPU, "GPU  DETAIL");
  pLabel(10, 26, "CORE TEMP");
  tft.drawFastVLine(160, 24, 62, C_EDGE);
  pLabel(170, 26, "HOT SPOT");
  pLabel(170, 62, "VRAM TEMP");
  tft.drawFastHLine(0, 112, 320, C_EDGE);
  pLabel(10, 116, "GPU LOAD");
  pLabel(10, 152, "VRAM");
  tft.drawFastHLine(0, 190, 320, C_EDGE);
  pFooter(m.gpuName[0] ? m.gpuName : "GPU");
  pResetAll();
}

static void p3Values() {
  char b[24];
  pBigTemp(11, 10, 36, m.gpuTemp);

  if (isnan(m.gpuHot)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.1f C", m.gpuHot);
  pTextR(12, 312, 40, 2, tempColor(m.gpuHot), b);

  if (isnan(m.gpuMemTemp)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.1f C", m.gpuMemTemp);
  pTextR(13, 312, 76, 2, C_GRAY, b);

  if (isnan(m.gpuLoad)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f%%", m.gpuLoad);
  pTextR(14, 312, 114, 2, C_INK, b);
  pBarSet(0, 10, 132, 300, 6, m.gpuLoad, C_ACC);

  if (!isnan(m.vramUsed) && !isnan(m.vramTotal) && m.vramTotal > 0) {
    snprintf(b, sizeof(b), "%.0f / %.0f MB", m.vramUsed, m.vramTotal);
    pTextR(15, 312, 152, 1, C_GRAY, b);
    pBarSet(1, 10, 170, 300, 6, m.vramUsed / m.vramTotal * 100.0f, C_INK);
  } else {
    pTextR(15, 312, 152, 1, C_GRAY, "--");
  }

  if (isnan(m.gpuClock)) snprintf(b, sizeof(b), "----");
  else snprintf(b, sizeof(b), "%.0f", m.gpuClock);
  pText(16, 10, 196, 4, C_INK, b);

  if (isnan(m.gpuPower)) snprintf(b, sizeof(b), "-- W");
  else snprintf(b, sizeof(b), "%.0f W", m.gpuPower);
  pTextR(17, 312, 196, 4, C_ACC, b);
}

static void p4Frame() {
  pHeader(ICON_DISK, "STORAGE / RAM");
  pLabel(10, 26, "DISK 1");
  pLabel(10, 86, "DISK 2");
  tft.drawFastHLine(0, 146, 320, C_EDGE);
  pLabel(10, 152, "MEMORY");
  pFooter("SSD TEMP / USAGE");
  pResetAll();
}

static void p4Values() {
  char b[24];
  for (int d = 0; d < 2; d++) {
    int y = 26 + d * 60;
    if (m.dkName[d][0]) {
      char nm[14];
      strncpy(nm, m.dkName[d], 13);
      nm[13] = 0;
      pText(18 + d, 52, y, 1, C_INK, nm);
    }
    if (isnan(m.dkT[d])) snprintf(b, sizeof(b), "--");
    else snprintf(b, sizeof(b), "%.0fC", m.dkT[d]);
    pTextR(20 + d, 312, y - 2, 2, tempColor(m.dkT[d]), b);

    pBarSet(d, 10, y + 18, 300, 6, m.dkUse[d], C_GRAY);

    if (isnan(m.dkUse[d])) snprintf(b, sizeof(b), "--");
    else snprintf(b, sizeof(b), "%.0f%% used", m.dkUse[d]);
    pTextR(22 + d, 312, y + 26, 1, C_GRAY, b);

    if (!isnan(m.dkR[d])) {
      snprintf(b, sizeof(b), "R %.0f   W %.0f KB/s", m.dkR[d], isnan(m.dkW[d]) ? 0.0f : m.dkW[d]);
      pText(24 + d, 10, y + 38, 1, C_GRAY2, b);
    }
  }

  if (!isnan(m.memUsedGB))
    snprintf(b, sizeof(b), "%.1f / %.1f GB", m.memUsedGB, isnan(m.memTotalGB) ? 0.0f : m.memTotalGB);
  else
    snprintf(b, sizeof(b), "--");
  pText(26, 10, 168, 4, C_INK, b);

  if (!isnan(m.dimm0)) snprintf(b, sizeof(b), "DIMM %.0f", m.dimm0);
  else snprintf(b, sizeof(b), "--");
  pText(27, 224, 176, 1, C_GRAY, b);

  if (!isnan(m.dimm1)) snprintf(b, sizeof(b), "/ %.0fC", m.dimm1);
  else snprintf(b, sizeof(b), "");
  pText(28, 224, 190, 1, C_GRAY, b);

  pBarSet(2, 10, 210, 300, 6, m.memUsed, C_ACC);
}

static void p5Frame() {
  pHeader(ICON_NET, "NET / POWER");
  drawIcon(ICON_NET, 10, 23, C_GRAY2);
  pLabel(26, 26, "DOWNLOAD");
  tft.drawFastHLine(0, 80, 320, C_EDGE);
  drawIcon(ICON_NET, 10, 83, C_GRAY2);
  pLabel(26, 86, "UPLOAD");
  tft.drawFastHLine(0, 140, 320, C_EDGE);
  drawIcon(ICON_BATT, 10, 143, C_GRAY2);
  pLabel(26, 146, "BATTERY");
  pFooter("LIVE");
  pResetAll();
}

static void p5Values() {
  char b[24];
  if (isnan(m.netDown)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f", m.netDown);
  pText(29, 10, 38, 6, C_INK, b);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString("KB/s", 10 + tft.textWidth("00000", 6) + 8, 62, 1);

  if (isnan(m.netUp)) snprintf(b, sizeof(b), "--");
  else snprintf(b, sizeof(b), "%.0f", m.netUp);
  pText(30, 10, 98, 6, C_ACC, b);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString("KB/s", 10 + tft.textWidth("00000", 6) + 8, 122, 1);

  if (!isnan(m.batPct)) snprintf(b, sizeof(b), "%.0f%%", m.batPct);
  else snprintf(b, sizeof(b), "--");
  pText(31, 10, 158, 6, C_INK, b);

  pBarSet(0, 10, 202, 300, 8, m.batPct, C_GREEN);

  if (!isnan(m.batV)) snprintf(b, sizeof(b), "%.2f V", m.batV);
  else snprintf(b, sizeof(b), "--");
  pText(32, 10, 214, 1, C_GRAY, b);

  if (!isnan(m.batHealth)) snprintf(b, sizeof(b), "HEALTH %.0f%%", 100.0f - m.batHealth);
  else snprintf(b, sizeof(b), "--");
  pTextR(33, 312, 214, 1, C_GRAY, b);
}

static void pFrame(int p) {
  switch (p) {
    case 0: p1Frame(); break;
    case 1: p2Frame(); break;
    case 2: p3Frame(); break;
    case 3: p4Frame(); break;
    default: p5Frame(); break;
  }
}

static void pValues(int p) {
  switch (p) {
    case 0: p1Values(); break;
    case 1: p2Values(); break;
    case 2: p3Values(); break;
    case 3: p4Values(); break;
    default: p5Values(); break;
  }
}

void drawStatic() {
  gPage = 0;
  gPageAt = millis();
  gNeedFull = true;
  tft.fillScreen(C_PAPER);
  pFrame(gPage);
}

// 翻页过渡：粉色扫描线从左往右，把旧页擦成纸白
static void pageWipe() {
  for (int x = 0; x < 320; x += WIPE_STEP) {
    int xx = (x >= 2) ? x - 2 : 0;
    tft.fillRect(xx, 0, WIPE_STEP + 2, 240, C_PAPER);   // 顺带擦掉上一帧的扫描线
    tft.fillRect(x + WIPE_STEP - 2, 0, 2, 240, C_ACC);  // 粉色扫描线
    delay(WIPE_DELAY_MS);
  }
  tft.fillRect(320 - WIPE_STEP, 0, WIPE_STEP, 240, C_PAPER);
}

void drawValues() {
  if (gNeedFull) {
    gNeedFull = false;
    tft.fillScreen(C_PAPER);
    pFrame(gPage);
  }
  if (millis() - gPageAt > PAGE_MS) {
    gPage = (gPage + 1) % NPAGE;
    pageWipe();
    gPageAt = millis();
    gLedFlash = millis() + LED_FLASH;
    pFrame(gPage);
  }
  pValues(gPage);
}

void drawOffline() {
  gNeedFull = true;
  tft.fillScreen(C_PAPER);
  pHeader(ICON_MONITOR, "PC OFFLINE");
  tft.setTextColor(C_GRAY2, C_PAPER);
  tft.drawString("NO SIGNAL", 10, 96, 4);
  tft.setTextColor(C_GRAY, C_PAPER);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "SEARCHING DATA SOURCE" : "WIFI RECONNECTING",
                 10, 140, 1);
  pFooter("RETRY");
}

// ---------------- 串口配网协议（配合 wifi_manager.html）----------------
// 电脑浏览器通过 Web Serial 发命令，ESP32 用 #TAG {json} 回包
//   STATUS          查询当前 WiFi 状态
//   SCAN            扫描附近 WiFi
//   WIFI <ssid>\t<pass>   连接（成功后写 NVS；失败不落盘）
//   FORGET          清除保存的 WiFi
static bool gBusy = false;

#include <esp_wifi.h>

// 最近一次断连原因（事件回调里记录，配网失败时回给网页）
static volatile uint8_t gLastReason = 0;

static const char* wifiReasonName(uint8_t r) {
  switch (r) {
    case 2:   return "AUTH_EXPIRE";
    case 4:   return "ASSOC_EXPIRE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";   // 密码错最常见的原因
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "UNKNOWN";
  }
}

// JSON 字符串转义：SSID 里可能有 " 或 \，不转义整条回包就 JSON.parse 失败
static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char b[7];
          snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

// 连接一个 AP。
//
// 绝对不能先 WiFi.disconnect(true)：它会 esp_wifi_stop() + esp_wifi_deinit()，
// 之后的 esp_wifi_set_config / esp_wifi_connect 全部返回 ESP_ERR_WIFI_NOT_INIT，
// 射频彻底死掉，只能重启恢复（重启后靠 NVS 里的账号"看起来又好了"）。
// 这里只改配置 + connect，断开用 esp_wifi_disconnect()，射频始终保持在线。
//
// 返回值必须检查：不检查的话失败是完全静默的。
static bool staConnect(const String& ssid, const String& pass, wifi_auth_mode_t auth) {
  if (WiFi.getMode() != WIFI_MODE_STA) {   // 射频被关过（旧固件 / 手动 stop）→ 重新 init + start
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  }
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
    Serial.printf("[WIFI] 参数非法 ssid=%u pass=%u\n",
                  (unsigned)ssid.length(), (unsigned)pass.length());
    return false;
  }

  // 驱动停在 connecting 时直接 set_config 会返回 ESP_ERR_WIFI_STATE
  //（Arduino 的 WiFi.disconnect() 在"未连上"时会直接 return，并不会取消连接）
  esp_wifi_disconnect();
  delay(60);

  wifi_config_t cfg = {};                   // 清零：短 SSID / 空密码自动补 0
  memcpy(cfg.sta.ssid,     ssid.c_str(), ssid.length());
  memcpy(cfg.sta.password, pass.c_str(), pass.length());
  // 空密码 = 开放网络，阈值必须是 OPEN，否则永远连不上
  cfg.sta.threshold.authmode = (pass.length() == 0) ? WIFI_AUTH_OPEN : auth;
  // WPA3 必须要 PMF；WPA2 下关掉，兼容 Windows 软 AP 的 802.11w 问题
  cfg.sta.pmf_cfg.capable    = (auth == WIFI_AUTH_WPA3_PSK);
  cfg.sta.pmf_cfg.required   = false;

  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (e == ESP_ERR_WIFI_STATE) {            // 还在 connecting：再掐一次，等一拍重试
    esp_wifi_disconnect();
    delay(200);
    e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  }
  if (e != ESP_OK) {
    Serial.printf("[WIFI] set_config 失败: %s\n", esp_err_to_name(e));
    return false;
  }

  e = esp_wifi_connect();
  if (e != ESP_OK) {
    Serial.printf("[WIFI] connect 失败: %s\n", esp_err_to_name(e));
    return false;
  }
  return true;
}

// WPA2 → WPA3 两段式：Windows 11 热点和普通路由是 WPA2（或 WPA2/WPA3 混合），
// 纯 WPA3 路由只有第二次（SAE + PMF）能连上。
static bool connectWithFallback(const String& ssid, const String& pass, uint32_t waitMs) {
  if (ssid.length() == 0) return false;
  if (pass.length() > 0 && pass.length() < 8) {   // WPA2/WPA3 都要求 8-64 位，别白等 8 秒
    Serial.println("[WIFI] 密码长度必须 8-64 位，直接放弃");
    return false;
  }
  gLastReason = 0;                                 // 只统计这次尝试的原因

  staConnect(ssid, pass, WIFI_AUTH_WPA2_PSK);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) return true;
  if (pass.length() == 0) return false;     // 开放网络没有第二种认证方式

  Serial.printf("[WIFI] WPA2 超时，改试 WPA3(SAE)，status=%d reason=%d\n",
                (int)WiFi.status(), (int)gLastReason);
  staConnect(ssid, pass, WIFI_AUTH_WPA3_PSK);
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

static void sendTag(const char* tag, const String& body) {
  Serial.print(tag);
  Serial.print(' ');
  Serial.println(body);
}

static void cmdStatus() {
  bool on = (WiFi.status() == WL_CONNECTED);
  String j = "{\"connected\":";
  j += on ? "true" : "false";
  j += ",\"ssid\":\"" + (on ? jsonEscape(WiFi.SSID()) : String("")) + "\"";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rssi\":" + String(on ? WiFi.RSSI() : 0);
  j += "}";
  sendTag("#STATUS", j);
}

static void cmdScan() {
  gBusy = true;
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("SCANNING WIFI", 20, 90, 4);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("please wait ~3s", 20, 130, 1);

  // 扫描前必须先掐掉正在进行的重连：
  // 驱动停在 connecting 状态时 scanNetworks() 会直接失败，返回负数，
  // 于是网页看到的永远是 {"nets":[]}（像是"附近没有 WiFi"，其实是没扫成）
  // 已经连上时不用断，直接扫即可；断了会白白掉线
  if (WiFi.getMode() != WIFI_MODE_STA) {     // 射频被关过就重新打开，否则扫不出任何东西
    WiFi.mode(WIFI_STA);
    delay(200);
  }
  bool wasOnline = (WiFi.status() == WL_CONNECTED);
  if (!wasOnline) {
    // 用 esp_wifi_disconnect()：Arduino 的 WiFi.disconnect() 在"未连上"时会直接 return，
    // 取消不了正在进行的连接，扫描会被驱动拒绝
    esp_wifi_disconnect();
    delay(120);
  }

  int n = 0;
  for (int attempt = 0; attempt < 3 && n <= 0; attempt++) {
    if (attempt) delay(350);
    n = WiFi.scanNetworks(false, true);
    Serial.printf("[SCAN] attempt=%d result=%d status=%d\n", attempt, n, (int)WiFi.status());
  }

  if (n < 0) {
    // 扫描失败和"附近真没有 WiFi"要区分开，否则网页只会显示"没扫到网络"
    Serial.printf("[SCAN] 失败 status=%d\n", (int)WiFi.status());
    sendTag("#ERR", "{\"msg\":\"scan failed\"}");
    WiFi.scanDelete();
    gBusy = false;
    drawStatic();
    drawValues();
    return;
  }

  String j = "{\"nets\":[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    String s = jsonEscape(WiFi.SSID(i));
    j += "{\"s\":\"" + s + "\",\"r\":" + String(WiFi.RSSI(i)) +
         ",\"e\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) +
         ",\"a\":" + String((int)WiFi.encryptionType(i)) + "}";
    Serial.printf("[SCAN]   %s  rssi=%d  auth=%d  ch=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  (int)WiFi.encryptionType(i), WiFi.channel(i));
  }
  j += "]}";
  WiFi.scanDelete();
  sendTag("#SCAN", j);

  // 在线扫描偶尔会把连接扫掉，autoReconnect 又是关的，这里补一次
  if (wasOnline && WiFi.status() != WL_CONNECTED && gWifiSsid.length() > 0) {
    Serial.println("[SCAN] 扫描后掉线，重新连接");
    staConnect(gWifiSsid, gWifiPass, WIFI_AUTH_WPA2_PSK);
  }

  gBusy = false;
  drawStatic();
  drawValues();
}

static void cmdWifi(const String& arg) {
  int tab = arg.indexOf('\t');
  String ssid = (tab >= 0) ? arg.substring(0, tab) : arg;
  String pass = (tab >= 0) ? arg.substring(tab + 1) : "";
  // 不要 trim：SSID 允许首尾空格，网页选中的是扫描到的原样名字
  if (ssid.length() == 0) { sendTag("#ERR", "{\"msg\":\"empty ssid\"}"); return; }
  if (ssid.length() > 32)  { sendTag("#ERR", "{\"msg\":\"ssid too long\"}"); return; }
  if (pass.length() > 64)  { sendTag("#ERR", "{\"msg\":\"password too long\"}"); return; }
  if (pass.length() > 0 && pass.length() < 8) {
    sendTag("#ERR", "{\"msg\":\"password must be 8-64 chars\"}");   // 开放网络请留空
    return;
  }

  gUserWifi = true;
  String oldSsid = gWifiSsid;          // 换网失败要退回去，不能让板子一直对着错密码重试
  String oldPass = gWifiPass;
  gWifiSsid = ssid;
  gWifiPass = pass;

  gBusy = true;
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("CONNECTING", 20, 80, 4);
  tft.setTextColor(C_ACC, C_BG);
  tft.drawString(ssid.c_str(), 20, 120, 2);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString(pass.length() ? "trying WPA2 / WPA3..." : "open network", 20, 150, 1);

  // WPA2 → WPA3 两段式；每段最多 8 秒
  bool ok = connectWithFallback(ssid, pass, 8000);

  if (ok) {
    // 连上了才落盘：密码打错不会污染 NVS，重启还能回到原来的网
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    String j = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + "}";
    sendTag("#OK", j);
    tft.fillScreen(C_BG);
    tft.setTextColor(C_GREEN, C_BG);
    tft.drawString("CONNECTED", 20, 90, 4);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString(WiFi.localIP().toString().c_str(), 20, 130, 2);
    // 交给 loop() 去发现：在这里同步跑 discoverHost() 会阻塞最多 9 秒，
    // 期间网页发的 STATUS / FORGET 全部压着不动，看起来就是"点了没反应"
    gNeedDiscover = true;
  } else {
    uint8_t r = gLastReason;
    String msg = "connect failed: ";
    msg += wifiReasonName(r);
    msg += " (";
    msg += String(r);
    msg += ")";
    sendTag("#ERR", "{\"msg\":\"" + msg + "\"}");
    Serial.printf("[WIFI] 连接 %s 失败，status=%d reason=%d\n",
                  ssid.c_str(), (int)WiFi.status(), (int)r);
    tft.fillScreen(C_BG);
    tft.setTextColor(C_RED, C_BG);
    tft.drawString("CONNECT FAIL", 20, 90, 4);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString("check password / signal", 20, 130, 1);

    // 退回原来的账号：连接成功前 NVS 没被改，这里只要把内存里的目标改回去即可
    gWifiSsid = oldSsid;
    gWifiPass = oldPass;
    if (gWifiSsid.length() > 0) {
      Serial.println("[WIFI] 恢复原来的账号");
      staConnect(gWifiSsid, gWifiPass, WIFI_AUTH_WPA2_PSK);
    }
  }
  delay(1200);

  gBusy = false;
  drawStatic();
  fetchMetrics(m);
  drawValues();
}

static void serialPoll() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) {
        if (buf == "STATUS")      cmdStatus();
        else if (buf == "SCAN")   cmdScan();
        else if (buf == "FORGET") {
          prefs.remove("ssid");
          prefs.remove("pass");
          gWifiSsid = "";
          gWifiPass = "";
          esp_wifi_disconnect();          // 断开当前连接，但射频保持在线（还能扫描/重配）
          sendTag("#OK", "{\"msg\":\"forgotten\"}");
          drawOffline();                  // 屏幕上给个反馈，否则看起来像没生效
        }
        else if (buf.startsWith("WIFI ")) cmdWifi(buf.substring(5));
        buf = "";
      }
    } else if (buf.length() < 240) {
      buf += c;
    }
  }
}

// ---------------- 主流程 ----------------
// WiFi 连接相关的底层函数（staConnect / connectWithFallback）在「串口配网协议」一节，
// 因为配网命令和开机流程共用它们。

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n[BOOT] esp32_monitor start");
  Serial.printf("[BOOT] flash=%u psram=%u psramfree=%u\n",
                (unsigned)ESP.getFlashChipSize(),
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreePsram());

  tft.init();
  Serial.println("[BOOT] tft.init done");
  tft.setRotation(3);

#if PERF_SWEEP
  // 开机扫一遍 SPI 时钟，打印各频率的真实全屏耗时（调试用，会闪 4 下白屏）
  {
    const uint32_t rates[] = {20000000UL, 40000000UL, 62500000UL, 80000000UL};
    for (uint8_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
      tft.setFreq(rates[i]);
      uint32_t t0 = micros();
      tft.fillScreen(TFT_WHITE);
      uint32_t us = micros() - t0;
      Serial.printf("[PERF] %2lu MHz -> %5lu us  (%.1f fps)\n",
                    (unsigned long)(rates[i] / 1000000UL), (unsigned long)us,
                    us ? 1000000.0f / (float)us : 0.0f);
    }
    tft.setFreq(TFT_FREQ);   // 回到默认时钟
  }
#endif

  tft.fillScreen(C_BG);
  tft.setTextColor(C_WHITE, C_BG);
  tft.drawString("WiFi...", 10, 100, 4);

  WiFi.mode(WIFI_STA);
  // 国家码设成 CN：允许 1-13 信道发射。
  // 默认国家码会把 12/13 当"只能被动扫描"的信道，热点落在这两个信道上时
  // 就会表现成"搜得到但连不上"（认证帧发不出去，reason=2 AUTH_EXPIRE）
  {
    wifi_country_t cn = {};
    strcpy(cn.cc, "CN");
    cn.schan = 1; cn.nchan = 13;
    // 注意单位是 0.25 dBm，不是 dBm：映射表 {20,5} 即 20 → 5 dBm。
    // 原来写 20 等于把发射功率压到 1/16，远一点就连不上/掉线。
    cn.max_tx_power = 80;              // 80 * 0.25 = 20 dBm
    cn.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_err_t e = esp_wifi_set_country(&cn);
    if (e != ESP_OK) Serial.printf("[BOOT] set_country 失败: %s\n", esp_err_to_name(e));
    esp_wifi_set_max_tx_power(80);     // 再显式设一次，确保不被 PHY 默认值盖回去
  }
  WiFi.setSleep(false);              // 关省电，否则 HTTP 延迟忽高忽低
  WiFi.setAutoReconnect(false);      // 改用 loop() 里自己的 5 秒节流重连，避免驱动卡在 connecting

  // 断连原因诊断：15=4WAY_HANDSHAKE_TIMEOUT(密码错) 201=NO_AP_FOUND 2=AUTH_EXPIRE
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    gLastReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WIFI-EVT] event=%d reason=%d (%s)\n", (int)event,
                  (int)gLastReason, wifiReasonName(gLastReason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  prefs.begin("mon", false);         // 先开 NVS：WiFi 账号可能已经被网页配过
  gWifiSsid = prefs.getString("ssid", WIFI_SSID);
  gWifiPass = prefs.getString("pass", WIFI_PASS);
  Serial.printf("[BOOT] wifi ssid='%s'%s\n", gWifiSsid.c_str(),
                prefs.isKey("ssid") ? " (from NVS)" : " (default)");

  // 先按 WPA2 连；6 秒还没成，再按 WPA3(SAE) 试一次（纯 WPA3 路由只有第二次能连）。
  // 等待期间照常处理串口命令：插上 USB 打开配网页时板子会被复位，
  // 如果这里死等，页面点「扫描」会十几秒没反应，像是坏的。
  bool triedWpa3 = (gWifiSsid.length() == 0) || (gWifiPass.length() == 0);
  if (gWifiSsid.length() > 0) staConnect(gWifiSsid, gWifiPass, WIFI_AUTH_WPA2_PSK);

  uint32_t t0 = millis();
  const uint32_t kBootWait = 12000;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < kBootWait) {
    // 用户在配网（gUserWifi）就不要再拿开机读到的旧账号去试 WPA3，会盖掉他刚设的配置
    if (!triedWpa3 && !gUserWifi && millis() - t0 >= kBootWait / 2) {
      triedWpa3 = true;
      Serial.println("\n[WIFI] WPA2 超时，改试 WPA3(SAE)");
      staConnect(gWifiSsid, gWifiPass, WIFI_AUTH_WPA3_PSK);
    }
    serialPoll();
    delay(100);
    Serial.print(".");
  }

  tft.fillScreen(C_BG);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK  %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi 失败");
    tft.setTextColor(C_RED, C_BG);
    tft.drawString("WiFi FAILED", 20, 100, 4);
    delay(3000);
  }

  // 自动发现数据源。WiFi 没连上时别白等 8 秒 UDP 广播：等 WiFi 起来后 loop() 补一次
  tft.setTextColor(C_LABEL, C_BG);
  if (WiFi.status() == WL_CONNECTED) {
    tft.drawString("FINDING PC...", 20, 100, 4);
    discoverHost();
  } else {
    gNeedDiscover = true;
  }

  drawStatic();
  fetchMetrics(m);
  drawValues();
  lastFetch = millis();
}

void loop() {
  serialPoll();                      // 串口配网命令（网页用）

  // WiFi 掉线重连：5 秒一次。两次重试交替 WPA2 / WPA3，
  // 这样纯 WPA3 路由在断线后也能自己回来。
  //（原版这里 delay(1000) + WiFi.reconnect()，驱动会一直停在 connecting，
  //  串口刷屏 "sta is connecting, return error"，而且 SCAN 永远扫不出来）
  static uint32_t lastReconnect = 0;
  static bool     tryWpa3 = true;    // 取反后用：第一次重试用 WPA2（最常见）
  if (!gBusy && WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    if (gWifiSsid.length() > 0) {
      tryWpa3 = !tryWpa3;
      Serial.printf("[WiFi] retry connect, status=%d %s\n",
                    (int)WiFi.status(), tryWpa3 ? "WPA3" : "WPA2");
      staConnect(gWifiSsid, gWifiPass, tryWpa3 ? WIFI_AUTH_WPA3_PSK : WIFI_AUTH_WPA2_PSK);
    } else if (WiFi.getMode() != WIFI_MODE_STA) {
      WiFi.mode(WIFI_STA);           // 没账号也要保证射频在线，配网页才能扫描
    }
  }

  // WiFi 恢复后补一次数据源发现（开机时 WiFi 没起来会跳过）
  if (!gBusy && gNeedDiscover && WiFi.status() == WL_CONNECTED) {
    gNeedDiscover = false;
    Serial.println("[发现] WiFi 已恢复，重新搜索数据源");
    discoverHost();
    drawStatic();
  }

  if (!gBusy && millis() - lastFetch > REFRESH_MS) {
    lastFetch = millis();
    Metrics fresh;
    if (fetchMetrics(fresh)) {
      m = fresh;
      gLedOnline = true;
      failCount = 0;
      drawValues();
    } else {
      failCount++;
      gLedOnline = false;
      // 连续 5 次失败（约 5 秒）→ 重新自动发现，应对 PC 换 IP / 换机器
      if (failCount >= (5000 / REFRESH_MS > 3 ? 5000 / REFRESH_MS : 3)) {
        failCount = 0;
        drawOffline();
        // WiFi 断着的时候发现是白费功夫（缓存探测必失败 + 8 秒 UDP 空等），
        // 等 WiFi 恢复后由上面的 gNeedDiscover 补一次
        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[发现] 连续失败，重新搜索数据源");
          discoverHost();
          drawStatic();
          fetchMetrics(m);
          drawValues();
        }
      } else {
        drawOffline();
      }
    }
    Serial.printf("CPU %.0fC %.0f%% | GPU %.0fC hot %.0fC %.0f%% | MEM %.0f%% | %dms\n",
                  m.cpuTemp, m.cpuLoad, m.gpuTemp, m.gpuHot, m.gpuLoad, m.memUsed, m.rttMs);
  }

  // 板载 RGB 灯：随温度呼吸 / 断线慢闪 / 换页白闪
  ledUpdate(gLedOnline ? m.cpuTemp : NAN, gLedOnline,
            WiFi.status() == WL_CONNECTED, millis());

  delay(50);
}


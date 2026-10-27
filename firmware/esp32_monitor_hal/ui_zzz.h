/* ============================================================
 *  ui_zzz.h —— 320x240「绝区零 / ZZZ」风格仪表盘
 *
 *  近黑底 #0A0A0A + 酸性荧光绿 #BFFF09 + 白字 + 灰标签
 *  顶部：Z 方块 + ZENLESS ZONE ZERO + LIVE 胶囊
 *  第二行：胶囊页签（ALL / CPU / GPU / MEM / NET）
 *  中间：2×2 卡片，卡外 2px 黑框、内层 #222、右下直角
 *  底部：LIVE / 1 Hz / RTT / IP
 *
 *  这一版是替换原 5 页排版的 UI；数据解析、WiFi、页掩码都不变。
 *  差量策略：每秒只重绘 4 张卡片的内层（#222）和 footer，约 10 ms。
 * ============================================================ */
#pragma once
#include <Arduino.h>
#include "TFT_hal.h"

// ---------- palette (RGB565) ----------
#define ZZZ_BG      0x0841   // #0A0A0A
#define ZZZ_DARK    0x0000   // #000000 frame
#define ZZZ_CARD    0x2104   // #222222
#define ZZZ_TAB     0x10A2   // #141414
#define ZZZ_LINE    0x3186   // #313131
#define ZZZ_TEXT    0xEF5D   // #E8E8E8
#define ZZZ_MUTED   0x9CD3   // #9A9A9A
#define ZZZ_LIME    0xBFE1   // #BFFF09
#define ZZZ_CYAN    0x073F   // #00E5FF
#define ZZZ_RED     0xFA69   // #FF4D4F
#define ZZZ_YELLOW  0xFFE0   // #FBFE00
#define ZZZ_GREEN   0x4EF0   // #4ADE80

// TFT 访问锁：主循环画卡片、marquee 任务画顶栏，两边不能同时碰 SPI
static SemaphoreHandle_t gZzzTftMutex = nullptr;
static inline void zzzTftInit() {
  if (!gZzzTftMutex) gZzzTftMutex = xSemaphoreCreateRecursiveMutex();
}
struct ZzzTftGuard {
  ZzzTftGuard()  { if (gZzzTftMutex) xSemaphoreTakeRecursive(gZzzTftMutex, portMAX_DELAY); }
  ~ZzzTftGuard() { if (gZzzTftMutex) xSemaphoreGiveRecursive(gZzzTftMutex); }
};

#define ZZZ_PAGE_MS 10000
#define ZZZ_CARD_W  148
#define ZZZ_CARD_H  76
#define ZZZ_X1      8
#define ZZZ_X2      164
#define ZZZ_Y1      54
#define ZZZ_Y2      136

struct ZzzCard {
  char     label[16];
  char     idx[4];
  char     value[12];
  char     unit[6];
  char     footL[24];
  char     footR[20];
  uint8_t  font;        // 4 or 6
  uint8_t  pct;         // 0..100, 255 = no bar
  uint16_t valueColor;
  uint16_t barColor;
};

static int      gPage     = 0;
static uint32_t gPageAt   = 0;
static bool     gNeedFull = true;

static const char *ZZZ_TAB_LABELS[PAGE_COUNT] = {"ALL", "CPU", "GPU", "MEM", "NET"};

static uint16_t zzzTempColor(float t) {
  if (isnan(t)) return ZZZ_MUTED;
  if (t >= 85)  return ZZZ_RED;
  if (t >= 70)  return ZZZ_YELLOW;
  return ZZZ_TEXT;
}

// 圆角矩形，但右下角保持直角（参考站的 24 24 0 24）
static void zzzFillRoundBR(int x, int y, int w, int h, int r, uint16_t c) {
  tft.fillRoundRect(x, y, w, h, r, c);
  tft.fillRect(x + w - r, y + h - r, r, r, c);
}

static void zzzSet(ZzzCard &c, const char *label, const char *idx, const char *value,
                  const char *unit, const char *footL, const char *footR,
                  int pct, uint8_t font, uint16_t vcolor, uint16_t bcolor) {
  strncpy(c.label, label, sizeof(c.label) - 1); c.label[sizeof(c.label) - 1] = 0;
  strncpy(c.idx, idx, sizeof(c.idx) - 1);       c.idx[sizeof(c.idx) - 1] = 0;
  strncpy(c.value, value, sizeof(c.value) - 1); c.value[sizeof(c.value) - 1] = 0;
  strncpy(c.unit, unit, sizeof(c.unit) - 1);    c.unit[sizeof(c.unit) - 1] = 0;
  strncpy(c.footL, footL, sizeof(c.footL) - 1); c.footL[sizeof(c.footL) - 1] = 0;
  strncpy(c.footR, footR, sizeof(c.footR) - 1); c.footR[sizeof(c.footR) - 1] = 0;
  c.font = font;
  c.pct = (pct < 0 || pct > 100) ? 255 : (uint8_t)pct;
  c.valueColor = vcolor;
  c.barColor = bcolor;
}

static void zzzNum(ZzzCard &c, const char *label, const char *idx, float v, int dec,
                  const char *unit, const char *footL, const char *footR,
                  float pct, uint8_t font, uint16_t vcolor, uint16_t bcolor) {
  char b[16];
  if (isnan(v)) snprintf(b, sizeof(b), "--");
  else          snprintf(b, sizeof(b), "%.*f", dec, v);
  int p = (isnan(pct) || pct < 0) ? -1 : (int)(pct + 0.5f);
  zzzSet(c, label, idx, b, unit, footL, footR, p, font, vcolor, bcolor);
}

static void zzzTemp(ZzzCard &c, const char *label, const char *idx, float t,
                   const char *footL, const char *footR, float pct) {
  uint16_t vc = zzzTempColor(t);
  uint16_t bc = (t >= 70) ? vc : ZZZ_LIME;
  zzzNum(c, label, idx, t, 0, "C", footL, footR, pct, 6, vc, bc);
}

// ---------------- 页面数据 ----------------
static void zzzBuildCards(uint8_t page, ZzzCard c[4]) {
  char f1[24], f2[24];

  switch (page) {
    case PAGE_OVERVIEW: {
      snprintf(f1, sizeof(f1), "LOAD %.0f%%", isnan(m.cpuLoad) ? 0.0f : m.cpuLoad);
      snprintf(f2, sizeof(f2), "%.0f MHz", isnan(m.cpuClock) ? 0.0f : m.cpuClock);
      zzzTemp(c[0], "CPU TEMP", "01", m.cpuTemp, f1, f2, m.cpuLoad);

      snprintf(f1, sizeof(f1), "LOAD %.0f%%", isnan(m.gpuLoad) ? 0.0f : m.gpuLoad);
      snprintf(f2, sizeof(f2), "%.0f MHz", isnan(m.gpuClock) ? 0.0f : m.gpuClock);
      zzzTemp(c[1], "GPU TEMP", "02", m.gpuTemp, f1, f2, m.gpuLoad);

      snprintf(f1, sizeof(f1), "%.1f / %.1f GB",
               isnan(m.memUsedGB) ? 0.0f : m.memUsedGB,
               isnan(m.memTotalGB) ? 0.0f : m.memTotalGB);
      zzzNum(c[2], "MEMORY", "03", m.memUsed, 0, "%", f1, "USED", m.memUsed, 6, ZZZ_TEXT, ZZZ_LIME);

      snprintf(f1, sizeof(f1), "UP %.0f KB/s", isnan(m.netUp) ? 0.0f : m.netUp);
      snprintf(f2, sizeof(f2), "RTT %dms", m.rttMs);
      zzzNum(c[3], "NETWORK", "04", m.netDown, 1, "KB/s", f1, f2, -1, 4, ZZZ_CYAN, ZZZ_CYAN);
      break;
    }

    case PAGE_CPU: {
      snprintf(f1, sizeof(f1), "POWER %.0f W", isnan(m.cpuPower) ? 0.0f : m.cpuPower);
      zzzTemp(c[0], "CPU TEMP", "01", m.cpuTemp, f1, "PACKAGE",
             isnan(m.cpuTemp) ? -1 : (m.cpuTemp - 20.0f) * 100.0f / 80.0f);

      snprintf(f1, sizeof(f1), "THREADS %d", m.nCoreLoad);
      zzzNum(c[1], "CPU LOAD", "02", m.cpuLoad, 0, "%", f1, "1 SEC", m.cpuLoad, 6, ZZZ_TEXT, ZZZ_LIME);

      zzzNum(c[2], "CLOCK", "03", m.cpuClock, 0, "MHz", "AVERAGE", "ALL CORE",
            isnan(m.cpuClock) ? -1 : m.cpuClock / 50.0f, 4, ZZZ_TEXT, ZZZ_LIME);

      zzzNum(c[3], "POWER", "04", m.cpuPower, 0, "W", "PACKAGE", "SMU",
            isnan(m.cpuPower) ? -1 : m.cpuPower / 1.5f, 6, ZZZ_TEXT, ZZZ_LIME);
      break;
    }

    case PAGE_GPU: {
      snprintf(f1, sizeof(f1), "HOT %.0f", isnan(m.gpuHot) ? 0.0f : m.gpuHot);
      snprintf(f2, sizeof(f2), "VRAM %.0f", isnan(m.gpuMemTemp) ? 0.0f : m.gpuMemTemp);
      zzzTemp(c[0], "GPU TEMP", "01", m.gpuTemp, f1, f2,
             isnan(m.gpuTemp) ? -1 : (m.gpuTemp - 20.0f) * 100.0f / 80.0f);

      snprintf(f1, sizeof(f1), "CORE");
      snprintf(f2, sizeof(f2), "%.0f MHz", isnan(m.gpuClock) ? 0.0f : m.gpuClock);
      zzzNum(c[1], "GPU LOAD", "02", m.gpuLoad, 0, "%", f1, f2, m.gpuLoad, 6, ZZZ_TEXT, ZZZ_LIME);

      float vp = (m.vramTotal > 0 && !isnan(m.vramUsed) && !isnan(m.vramTotal))
                 ? m.vramUsed / m.vramTotal * 100.0f : -1;
      snprintf(f1, sizeof(f1), "%.0f / %.0f MB",
               isnan(m.vramUsed) ? 0.0f : m.vramUsed,
               isnan(m.vramTotal) ? 0.0f : m.vramTotal);
      zzzNum(c[2], "VRAM", "03", vp, 0, "%", f1, "GDDR", vp, 6, ZZZ_TEXT, ZZZ_CYAN);

      snprintf(f1, sizeof(f1), "PACKAGE");
      snprintf(f2, sizeof(f2), "%.0f MHz", isnan(m.gpuClock) ? 0.0f : m.gpuClock);
      zzzNum(c[3], "POWER", "04", m.gpuPower, 0, "W", f1, f2,
            isnan(m.gpuPower) ? -1 : m.gpuPower / 3.0f, 6, ZZZ_TEXT, ZZZ_LIME);
      break;
    }

    case PAGE_STORAGE: {
      snprintf(f1, sizeof(f1), "%.1f / %.1f GB",
               isnan(m.memUsedGB) ? 0.0f : m.memUsedGB,
               isnan(m.memTotalGB) ? 0.0f : m.memTotalGB);
      zzzNum(c[0], "MEMORY", "01", m.memUsed, 0, "%", f1, "USED", m.memUsed, 6, ZZZ_TEXT, ZZZ_LIME);

      for (int d = 0; d < 2; d++) {
        char dn[12];
        snprintf(dn, sizeof(dn), "%.10s", m.dkName[d]);
        if (m.nDisk > d && !isnan(m.dkT[d])) {
          snprintf(f1, sizeof(f1), "USED %.0f%%", isnan(m.dkUse[d]) ? 0.0f : m.dkUse[d]);
          zzzTemp(c[1 + d], d == 0 ? "DISK 1" : "DISK 2", d == 0 ? "02" : "03",
                 m.dkT[d], f1, dn, m.dkUse[d]);
        } else {
          zzzSet(c[1 + d], d == 0 ? "DISK 1" : "DISK 2", d == 0 ? "02" : "03",
                "--", "C", "--", "--", -1, 6, ZZZ_MUTED, ZZZ_MUTED);
        }
      }

      snprintf(f1, sizeof(f1), "DIMM 1 %.0f", isnan(m.dimm1) ? 0.0f : m.dimm1);
      zzzNum(c[3], "DIMM 0", "04", m.dimm0, 0, "C", "SENSOR", f1, -1, 6,
            zzzTempColor(m.dimm0), ZZZ_LIME);
      break;
    }

    default: {   // PAGE_NET
      snprintf(f1, sizeof(f1), "DOWNLOAD");
      snprintf(f2, sizeof(f2), "RTT %dms", m.rttMs);
      zzzNum(c[0], "DOWN", "01", m.netDown, 1, "KB/s", f1, f2, -1, 4, ZZZ_CYAN, ZZZ_CYAN);

      snprintf(f1, sizeof(f1), "UPLOAD");
      zzzNum(c[1], "UP", "02", m.netUp, 1, "KB/s", f1, "LHM", -1, 4, ZZZ_CYAN, ZZZ_CYAN);

      snprintf(f1, sizeof(f1), "%.2f V", isnan(m.batV) ? 0.0f : m.batV);
      snprintf(f2, sizeof(f2), "HEALTH %.0f%%", isnan(m.batHealth) ? 0.0f : 100.0f - m.batHealth);
      zzzNum(c[2], "BATTERY", "03", m.batPct, 0, "%", f1, f2, m.batPct, 6, ZZZ_TEXT, ZZZ_LIME);

      zzzNum(c[3], "RTT", "04", (float)m.rttMs, 0, "ms", "LHM HTTP", "1 Hz", -1, 6, ZZZ_TEXT, ZZZ_CYAN);
      break;
    }
  }
}

// ---------------- 顶部滚动背板 ----------------
#define ZZZ_MARQ_H     7
#define ZZZ_MARQ_COLOR 0x4A49      // #4A4A4A，压在黑底上的一层灰字
static const char *ZZZ_MARQ_TEXT = "ZENLESS ZONE ZERO   ";
static uint16_t zzzMarqBuf[320 * ZZZ_MARQ_H];
static int16_t  zzzMarqX = 0;

static const uint8_t *zzzTinyGlyph(char c) {
  static const uint8_t GZ[7]  = {31, 1, 2, 4, 8, 16, 31};
  static const uint8_t GE[7]  = {31, 16, 30, 16, 16, 16, 31};
  static const uint8_t GN[7]  = {17, 25, 21, 19, 17, 17, 17};
  static const uint8_t GL[7]  = {16, 16, 16, 16, 16, 16, 31};
  static const uint8_t GS[7]  = {15, 16, 16, 14, 1, 1, 30};
  static const uint8_t GO[7]  = {14, 17, 17, 17, 17, 17, 14};
  static const uint8_t GR[7]  = {30, 17, 17, 30, 20, 18, 17};
  static const uint8_t GSP[7] = {0, 0, 0, 0, 0, 0, 0};
  switch (c) {
    case 'Z': return GZ;
    case 'E': return GE;
    case 'N': return GN;
    case 'L': return GL;
    case 'S': return GS;
    case 'O': return GO;
    case 'R': return GR;
    default:  return GSP;
  }
}

static void zzzTinyChar(uint16_t *buf, int x, char c, uint16_t color) {
  const uint8_t *g = zzzTinyGlyph(c);
  for (int r = 0; r < ZZZ_MARQ_H; r++) {
    uint8_t bits = g[r];
    for (int col = 0; col < 5; col++) {
      if (bits & (1 << (4 - col))) {
        int px = x + col;
        if (px >= 0 && px < 320) buf[r * 320 + px] = color;
      }
    }
  }
}

static void zzzTinyString(uint16_t *buf, int x, const char *s, uint16_t color) {
  while (*s) { zzzTinyChar(buf, x, *s++, color); x += 6; }
}

// 每 50 ms 调一次：整条 320x7 一次 pushImage，不闪
void drawMarquee() {
  ZzzTftGuard lock;
  const int w = (int)strlen(ZZZ_MARQ_TEXT) * 6;
  for (int i = 0; i < 320 * ZZZ_MARQ_H; i++) zzzMarqBuf[i] = ZZZ_DARK;
  for (int x = zzzMarqX; x < 320; x += w) {
    zzzTinyString(zzzMarqBuf, x, ZZZ_MARQ_TEXT, ZZZ_MARQ_COLOR);
  }
  tft.pushImage(0, 0, 320, ZZZ_MARQ_H, zzzMarqBuf);
  zzzMarqX--;
  if (zzzMarqX <= -w) zzzMarqX += w;
}

// ---------------- 静态框架 ----------------
static void zzzHeader(bool online) {
  tft.fillRect(0, 0, 320, 27, ZZZ_DARK);

  // logo 方块
  tft.fillRoundRect(8, 7, 14, 14, 3, ZZZ_LIME);
  tft.setTextColor(ZZZ_DARK, ZZZ_LIME);
  {
    int zx = 8 + (14 - tft.textWidth("Z", 2)) / 2;
    tft.drawString("Z", zx, 8, 2);
  }

  // 品牌
  tft.setTextColor(ZZZ_TEXT, ZZZ_DARK);
  tft.drawString("ZENLESS ZONE ZERO", 28, 9, 2);

  // 状态胶囊
  tft.fillRoundRect(272, 7, 40, 15, 7, online ? ZZZ_LIME : ZZZ_TAB);
  tft.setTextColor(online ? ZZZ_DARK : ZZZ_RED, online ? ZZZ_LIME : ZZZ_TAB);
  tft.drawString(online ? "LIVE" : "OFF", online ? 281 : 286, 11, 1);

  tft.fillCircle(260, 14, 3, online ? ZZZ_GREEN : ZZZ_RED);
  tft.drawFastHLine(0, 27, 320, ZZZ_LINE);
}

static void zzzTabs() {
  int n = gPageCount;
  if (n < 1) n = 1;
  int gap = 4;
  int tw = (304 - gap * (n - 1)) / n;
  int x = 8;

  for (int i = 0; i < n; i++) {
    bool active = (i == gPage);
    const char *name = ZZZ_TAB_LABELS[gPageList[i]];

    if (active) {
      tft.fillRoundRect(x, 31, tw, 16, 8, ZZZ_LIME);
      tft.setTextColor(ZZZ_DARK, ZZZ_LIME);
    } else {
      tft.fillRoundRect(x, 31, tw, 16, 8, ZZZ_TAB);
      tft.setTextColor(ZZZ_MUTED, ZZZ_TAB);
    }
    int w = tft.textWidth(name, 1);
    tft.drawString(name, x + (tw - w) / 2, 35, 1);
    x += tw + gap;
  }
}

static void zzzCardFrames() {
  const int xs[4] = {ZZZ_X1, ZZZ_X2, ZZZ_X1, ZZZ_X2};
  const int ys[4] = {ZZZ_Y1, ZZZ_Y1, ZZZ_Y2, ZZZ_Y2};

  for (int i = 0; i < 4; i++) {
    zzzFillRoundBR(xs[i], ys[i], ZZZ_CARD_W, ZZZ_CARD_H, 9, ZZZ_DARK);
    zzzFillRoundBR(xs[i] + 2, ys[i] + 2, ZZZ_CARD_W - 4, ZZZ_CARD_H - 4, 7, ZZZ_CARD);
  }
}

static void zzzFooter(bool online) {
  tft.fillRect(0, 226, 320, 14, ZZZ_BG);
  tft.drawFastHLine(0, 226, 320, ZZZ_LINE);

  tft.fillCircle(9, 233, 2, online ? ZZZ_LIME : ZZZ_RED);
  tft.setTextColor(online ? ZZZ_LIME : ZZZ_RED, ZZZ_BG);
  tft.drawString(online ? "LIVE" : "OFF", 14, 230, 1);

  tft.setTextColor(ZZZ_MUTED, ZZZ_BG);
  tft.drawString("1 Hz", 40, 230, 1);

  char b[20];
  snprintf(b, sizeof(b), "RTT %dms", m.rttMs);
  tft.drawString(b, 66, 230, 1);

  if (WiFi.status() == WL_CONNECTED) {
    tft.drawRightString(WiFi.localIP().toString(), 312, 230, 1);
  } else {
    tft.drawRightString("NO WIFI", 312, 230, 1);
  }
}

// ---------------- 卡片 ----------------
static void zzzDrawCard(int x, int y, const ZzzCard &c) {
  zzzFillRoundBR(x + 2, y + 2, ZZZ_CARD_W - 4, ZZZ_CARD_H - 4, 7, ZZZ_CARD);

  tft.setTextColor(ZZZ_MUTED, ZZZ_CARD);
  tft.drawString(c.label, x + 7, y + 5, 1);

  tft.setTextColor(0x5AEB, ZZZ_CARD);
  tft.drawRightString(c.idx, x + ZZZ_CARD_W - 7, y + 5, 1);

  int vy = (c.font == 6) ? y + 13 : y + 23;
  tft.setTextColor(c.valueColor, ZZZ_CARD);
  tft.drawString(c.value, x + 7, vy, c.font);

  if (c.unit[0]) {
    int vw = tft.textWidth(c.value, c.font);
    int uy = (c.font == 6) ? y + 20 : y + 30;
    if (strcmp(c.unit, "C") == 0) {
      tft.drawCircle(x + 7 + vw + 5, uy + 4, 2, c.barColor);
      tft.setTextColor(c.barColor, ZZZ_CARD);
      tft.drawString("C", x + 7 + vw + 9, uy, 2);
    } else {
      tft.setTextColor(c.barColor, ZZZ_CARD);
      tft.drawString(c.unit, x + 7 + vw + 4, uy, 2);
    }
  }

  tft.setTextColor(ZZZ_MUTED, ZZZ_CARD);
  tft.drawString(c.footL, x + 7, y + ZZZ_CARD_H - 18, 1);

  tft.setTextColor(ZZZ_TEXT, ZZZ_CARD);
  tft.drawRightString(c.footR, x + ZZZ_CARD_W - 7, y + ZZZ_CARD_H - 18, 1);

  if (c.pct <= 100) {
    int by = y + ZZZ_CARD_H - 8;
    int bw = ZZZ_CARD_W - 14;
    tft.fillRect(x + 7, by, bw, 3, ZZZ_LINE);
    int fw = bw * c.pct / 100;
    if (fw > 0) tft.fillRect(x + 7, by, fw, 3, c.barColor);
  }
}

static void zzzDrawCards() {
  ZzzCard c[4];
  zzzBuildCards(gPageList[gPage], c);

  const int xs[4] = {ZZZ_X1, ZZZ_X2, ZZZ_X1, ZZZ_X2};
  const int ys[4] = {ZZZ_Y1, ZZZ_Y1, ZZZ_Y2, ZZZ_Y2};
  for (int i = 0; i < 4; i++) zzzDrawCard(xs[i], ys[i], c[i]);
}

// ---------------- 对外接口 ----------------
static void pageWipe() {
  for (int x = 0; x < 320; x += 8) {
    int xx = (x >= 2) ? x - 2 : 0;
    tft.fillRect(xx, 0, 10, 240, ZZZ_BG);
    tft.fillRect(x + 6, 0, 2, 240, ZZZ_LIME);
    delay(3);
  }
  tft.fillRect(0, 0, 320, 240, ZZZ_BG);
}

void drawStatic() {
  ZzzTftGuard lock;
  gPage = 0;
  gPageAt = millis();
  gNeedFull = false;

  tft.fillScreen(ZZZ_BG);
  zzzHeader(true);
  zzzTabs();
  zzzCardFrames();
  zzzFooter(true);
  zzzDrawCards();
}

void drawValues() {
  ZzzTftGuard lock;
  if (gNeedFull) { drawStatic(); return; }

  if (gPageCount > 1 && millis() - gPageAt > ZZZ_PAGE_MS) {
    gPage = (gPage + 1) % gPageCount;
    pageWipe();
    gPageAt = millis();
    gLedFlash = millis() + LED_FLASH;

    zzzHeader(true);
    zzzTabs();
    zzzCardFrames();
    zzzFooter(true);
    zzzDrawCards();
    return;
  }

  zzzDrawCards();
  zzzFooter(true);
}

void drawOffline() {
  ZzzTftGuard lock;
  gNeedFull = true;

  tft.fillScreen(ZZZ_BG);
  zzzHeader(false);
  zzzTabs();

  tft.fillRect(0, 54, 320, 170, ZZZ_BG);
  tft.fillRect(8, 82, 3, 78, ZZZ_LIME);

  tft.setTextColor(ZZZ_LIME, ZZZ_BG);
  tft.drawString("PC OFFLINE", 18, 78, 4);

  tft.setTextColor(ZZZ_TEXT, ZZZ_BG);
  tft.drawString("NO", 18, 104, 6);
  tft.drawString("SIGNAL", 18, 146, 6);

  tft.setTextColor(ZZZ_MUTED, ZZZ_BG);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "SEARCHING DATA SOURCE" : "WIFI RECONNECTING",
                 18, 196, 1);

  zzzFooter(false);
}



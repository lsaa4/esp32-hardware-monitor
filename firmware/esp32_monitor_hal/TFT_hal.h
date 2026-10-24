/* ============================================================
 *  TFT_hal.h —— TFT_eSPI 的平替实现（走 Arduino SPI，不碰寄存器）
 *
 *  为什么需要它：
 *    TFT_eSPI 为了速度直接读写 SPI 外设寄存器，而 Arduino core 3.3.12 的
 *    SPIClass::begin() 里有 periph_ll_reset()，会延迟清零这些寄存器。
 *    实测：tft.init() 直接卡死；打补丁后 init 能返回，但像素推不出去（白屏）。
 *    而同样的引脚用 SPIClass::writeBytes() 完全正常。
 *
 *  所以：保持 TFT_eSPI 的类名和方法签名，底层用 HAL SPI。
 *
 *  文字：font_aa.h 里是 Segoe UI Bold 转的 4bit 抗锯齿比例字模，
 *        cap 高度 7 / 11 / 19 / 40，对应字号 1 / 2 / 4 / 6（7、8 也走最大的）。
 *        带 alpha 混合，所以小字不再是方块锯齿。
 * ============================================================ */

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "font_aa.h"

// ---------- 引脚（按卖家 8 针表）----------
#define TFT_SCLK 21
#define TFT_MOSI 47
#define TFT_CS   41
#define TFT_DC   40
#define TFT_RST  45
#define TFT_BL   42

#define TFT_FREQ 20000000UL      // 实测：40MHz 杜邦线已到极限，20MHz 稳定，全屏约 60ms

// ---------- 常用颜色（RGB565）----------
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
#define TFT_YELLOW  0xFFE0
#define TFT_CYAN    0x07FF
#define TFT_MAGENTA 0xF81F
#define TFT_ORANGE  0xFD20
#define TFT_GREY    0x5AEB

// 字号 -> 字模表
static inline const FontAA &fontDefAA(uint8_t f) {
  if (f <= 1) return FONTS_AA[0];   // cap 7
  if (f <= 3) return FONTS_AA[1];   // cap 11
  if (f <= 5) return FONTS_AA[2];   // cap 19
  return FONTS_AA[3];               // cap 40
}

// alpha 混合（a: 0~15）
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  if (a == 0)  return bg;
  if (a == 15) return fg;
  uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (15 - a)) / 15;
  uint32_t g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * (15 - a)) / 15;
  uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (15 - a)) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

class TFT_eSPI {
public:
  TFT_eSPI() {}

  void init(uint8_t tc = 0);
  void setRotation(uint8_t r);
  int16_t width()  const { return _w; }
  int16_t height() const { return _h; }

  // ---- 基本图形 ----
  void fillScreen(uint16_t c) { fillRect(0, 0, _w, _h, c); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t c) { fillRect(x, y, w, 1, c); }
  void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t c) { fillRect(x, y, 1, h, c); }
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c);
  void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t c);
  void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t c);
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c);
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c);

  // ---- 文字 ----
  void setTextColor(uint16_t fg)                { _fg = fg; _bgSet = false; }
  void setTextColor(uint16_t fg, uint16_t bg)   { _fg = fg; _bg = bg; _bgSet = true; }
  void drawString(const char *s, int32_t x, int32_t y, uint8_t font = 1);
  void drawString(const String &s, int32_t x, int32_t y, uint8_t font = 1) {
    drawString(s.c_str(), x, y, font);
  }
  void drawRightString(const char *s, int32_t x, int32_t y, uint8_t font = 1);
  void drawRightString(const String &s, int32_t x, int32_t y, uint8_t font = 1) {
    drawRightString(s.c_str(), x, y, font);
  }
  void drawFloat(float v, uint8_t dec, int32_t x, int32_t y, uint8_t font = 1);
  int16_t textWidth(const char *s, uint8_t font = 1) const;
  int16_t textWidth(const String &s, uint8_t font = 1) const {
    return textWidth(s.c_str(), font);
  }
  int16_t fontHeight(uint8_t font = 1) const { return fontDefAA(font).cap; }

  // ---- 低层 ----
  void writecommand(uint8_t c) { cmd(c); }
  void writedata(uint8_t d)    { dat(d); }

private:
  void cmd(uint8_t c);
  void dat(uint8_t d);
  void setWindow(int32_t x, int32_t y, int32_t w, int32_t h);
  void pushColor(uint16_t color, uint32_t count);
  void drawChar(char c, int32_t x, int32_t y, uint8_t font);

  uint16_t _fg = 0xFFFF;
  uint16_t _bg = 0x0000;
  bool     _bgSet = false;
  uint8_t  _rot = 0;
  int16_t  _w = 240;      // 屏原生 240x320 竖屏；rotation 3 时变成 320x240
  int16_t  _h = 320;
};

// ==================== 实现 ====================

static SPIClass _tftSpi(FSPI);

inline void TFT_eSPI::cmd(uint8_t c) {
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  _tftSpi.write(c);
  digitalWrite(TFT_CS, HIGH);
}

inline void TFT_eSPI::dat(uint8_t d) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  _tftSpi.write(d);
  digitalWrite(TFT_CS, HIGH);
}

inline void TFT_eSPI::setWindow(int32_t x, int32_t y, int32_t w, int32_t h) {
  int32_t x1 = x + w - 1, y1 = y + h - 1;
  cmd(0x2A); dat(x >> 8); dat(x & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);
  cmd(0x2B); dat(y >> 8); dat(y & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);
  cmd(0x2C);
}

inline void TFT_eSPI::pushColor(uint16_t color, uint32_t count) {
  uint8_t buf[128];
  for (int i = 0; i < 128; i += 2) { buf[i] = color >> 8; buf[i + 1] = color & 0xFF; }
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  while (count) {
    uint32_t n = (count > 64) ? 64 : count;
    _tftSpi.writeBytes(buf, n * 2);
    count -= n;
  }
  digitalWrite(TFT_CS, HIGH);
}

void TFT_eSPI::init(uint8_t tc) {
  (void)tc;
  pinMode(TFT_BL, OUTPUT);   digitalWrite(TFT_BL, HIGH);
  pinMode(TFT_DC, OUTPUT);   pinMode(TFT_CS, OUTPUT);  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_RST, HIGH);

  _tftSpi.begin(TFT_SCLK, -1, TFT_MOSI, -1);
  _tftSpi.beginTransaction(SPISettings(TFT_FREQ, MSBFIRST, SPI_MODE3));

  // 硬件复位
  digitalWrite(TFT_RST, HIGH); delay(10);
  digitalWrite(TFT_RST, LOW);  delay(30);
  digitalWrite(TFT_RST, HIGH); delay(120);

  // ST7789 初始化（与验证通过的 sw_probe12 完全一致）
  cmd(0x01); delay(150);
  cmd(0x11); delay(120);
  cmd(0x3A); dat(0x55);     // 16 bit/px
  cmd(0x36); dat(0x00);     // 方向: 正常 RGB
  cmd(0x29); delay(20);     // 开显示
}

// 屏原生是 240(宽)x320(高) 竖屏；rotation 1/3 变成 320x240 横屏
// MADCTL: MY=0x80 MX=0x40 MV=0x20
void TFT_eSPI::setRotation(uint8_t r) {
  _rot = r & 3;
  uint8_t m;
  switch (_rot) {
    case 1:  m = 0x60; _w = 320; _h = 240; break;   // MX | MV
    case 2:  m = 0xC0; _w = 240; _h = 320; break;   // MX | MY
    case 3:  m = 0xA0; _w = 320; _h = 240; break;   // MV | MY
    default: m = 0x00; _w = 240; _h = 320; break;
  }
  cmd(0x36); dat(m);
}

void TFT_eSPI::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > _w) w = _w - x;
  if (y + h > _h) h = _h - y;
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, w, h);
  pushColor(c, (uint32_t)w * h);
}

void TFT_eSPI::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
  fillRect(x, y, w, 1, c);
  fillRect(x, y + h - 1, w, 1, c);
  fillRect(x, y, 1, h, c);
  fillRect(x + w - 1, y, 1, h, c);
}

void TFT_eSPI::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) {
  int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int32_t err = dx + dy;
  while (true) {
    fillRect(x0, y0, 1, 1, c);
    if (x0 == x1 && y0 == y1) break;
    int32_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void TFT_eSPI::fillCircle(int32_t cx, int32_t cy, int32_t r, uint16_t c) {
  if (r <= 0) { fillRect(cx, cy, 1, 1, c); return; }
  for (int32_t dy = -r; dy <= r; dy++) {
    int32_t w = (int32_t)sqrtf((float)(r * r - dy * dy));
    fillRect(cx - w, cy + dy, 2 * w + 1, 1, c);
  }
}

void TFT_eSPI::drawCircle(int32_t cx, int32_t cy, int32_t r, uint16_t c) {
  int32_t x = r, y = 0, err = 1 - r;
  while (x >= y) {
    fillRect(cx + x, cy + y, 1, 1, c); fillRect(cx - x, cy + y, 1, 1, c);
    fillRect(cx + x, cy - y, 1, 1, c); fillRect(cx - x, cy - y, 1, 1, c);
    fillRect(cx + y, cy + x, 1, 1, c); fillRect(cx - y, cy + x, 1, 1, c);
    fillRect(cx + y, cy - x, 1, 1, c); fillRect(cx - y, cy - x, 1, 1, c);
    y++;
    if (err < 0) err += 2 * y + 1; else { x--; err += 2 * (y - x) + 1; }
  }
}

void TFT_eSPI::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c) {
  if (r <= 0) { fillRect(x, y, w, h, c); return; }
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  fillRect(x + r, y, w - 2 * r, h, c);
  for (int32_t dy = 0; dy < r; dy++) {
    int32_t inset = r - (int32_t)sqrtf((float)(r * r - (r - dy) * (r - dy)));
    fillRect(x + inset, y + dy, w - 2 * inset, 1, c);
    fillRect(x + inset, y + h - 1 - dy, w - 2 * inset, 1, c);
  }
}

void TFT_eSPI::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c) {
  if (r <= 0) { drawRect(x, y, w, h, c); return; }
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  fillRect(x + r, y, w - 2 * r, 1, c);
  fillRect(x + r, y + h - 1, w - 2 * r, 1, c);
  fillRect(x, y + r, 1, h - 2 * r, c);
  fillRect(x + w - 1, y + r, 1, h - 2 * r, c);
  drawCircle(x + r, y + r, r, c);
  drawCircle(x + w - 1 - r, y + r, r, c);
  drawCircle(x + r, y + h - 1 - r, r, c);
  drawCircle(x + w - 1 - r, y + h - 1 - r, r, c);
}

// ---- 文字：4bit alpha 抗锯齿 + 逐行混合 ----
void TFT_eSPI::drawChar(char ch, int32_t x, int32_t y, uint8_t font) {
  const FontAA &F = fontDefAA(font);
  int idx = (unsigned char)ch - 32;
  if (idx < 0 || idx > 94) idx = '?' - 32;
  const GlyphAA &g = F.g[idx];
  if (g.w == 0 || g.h == 0) return;

  // y 是 cap 顶部；baseline = y + cap；字形顶部 = baseline + ytop
  int32_t left = x + g.xoff;
  int32_t top  = y + F.cap + g.ytop;
  if (left < 0 || top < 0 || left + g.w > _w || top + g.h > _h) {
    // 越界就裁剪到可用范围（简单起见直接裁窗口）
    if (left + g.w <= 0 || top + g.h <= 0) return;
  }

  setWindow(left, top, g.w, g.h);

  const uint8_t *base = F.blob + g.off;
  uint8_t row[128 * 2];                 // 单行最大 128 px
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (int r = 0; r < g.h; r++) {
    int n = 0;
    for (int c = 0; c < g.w; c++) {
      int p = r * g.w + c;
      uint8_t byte = base[p >> 1];
      uint8_t a = (p & 1) ? (byte & 0x0F) : (byte >> 4);
      uint16_t v = blend565(_fg, _bg, a);
      row[n++] = v >> 8;
      row[n++] = v & 0xFF;
    }
    _tftSpi.writeBytes(row, n);
  }
  digitalWrite(TFT_CS, HIGH);
}

void TFT_eSPI::drawString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!s) return;
  const FontAA &F = fontDefAA(font);
  while (*s) {
    int idx = (unsigned char)(*s) - 32;
    if (idx < 0 || idx > 94) idx = '?' - 32;
    drawChar(*s++, x, y, font);
    x += F.g[idx].adv;
  }
}

int16_t TFT_eSPI::textWidth(const char *s, uint8_t font) const {
  if (!s) return 0;
  const FontAA &F = fontDefAA(font);
  int32_t w = 0;
  while (*s) {
    int idx = (unsigned char)(*s) - 32;
    if (idx < 0 || idx > 94) idx = '?' - 32;
    w += F.g[idx].adv;
    s++;
  }
  return (int16_t)w;
}

void TFT_eSPI::drawRightString(const char *s, int32_t x, int32_t y, uint8_t font) {
  if (!s) return;
  drawString(s, x - textWidth(s, font), y, font);
}

void TFT_eSPI::drawFloat(float v, uint8_t dec, int32_t x, int32_t y, uint8_t font) {
  char b[32];
  if (isnan(v)) snprintf(b, sizeof(b), "--");
  else          snprintf(b, sizeof(b), "%.*f", (int)dec, v);
  drawString(b, x, y, font);
}

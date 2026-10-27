/* ============================================================
 *  boot_image.h —— 开机图片（单张 320x240 RGB565）+ 串口上传
 *
 *  文件: /boot.img（ffat）
 *    16 字节头 + 153,600 字节像素（小端 RGB565）
 *    头: "BIMG" | ver=1 | fmt=0 | w=320 | h=240 | crc32 | reserved
 *
 *  串口协议（配合 webui.html）：
 *    INFO                 -> #INFO {"fw":"...","mask":31,"img":true,"free":...}
 *    IMG?                 -> #IMG  {"ok":true,"w":320,"h":240,"bytes":153600,"crc":...,"free":...}
 *    IMG B <bytes> <crc>  -> #OK    （打开 /boot.tmp 并写头）
 *    IMG D <n>            -> #READY （然后 PC 发 n 字节；写完后 #OK）
 *    IMG E                -> #OK    （校验 CRC，替换 /boot.img）
 *    IMG A                -> #OK    （放弃本次上传）
 *    IMG DEL              -> #OK    （删除开机图片）
 *    CFG? / CFG <mask>    -> #CFG / #OK（显示页选择，见 display_config.h）
 *    REBOOT               -> #OK    （重启）
 *
 *  上传分块 + 每块 ACK，避免 115200/921600 下写 flash 时 UART FIFO 溢出。
 * ============================================================ */
#pragma once
#include <Arduino.h>
#include <FFat.h>
#include <esp_heap_caps.h>
#include <AnimatedGIF.h>
#include "TFT_hal.h"

extern TFT_eSPI tft;   // 在 .ino 里定义

#define BOOT_IMG_PATH   "/boot.img"
#define BOOT_IMG_TMP    "/boot.tmp"
#define BOOT_GIF_PATH   "/boot.gif"
#define BOOT_GIF_TMP    "/boot.tmp.gif"
#define BOOT_GIF_MAX    (6 * 1024 * 1024)
#define BOOT_IMG_W      320
#define BOOT_IMG_H      240
#define BOOT_IMG_BYTES  (BOOT_IMG_W * BOOT_IMG_H * 2)
#define BOOT_IMG_TOTAL  (16 + BOOT_IMG_BYTES)

struct BootImgHeader {
  char     magic[4];
  uint8_t  ver;
  uint8_t  fmt;       // 0 = RGB565 little-endian
  uint16_t w;
  uint16_t h;
  uint32_t crc;       // CRC32 of pixel bytes
  uint16_t reserved;
} __attribute__((packed));

static_assert(sizeof(BootImgHeader) == 16, "BootImgHeader must be 16 bytes");

static bool   gFFatOk       = false;
static File   gImgFile;
static bool   gImgUploading = false;
static uint32_t gImgExpectedCrc = 0;
static uint32_t gImgCalcCrc     = 0;
static uint32_t gImgExpectedLen = 0;
static uint32_t gImgReceived    = 0;

static File   gGifFile;
static bool   gGifUploading = false;
static uint32_t gGifExpectedCrc = 0;
static uint32_t gGifCalcCrc     = 0;
static uint32_t gGifExpectedLen = 0;
static uint32_t gGifReceived    = 0;
static AnimatedGIF gAnimatedGif;

// ---------- CRC32 (same as JS crc32) ----------
static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc;
}

static uint32_t crc32Of(const uint8_t *data, size_t len) {
  return ~crc32Update(0xFFFFFFFFUL, data, len);
}

// ---------- ffat ----------
static bool bootFsBegin() {
  gFFatOk = FFat.begin(true, "/ffat", 10, "ffat");   // 首次挂载失败会格式化
  if (gFFatOk) {
    Serial.printf("[FFAT] ok  total=%u KB  free=%u KB\n",
                  (unsigned)(FFat.totalBytes() / 1024), (unsigned)(FFat.freeBytes() / 1024));
  } else {
    Serial.println("[FFAT] mount failed (boot image disabled)");
  }
  return gFFatOk;
}

static uint32_t ffatFree() {
  return gFFatOk ? (uint32_t)FFat.freeBytes() : 0;
}

// ---------- boot image ----------
static bool bootImageExists() {
  if (!gFFatOk) return false;
  if (FFat.exists(BOOT_GIF_PATH)) return true;
  File f = FFat.open(BOOT_IMG_PATH, "r");
  if (!f) return false;

  BootImgHeader h;
  bool ok = (f.read((uint8_t *)&h, sizeof h) == sizeof h) &&
            (memcmp(h.magic, "BIMG", 4) == 0) &&
            (h.ver == 1) && (h.fmt == 0) &&
            (h.w == BOOT_IMG_W) && (h.h == BOOT_IMG_H) &&
            (f.size() == (size_t)BOOT_IMG_TOTAL);
  f.close();
  return ok;
}

// 播放 /boot.gif：cooked 模式解码到 framebuffer，再缩放到 320x240。
// 不直接依赖 GIF 原始尺寸，手机拍/网页存的任意尺寸都能用（上限 640x480）。
static bool bootGifShow() {
  File f = FFat.open(BOOT_GIF_PATH, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz < 10 || sz > BOOT_GIF_MAX) {
    Serial.printf("[GIF] bad size %u\n", (unsigned)sz);
    f.close();
    return false;
  }

  uint8_t *data = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!data) {
    Serial.println("[GIF] no PSRAM for file");
    f.close();
    return false;
  }
  if (f.read(data, sz) != sz) {
    Serial.println("[GIF] short read");
    f.close();
    heap_caps_free(data);
    return false;
  }
  f.close();

  int gw = (int)data[6] | ((int)data[7] << 8);
  int gh = (int)data[8] | ((int)data[9] << 8);
  if (gw < 1 || gh < 1 || gw > 640 || gh > 480) {
    Serial.printf("[GIF] canvas %dx%d unsupported\n", gw, gh);
    heap_caps_free(data);
    return false;
  }

  // cooked：8-bit canvas + 一整帧 RGB565 + 余量
  size_t fbSize = (size_t)gw * gh * 3 + 2048;
  uint8_t *fb = (uint8_t *)heap_caps_malloc(fbSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fb) {
    Serial.println("[GIF] no PSRAM for frame buffer");
    heap_caps_free(data);
    return false;
  }

  gAnimatedGif.begin(GIF_PALETTE_RGB565_LE);
  gAnimatedGif.setDrawType(GIF_DRAW_COOKED);
  gAnimatedGif.setFrameBuf(fb);

  bool played = false;
  if (gAnimatedGif.open(data, (int)sz, nullptr)) {
    int cw = gAnimatedGif.getCanvasWidth();
    int ch = gAnimatedGif.getCanvasHeight();
    Serial.printf("[GIF] %ux%u -> 320x240, %u bytes\n",
                  (unsigned)cw, (unsigned)ch, (unsigned)sz);
    if (cw >= 1 && ch >= 1 && cw <= gw && ch <= gh) {
      tft.fillScreen(TFT_BLACK);
      GIFINFO gi;
      gAnimatedGif.getInfo(&gi);
      int frames = (gi.iFrameCount > 0) ? gi.iFrameCount : 1;
      if (frames < 1) frames = 1;

      static uint16_t line[320];
      int cropW, cropH;
      if ((long)cw * 240 >= (long)ch * 320) {
        cropH = ch;
        cropW = (int)((long)ch * 320 / 240);
      } else {
        cropW = cw;
        cropH = (int)((long)cw * 240 / 320);
      }
      if (cropW > cw) cropW = cw;
      if (cropH > ch) cropH = ch;
      if (cropW < 1) cropW = 1;
      if (cropH < 1) cropH = 1;
      int ox = (cw - cropW) / 2;
      int oy = (ch - cropH) / 2;

      uint32_t start = millis();
      int loops = 0;
      do {
        gAnimatedGif.reset();
        for (;;) {
          int delayMs = 0;
          int more = gAnimatedGif.playFrame(false, &delayMs);
          const uint16_t *cooked = (const uint16_t *)&fb[(size_t)cw * ch];
          for (int dy = 0; dy < 240; dy++) {
            int sy = oy + (int)((uint32_t)dy * cropH / 240);
            if (sy >= ch) sy = ch - 1;
            const uint16_t *src = cooked + (size_t)sy * cw;
            for (int dx = 0; dx < 320; dx++) {
              int sx = ox + (int)((uint32_t)dx * cropW / 320);
              if (sx >= cw) sx = cw - 1;
              line[dx] = src[sx];
            }
            tft.pushImage(0, dy, 320, 1, line);
          }
          if (delayMs > 0) delay(delayMs > 1000 ? 1000 : delayMs);
          if (!more) break;
        }
        loops++;
      } while (frames > 1 && loops < 30 && millis() - start < 3000);

      Serial.printf("[GIF] played %d loops, %d frames\n", loops, frames);
      played = true;
    } else {
      Serial.println("[GIF] canvas mismatch");
    }
    gAnimatedGif.close();
  } else {
    Serial.printf("[GIF] open failed err=%d\n", (int)gAnimatedGif.getLastError());
  }

  heap_caps_free(fb);
  heap_caps_free(data);
  return played;
}

// 在屏幕上画开机图片。成功返回 true；失败（没图/格式不对/CRC 错）返回 false。
static bool bootImageShow() {
  if (!gFFatOk) return false;
  if (FFat.exists(BOOT_GIF_PATH)) {
    if (bootGifShow()) return true;
    Serial.println("[BOOT] gif failed, try static image");
  }

  File f = FFat.open(BOOT_IMG_PATH, "r");
  if (!f) return false;

  BootImgHeader h;
  if (f.read((uint8_t *)&h, sizeof h) != sizeof h ||
      memcmp(h.magic, "BIMG", 4) != 0 ||
      h.ver != 1 || h.fmt != 0 ||
      h.w != BOOT_IMG_W || h.h != BOOT_IMG_H ||
      f.size() != (size_t)BOOT_IMG_TOTAL) {
    f.close();
    Serial.println("[BOOT] invalid image header");
    return false;
  }

  // 优先放进 PSRAM 一次校验、再分 8 条推屏；PSRAM 不够时退回 16 行条带流式显示。
  uint8_t *buf = (uint8_t *)heap_caps_malloc(BOOT_IMG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf) {
    size_t got = f.read(buf, BOOT_IMG_BYTES);
    f.close();
    if (got != (size_t)BOOT_IMG_BYTES) {
      heap_caps_free(buf);
      Serial.println("[BOOT] short read");
      return false;
    }
    uint32_t crc = crc32Of(buf, BOOT_IMG_BYTES);
    if (crc != h.crc) {
      heap_caps_free(buf);
      Serial.printf("[BOOT] crc mismatch got=%08X want=%08X\n", (unsigned)crc, (unsigned)h.crc);
      return false;
    }
    Serial.printf("[BOOT] image %ux%u psram, crc ok\n", (unsigned)h.w, (unsigned)h.h);
    for (int y = 0; y < BOOT_IMG_H; y += 30) {
      int rows = (BOOT_IMG_H - y > 30) ? 30 : (BOOT_IMG_H - y);
      tft.pushImage(0, y, BOOT_IMG_W, rows,
                    (const uint16_t *)(buf + (size_t)y * BOOT_IMG_W * 2));
      delay(8);                       // 约 60 ms 的逐条展开，避免“啪”一下整屏出现
    }
    heap_caps_free(buf);
    return true;
  }

  // 没有 PSRAM 可用：16 行一条，边读边推；CRC 在结束时校验。
  static uint16_t band[BOOT_IMG_W * 16];
  uint32_t crc = 0xFFFFFFFFUL;
  int y = 0;
  while (y < BOOT_IMG_H) {
    int rows = (BOOT_IMG_H - y > 16) ? 16 : (BOOT_IMG_H - y);
    size_t n = (size_t)rows * BOOT_IMG_W * 2;
    if (f.read((uint8_t *)band, n) != n) { f.close(); return false; }
    crc = crc32Update(crc, (const uint8_t *)band, n);
    tft.pushImage(0, y, BOOT_IMG_W, rows, band);
    y += rows;
    delay(8);
  }
  f.close();

  if (~crc != h.crc) {
    Serial.println("[BOOT] crc mismatch (streamed)");
    tft.fillScreen(TFT_BLACK);
    return false;
  }
  Serial.printf("[BOOT] image %ux%u stream, crc ok\n", (unsigned)h.w, (unsigned)h.h);
  return true;
}

// 开机时盖在图片底部的一条状态栏（不破坏图片主体）
static void bootStatus(const char *text, uint16_t color) {
  tft.fillRect(0, 226, 320, 14, TFT_BLACK);
  tft.drawFastHLine(0, 226, 320, 0x4208);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(text, 8, 230, 1);
}

// ---------- 串口上传 ----------
static void imgErr(const char *msg) {
  Serial.print("#ERR {\"msg\":\"");
  Serial.print(msg);
  Serial.println("\"}");
}

static void imgInfo() {
  bool ok = false;
  uint32_t crc = 0;
  uint32_t bytes = BOOT_IMG_BYTES;
  bool isGif = false;
  if (gFFatOk) {
    File gf = FFat.open(BOOT_GIF_PATH, "r");
    if (gf) {
      if (gf.size() >= 6) {
        ok = true;
        isGif = true;
        bytes = (uint32_t)gf.size();
      }
      gf.close();
    } else {
      File f = FFat.open(BOOT_IMG_PATH, "r");
      if (f) {
        BootImgHeader h;
        if (f.read((uint8_t *)&h, sizeof h) == sizeof h &&
            memcmp(h.magic, "BIMG", 4) == 0 &&
            h.ver == 1 && h.fmt == 0 &&
            h.w == BOOT_IMG_W && h.h == BOOT_IMG_H &&
            f.size() == (size_t)BOOT_IMG_TOTAL) {
          ok = true;
          crc = h.crc;
        }
        f.close();
      }
    }
  }

  Serial.print("#IMG {\"ok\":");
  Serial.print(ok ? "true" : "false");
  Serial.print(",\"gif\":");
  Serial.print(isGif ? "true" : "false");
  Serial.print(",\"w\":");
  Serial.print(BOOT_IMG_W);
  Serial.print(",\"h\":");
  Serial.print(BOOT_IMG_H);
  Serial.print(",\"bytes\":");
  Serial.print(bytes);
  Serial.print(",\"free\":");
  Serial.print(ffatFree());
  if (ok) {
    Serial.print(",\"crc\":");
    Serial.print(crc);
  }
  Serial.println("}");
}

static void imgAbort() {
  if (gImgUploading) {
    gImgFile.flush();
    gImgFile.close();
    gImgUploading = false;
  }
  if (gFFatOk && FFat.exists(BOOT_IMG_TMP)) FFat.remove(BOOT_IMG_TMP);
  gImgReceived = 0;
}

static void imgBegin(uint32_t bytes, uint32_t crc) {
  if (!gFFatOk) { imgErr("ffat not mounted"); return; }
  if (bytes != (uint32_t)BOOT_IMG_BYTES) { imgErr("bad length"); return; }

  imgAbort();

  gImgFile = FFat.open(BOOT_IMG_TMP, "w");
  if (!gImgFile) { imgErr("open tmp failed"); return; }

  BootImgHeader h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "BIMG", 4);
  h.ver = 1;
  h.fmt = 0;
  h.w = BOOT_IMG_W;
  h.h = BOOT_IMG_H;
  h.crc = crc;

  if (gImgFile.write((const uint8_t *)&h, sizeof h) != sizeof h) {
    gImgFile.close();
    FFat.remove(BOOT_IMG_TMP);
    imgErr("write header failed");
    return;
  }

  gImgUploading  = true;
  gImgExpectedCrc = crc;
  gImgExpectedLen = bytes;
  gImgReceived    = 0;
  gImgCalcCrc     = 0xFFFFFFFFUL;
  Serial.println("#OK {\"msg\":\"ready\"}");
}

static void imgData(uint32_t n) {
  if (!gImgUploading) { imgErr("no upload"); return; }
  if (n == 0 || n > 4096) { imgErr("bad chunk"); return; }
  if (gImgReceived + n > gImgExpectedLen) { imgErr("too much data"); imgAbort(); return; }

  Serial.println("#READY");

  static uint8_t buf[4096];
  Serial.setTimeout(5000);
  size_t got = Serial.readBytes(buf, n);
  if (got != n) {
    imgErr("timeout");
    imgAbort();
    return;
  }

  if (gImgFile.write(buf, n) != n) {
    imgErr("write failed");
    imgAbort();
    return;
  }

  gImgCalcCrc = crc32Update(gImgCalcCrc, buf, n);
  gImgReceived += n;

  Serial.print("#OK {\"n\":");
  Serial.print(gImgReceived);
  Serial.println("}");
}

static void imgEnd() {
  if (!gImgUploading) { imgErr("no upload"); return; }

  gImgFile.flush();
  gImgFile.close();
  gImgUploading = false;

  uint32_t crc = ~gImgCalcCrc;

  if (gImgReceived != gImgExpectedLen) {
    FFat.remove(BOOT_IMG_TMP);
    imgErr("length mismatch");
    return;
  }
  if (crc != gImgExpectedCrc) {
    FFat.remove(BOOT_IMG_TMP);
    imgErr("crc mismatch");
    return;
  }

  // FatFs 的 rename 不会覆盖已有文件，先删旧的再改名。
  if (FFat.exists(BOOT_IMG_PATH)) FFat.remove(BOOT_IMG_PATH);
  if (!FFat.rename(BOOT_IMG_TMP, BOOT_IMG_PATH)) {
    imgErr("rename failed");
    return;
  }
  // 静态图优先替换掉之前的 GIF，避免下次开机还在播旧动画
  if (FFat.exists(BOOT_GIF_PATH)) FFat.remove(BOOT_GIF_PATH);

  Serial.print("#OK {\"msg\":\"saved\",\"crc\":");
  Serial.print(crc);
  Serial.println("}");
}

static void gifAbort();

static void imgDelete() {
  imgAbort();
  gifAbort();
  if (!gFFatOk) { imgErr("ffat not mounted"); return; }
  if (FFat.exists(BOOT_IMG_PATH)) FFat.remove(BOOT_IMG_PATH);
  if (FFat.exists(BOOT_GIF_PATH)) FFat.remove(BOOT_GIF_PATH);
  Serial.println("#OK {\"msg\":\"deleted\"}");
}

// ---------- GIF 上传（原样存 /boot.gif，开机 AnimatedGIF 播放） ----------
static void gifAbort() {
  if (gGifFile) {
    gGifFile.flush();
    gGifFile.close();
  }
  gGifUploading = false;
  if (gFFatOk && FFat.exists(BOOT_GIF_TMP)) FFat.remove(BOOT_GIF_TMP);
  gGifReceived = 0;
}

static void gifBegin(uint32_t bytes, uint32_t crc) {
  if (!gFFatOk) { imgErr("ffat not mounted"); return; }
  if (bytes < 6) { imgErr("gif too small"); return; }
  if (bytes > BOOT_GIF_MAX) { imgErr("gif too large"); return; }
  if (bytes > ffatFree()) { imgErr("no ffat space"); return; }

  gifAbort();
  gGifFile = FFat.open(BOOT_GIF_TMP, "w");
  if (!gGifFile) { imgErr("open gif tmp failed"); return; }

  gGifUploading  = true;
  gGifExpectedCrc = crc;
  gGifExpectedLen = bytes;
  gGifReceived    = 0;
  gGifCalcCrc     = 0xFFFFFFFFUL;
  Serial.println("#OK {\"msg\":\"gif ready\"}");
}

static void gifData(uint32_t n) {
  if (!gGifUploading) { imgErr("no gif upload"); return; }
  if (n == 0 || n > 4096) { imgErr("bad gif chunk"); return; }
  if (gGifReceived + n > gGifExpectedLen) { imgErr("too much gif data"); gifAbort(); return; }

  Serial.println("#READY");

  static uint8_t buf[4096];
  Serial.setTimeout(5000);
  size_t got = Serial.readBytes(buf, n);
  if (got != n) {
    imgErr("gif timeout");
    gifAbort();
    return;
  }

  if (gGifFile.write(buf, n) != n) {
    imgErr("gif write failed");
    gifAbort();
    return;
  }

  gGifCalcCrc = crc32Update(gGifCalcCrc, buf, n);
  gGifReceived += n;

  Serial.print("#OK {\"n\":");
  Serial.print(gGifReceived);
  Serial.println("}");
}

static void gifEnd() {
  if (!gGifUploading) { imgErr("no gif upload"); return; }
  gGifFile.flush();
  gGifFile.close();
  gGifUploading = false;

  uint32_t crc = ~gGifCalcCrc;
  Serial.printf("[GIF] crc got=%08X want=%08X len=%u/%u\n",
                (unsigned)crc, (unsigned)gGifExpectedCrc,
                (unsigned)gGifReceived, (unsigned)gGifExpectedLen);
  if (gGifReceived != gGifExpectedLen) {
    FFat.remove(BOOT_GIF_TMP);
    imgErr("gif length mismatch");
    return;
  }
  if (crc != gGifExpectedCrc) {
    FFat.remove(BOOT_GIF_TMP);
    imgErr("gif crc mismatch");
    return;
  }

  if (FFat.exists(BOOT_GIF_PATH)) FFat.remove(BOOT_GIF_PATH);
  if (!FFat.rename(BOOT_GIF_TMP, BOOT_GIF_PATH)) {
    imgErr("gif rename failed");
    return;
  }

  Serial.print("#OK {\"msg\":\"gif saved\",\"crc\":");
  Serial.print(crc);
  Serial.println("}");
}

static void gifDelete() {
  gifAbort();
  if (!gFFatOk) return;
  if (FFat.exists(BOOT_GIF_PATH)) FFat.remove(BOOT_GIF_PATH);
}

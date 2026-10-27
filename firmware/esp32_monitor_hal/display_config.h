/* ============================================================
 *  display_config.h —— 显示内容选择（页级）
 *
 *  只做“哪些页参与轮播”，不碰每页的排版。
 *  配置存在 NVS 的 "pmask" 里，每 bit 对应一页：
 *      bit0 总览  bit1 CPU  bit2 GPU  bit3 存储  bit4 网络
 *
 *  这是给后续 UI 重构留的最小接口：重构时只要继续读 gPageMask /
 *  gPageList，页面渲染函数可以整体换掉。
 * ============================================================ */
#pragma once
#include <Arduino.h>
#include <Preferences.h>

enum PageId : uint8_t {
  PAGE_OVERVIEW = 0,
  PAGE_CPU,
  PAGE_GPU,
  PAGE_STORAGE,
  PAGE_NET,
  PAGE_COUNT
};

#define PAGE_MASK_ALL ((uint16_t)((1u << PAGE_COUNT) - 1u))

extern Preferences prefs;   // 在 .ino 里定义，必须早于本文件 include

static uint16_t gPageMask = PAGE_MASK_ALL;
static uint8_t  gPageList[PAGE_COUNT] = {PAGE_OVERVIEW, PAGE_CPU, PAGE_GPU, PAGE_STORAGE, PAGE_NET};
static uint8_t  gPageCount = PAGE_COUNT;

// 按掩码重建轮播顺序（固定顺序，不做拖动排序）
static void pagesRebuild() {
  gPageCount = 0;
  for (uint8_t i = 0; i < PAGE_COUNT; i++)
    if (gPageMask & (uint16_t)(1u << i)) gPageList[gPageCount++] = i;

  if (gPageCount == 0) {              // 全关时退回总览，避免除零/空屏
    gPageMask = (uint16_t)(1u << PAGE_OVERVIEW);
    gPageList[0] = PAGE_OVERVIEW;
    gPageCount = 1;
  }
}

static void pagesLoad() {
  gPageMask = prefs.getUShort("pmask", PAGE_MASK_ALL);
  pagesRebuild();
}

static bool pagesSave(uint16_t mask) {
  mask &= PAGE_MASK_ALL;
  if (mask == 0) return false;
  gPageMask = mask;
  pagesRebuild();
  prefs.putUShort("pmask", gPageMask);
  return true;
}

static bool pageEnabled(PageId p) {
  return (gPageMask >> (uint8_t)p) & 1u;
}

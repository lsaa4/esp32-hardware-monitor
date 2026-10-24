/* ============================================================
 *  secrets.example.h —— WiFi 配置模板
 *
 *  用法：复制本文件为 secrets.h，填入自己的 WiFi：
 *
 *      copy secrets.example.h secrets.h
 *
 *  secrets.h 已写进 .gitignore，不会被提交到仓库。
 *  如果不想建 secrets.h，也可以烧录后打开 wifi_manager.html
 *  通过 USB 串口配网，账号会存进板子的 NVS，效果一样。
 * ============================================================ */
#pragma once

#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"

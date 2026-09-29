#pragma once
#include <Arduino.h>

// ============================================================
// mbedtls 分配器运行时钩子（v3.55）：TLS 大块路由 PSRAM
//   背景：固件 sdkconfig 是 MBEDTLS_INTERNAL_MEM_ALLOC=y——mbedtls 的全部分配被
//   强制进内部 RAM（优先级高于 SPIRAM_USE_MALLOC 的路由），一条 TLS 连接要
//   ~42KB 内部堆峰值（官方表：默认 42196B），是自愈性打盹（maxblk 跌破 26KB）
//   的主要推手。Arduino 预编译内核改不了 menuconfig，但 IDF 的默认分配器是
//   编译期函数指针初始值（esp_mem.c），不在每次连接时重新注册——setup 里
//   mbedtls_platform_set_calloc_free 换一次即全局生效。
//   策略：size ≥ 6KB（TLS 收发缓冲各 ~17.5KB、会话票据等）→ PSRAM；
//         小块（WiFi EAPOL 加密、证书解析、MPI 运算数）→ 维持内部 RAM，性能不变。
//   S3 的 AES/SHA/MPI 硬件加速经 GDMA 可访问 PSRAM（IDF 官方 mbedtls
//   "External SPIRAM" 分配模式即同一路径）。PSRAM 不足时自动退回内部 RAM（旧行为）。
//   生效自证：首个 ≥10KB 的 PSRAM 路由打一行日志；计数从 /api/status 的
//   tlsroute_kb 读（0 = 钩子未生效或还没走过 TLS）。
// ============================================================

void laapTlsHeapInit();             // setup 最早处调用一次（任何 TLS 之前）
uint32_t laapTlsHeapRouteKb();      // 已路由 PSRAM 的累计 KB（诊断）
uint32_t laapTlsHeapRouteCount();   // 已路由 PSRAM 的分配笔数

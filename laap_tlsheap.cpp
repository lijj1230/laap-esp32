#include "laap_tlsheap.h"
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

static uint32_t s_routeBytes = 0;
static uint32_t s_routeCount = 0;
static bool s_firstLogged = false;

// mbedtls 的 calloc 语义：返回的内存必须全零（heap_caps_calloc 本身清零）
static void* laapMbedCalloc(size_t n, size_t sz) {
  if (n && sz && n > SIZE_MAX / sz) return nullptr;   // 乘法溢出：按分配失败处理
  size_t total = n * sz;
  if (total >= 6144) {
    void* p = heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
      s_routeBytes += (uint32_t)total; s_routeCount++;
      if (!s_firstLogged && total >= 10240) {         // 首个 ≥10KB 大块：证明钩子真的接管了
        s_firstLogged = true;
        Serial.println("[TLSHEAP] mbedtls 大块已路由 PSRAM（TLS 收发缓冲不再压内部堆）");
      }
      return p;
    }   // PSRAM 拿不到：退回内部 RAM = 回到旧行为，功能不中断
  }
  return heap_caps_calloc(1, total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// heap_caps_free 兼容内部 RAM 与 PSRAM 指针，无需区分来源
static void laapMbedFree(void* p) { heap_caps_free(p); }

void laapTlsHeapInit() {
  static bool done = false;
  if (done) return;
  done = true;
  // 必须在任何 TLS 活动之前（setup 最早处）；注册失败则维持默认内部 RAM 分配
  if (mbedtls_platform_set_calloc_free(laapMbedCalloc, laapMbedFree) != 0)
    Serial.println("[TLSHEAP] 钩子注册失败，维持默认内部 RAM 分配");
}

uint32_t laapTlsHeapRouteKb() { return s_routeBytes / 1024; }
uint32_t laapTlsHeapRouteCount() { return s_routeCount; }

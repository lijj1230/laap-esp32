#pragma once
// LAAP vendor 兼容层（C 安全）：以最小宏替换 IDF 日志/检查宏
// 寄存器 IO 通过 C 桥函数实现（实现在 laap_audio.cpp，内部走 Arduino Wire）
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define BIT(x) (1UL << (x))
extern void delay(uint32_t ms);
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t _e_ = (x); if (_e_ != ESP_OK) return _e_; } while (0)
#define ESP_RETURN_ON_FALSE(x, ret, tag, ...) do { if (!(x)) return ret; } while (0)
#define ESP_GOTO_ON_ERROR(x, got, tag, ...) do { esp_err_t _e_ = (x); if (_e_ != ESP_OK) { ret = _e_; goto got; } } while (0)
#define ESP_GOTO_ON_FALSE(x, retv, got, tag, ...) do { if (!(x)) { ret = retv; goto got; } } while (0)

#ifdef __cplusplus
extern "C" {
#endif
// 返回 0 成功
int laap_i2c_write(uint8_t addr, const uint8_t* data, size_t n);
int laap_i2c_write_read(uint8_t addr, uint8_t reg, uint8_t* out, size_t n);
#ifdef __cplusplus
}
#endif

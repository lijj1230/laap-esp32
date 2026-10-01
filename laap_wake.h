#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 本地唤醒词（v3.75 阶段四）
//   esp-sr WakeNet9（wn9_hiesp "Hi ESP"）+ AFE（AEC+NS+VAD）
//   模型驻留独立 model 分区（esp_sr_16 分区表，3.9MB 槽 3.3MB 模型）
//   数据自给：fill 回调从我们自己的 pump 环形缓冲取 16k 单声道样本
//   （48k I2S → 3:1 降采样 → 唤醒词引擎），避免与 ESP_SR 默认 I2SClass 直读冲突
// ============================================================

#include <sdkconfig.h>
// 双重门控：① sdkconfig 有 MODEL_IN_FLASH（esp-sr 库编译进来了）
// ② 当前分区表是 esp_sr_16（真有 model 分区放 srmodels.bin）
// no_fs 分区下宏不定义 → laapWake 整个模块编译为空，sr_start 不会被调
#if (CONFIG_IDF_TARGET_ESP32S3) && (CONFIG_MODEL_IN_FLASH || CONFIG_MODEL_IN_SDCARD) \
    && defined(ARDUINO_PARTITION_esp_sr_16)
#define LAAP_WAKEWORD_AVAILABLE 1
#endif

#ifdef LAAP_WAKEWORD_AVAILABLE

class LaapWake {
public:
  // 启动唤醒词检测（分区无模型/内存不足返回 false，设备退回 VAD 模式不受影响）
  bool begin();
  void end();
  bool active() const { return _active; }
  // 供音频泵喂 16k 单声道样本（laap_audio pump 的降采样输出，16bit）
  // SR 未启动时为空操作，零成本
  void feed(const int16_t* samples, size_t n);

  // 唤醒事件消费（主循环调用）：返回 true=刚才发生了唤醒，调用方应立即起对话
  bool consumeWakeup();
  // 唤醒计数（诊断误唤醒频率）
  uint32_t wakeCount() const { return _wakeCount; }

  // 事件回调桥（laap_wake.cpp 实现，由 ESP_SR 的 C 回调转发）
  void onWakeword();

private:
  bool _active = false;
  volatile bool _wakeFlag = false;
  uint32_t _wakeCount = 0;
};

extern LaapWake laapWake;
#endif

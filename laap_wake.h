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
// v3.76 回退（用户拍板"损耗太大"）：窗口模式唤醒词的代价 = 固件 96%（OTA 余量仅 114KB）
// + 空闲堆 -32KB + core 库补丁维护，功能收益撑不起。回退方式 = 默认关闸：
// 整个模块编译为空，ESP_SR 库不链接，固件回落 ~1.7MB。
// 重新启用：build_opt.h 加 -DLAAP_WAKEWORD_ENABLE=1 重编译即可——model 分区与
// srmodels 已烧在设备上，重启用是纯 OTA，无需动分区表。
#if defined(LAAP_WAKEWORD_ENABLE) \
    && (CONFIG_IDF_TARGET_ESP32S3) && (CONFIG_MODEL_IN_FLASH || CONFIG_MODEL_IN_SDCARD) \
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

  // ---- 窗口模式（v3.75c 定案）：wakenet9 常驻 47KB 与本固件共存放不下
  // （建引擎后最大块 3.5KB，堆护栏每次必触发）。改为按键/唤醒后窗口期在线：
  // 人在设备旁刚交互完，最可能连续语音；窗口关闭时引擎全额释放 47KB
  void setWindow(bool on);         // true=开窗（立即建引擎），false=关窗（让路）
  bool running() const { return _running; }
  void idleTick();                 // 主循环调用：堆护栏守护（窗口内堆不够时让路）

  // 事件回调桥（laap_wake.cpp 实现，由 ESP_SR 的 C 回调转发）
  void onWakeword();

private:
  bool _active = false;          // begin() 成功过（引擎可用）
  bool _running = false;         // 当前 sr 在跑
  volatile bool _wakeFlag = false;
  uint32_t _wakeCount = 0;
};

extern LaapWake laapWake;
#endif

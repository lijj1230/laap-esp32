#include "laap_wake.h"

#ifdef LAAP_WAKEWORD_AVAILABLE

#include "ESP_I2S.h"
#include "esp32-hal-sr.h"
#include <string.h>

LaapWake laapWake;

// 喂给 SR 的 16k 单声道环形缓冲（laap_audio pump 降采样后写入）
// 容量：500ms@16k。SR feed 任务 10ms 一帧 320B，500ms 冗余应对主循环短暂卡顿
#define WAKE_RB_SAMPLES ((size_t)(16000 / 2))          // 8000 样本 = 500ms
static int16_t s_rb[WAKE_RB_SAMPLES];
static volatile size_t s_rbW = 0, s_rbR = 0;           // 读写下标（单生产单消费，size_t 原子）

static bool s_wantStop = false;                        // 检测到唤醒后请求停 SR（在事件回调里置位）

size_t wakeRingRead(int16_t* out, size_t maxSamples) {
  size_t avail = (s_rbW + WAKE_RB_SAMPLES - s_rbR) % WAKE_RB_SAMPLES;
  size_t n = avail < maxSamples ? avail : maxSamples;
  for (size_t i = 0; i < n; i++) {
    out[i] = s_rb[(s_rbR + i) % WAKE_RB_SAMPLES];
  }
  s_rbR = (s_rbR + n) % WAKE_RB_SAMPLES;
  return n;
}

// SR 的数据自给回调：ESP_SR.feed 任务 10ms 要一次 16k 帧（单通道 320B）
// portMAX_DELAY 等待由我们控制——最多等 200ms（数据长期不足宁可报错让 SR 降速也不死锁）
static esp_err_t wakeFillCb(void* arg, void* out, size_t len, size_t* bytes_read, uint32_t timeout_ms) {
  int16_t* dst = (int16_t*)out;
  size_t want = len / 2;
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < want) {
    got += wakeRingRead(dst + got, want - got);
    if (got >= want) break;
    if (millis() - t0 > 200) break;                    // 200ms 还不够：交付短帧（SR 能容忍）
    vTaskDelay(2);
  }
  if (got < want) memset(dst + got, 0, (want - got) * 2);   // 不足补零（静音帧）
  *bytes_read = len;
  return ESP_OK;
}

// SR 事件回调跑在 sr_handler 任务上下文——只置标志，不碰对话状态
static void onWakeEvent(void* arg, sr_event_t event, int command_id, int phrase_id) {
  if (event == SR_EVENT_WAKEWORD) {
    laapWake.onWakeword();
  }
}

bool LaapWake::begin() {
  if (_active) return true;
  // SR_MODE_WAKEWORD：只用唤醒词检测（不用 MultiNet 命令词，语义对话交给云端 LLM）
  // 数据格式 "M"=单通道麦克风；我们的 pump 是 48k 单声道流，降采样后正好 16k
  esp_err_t err = sr_start(wakeFillCb, nullptr,
                           SR_CHANNELS_MONO, SR_MODE_WAKEWORD, "M",
                           nullptr, 0,
                           onWakeEvent, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[WAKE] sr_start 失败 err=%d（退回 VAD 模式）\n", (int)err);
    return false;
  }
  _active = true;
  Serial.println("[WAKE] 唤醒词引擎就绪（wn9_hiesp「Hi ESP」）");
  return true;
}

void LaapWake::end() {
  if (!_active) return;
  sr_stop();
  _active = false;
  s_rbW = s_rbR = 0;
  Serial.println("[WAKE] 唤醒词引擎停止");
}

void LaapWake::feed(const int16_t* samples, size_t n) {
  if (!_active) return;
  for (size_t i = 0; i < n; i++) {
    size_t next = (s_rbW + 1) % WAKE_RB_SAMPLES;
    if (next == s_rbR) { s_rbR = (s_rbR + 1) % WAKE_RB_SAMPLES; }   // 满：丢最老（唤醒词要新音频）
    s_rb[s_rbW] = samples[i];
    s_rbW = next;
  }
}

void LaapWake::onWakeword() {
  _wakeFlag = true;
  _wakeCount++;
  Serial.println("[WAKE] 唤醒词命中（Hi ESP）");
}

bool LaapWake::consumeWakeup() {
  if (!_wakeFlag) return false;
  _wakeFlag = false;
  return true;
}

#endif // LAAP_WAKEWORD_AVAILABLE

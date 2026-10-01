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
  if (_running) return true;   // 已在跑（_active 只表示"引擎可用过"，拆掉后要完整重启）
  uint32_t h0 = ESP.getFreeHeap();
  uint32_t m0 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  // SR_MODE_WAKEWORD：只用唤醒词检测（不用 MultiNet 命令词，语义对话交给云端 LLM）
  // 数据格式 "M"=单通道麦克风；我们的 pump 是 48k 单声道流，降采样后正好 16k
  esp_err_t err = sr_start(wakeFillCb, nullptr,
                           SR_CHANNELS_MONO, SR_MODE_WAKEWORD, "M",
                           nullptr, 0,
                           onWakeEvent, nullptr);
  uint32_t h1 = ESP.getFreeHeap();
  uint32_t m1 = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[WAKE] sr_start 堆账：free %u→%u（差 %d）最大块 %u→%u（差 %d）\n",
                h0, h1, (int)(h0 - h1), m0, m1, (int)(m0 - m1));
  if (err != ESP_OK) {
    Serial.printf("[WAKE] sr_start 失败 err=%d（退回 VAD 模式）\n", (int)err);
    return false;
  }
  _active = true;
  _running = true;
  Serial.println("[WAKE] 唤醒词引擎就绪（wn9_hiesp「Hi ESP」）");
  return true;
}

void LaapWake::end() {
  if (!_active) return;
  if (_running) sr_stop();
  _running = false;
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

// ---- 窗口模式（v3.75c） ----
void LaapWake::setWindow(bool on) {
  if (on) {
    if (_running) return;
    begin();                 // 开窗即建（失败下次 setWindow(true) 再试）
  } else if (_running) {
    sr_stop();               // 关窗让路：47KB 全额释放
    _running = false;
    s_rbW = s_rbR = 0;
    Serial.println("[WAKE] 窗口关闭，引擎让路");
  }
}

void LaapWake::idleTick() {
  if (_running) {
    // 堆护栏：窗口内若最大块跌破 28KB（打盹线 24KB 之上）→ 立即让路（窗口照旧，
    // 只是不再跑引擎）。wakenet9 与本固件共存就是紧，绝不把系统拖进打盹
    uint32_t maxBlk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (maxBlk < 28000) {
      sr_stop();
      _running = false;
      s_rbW = s_rbR = 0;
      Serial.printf("[WAKE] 堆护栏触发（最大块 %uB<28KB），引擎让路\n", (unsigned)maxBlk);
    }
  }
}bool LaapWake::consumeWakeup() {
  if (!_wakeFlag) return false;
  _wakeFlag = false;
  return true;
}

#endif // LAAP_WAKEWORD_AVAILABLE

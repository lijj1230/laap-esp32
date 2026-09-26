#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 音频管线（立创实战派 ESP32-S3）
//   I2S0 48kHz 全双工: 喇叭 ES8311(DO=45) / 麦克风 ES7210(DI=12)
//   共享 BCLK=14 WS=13 MCLK=38；PA 使能在 PCA9557 bit1
//   播放: 任意采样率 16bit 单声道 PCM → 线性插值升到 48k 双声道
//   录音: 48k 立体声→单声道→3:1 降采样到 16kHz（ASR 用）
//   VAD: 快/慢 RMS 能量比 + 环境自适应；兼作播放打断监测
// ============================================================

// 板载引脚
#define AUD_I2S_BCLK 14
#define AUD_I2S_WS   13
#define AUD_I2S_DOUT 45
#define AUD_I2S_DIN  12
#define AUD_I2S_MCLK 38
#define AUD_I2S_RATE 48000

class LaapAudio {
public:
  bool begin();
  bool ok() const { return _ok; }
  bool spkOk = false, micOk = false;

  // ---- 录音（16kHz 16bit 单声道，PSRAM 缓冲） ----
  bool recordStart(size_t maxSeconds = 20);
  void recordTick();                 // 非阻塞，需在 loop 高频调用
  size_t recordBytes() const { return _recLen; }
  const int16_t* recordData() const { return _recBuf; }
  void recordStop() { _recording = false; }
  bool recording() const { return _recording; }

  // ---- 播放 ----
  // data: 16bit 单声道 PCM；interruptCb 每 ~50ms 轮询一次，返回 true 则停播
  bool playPcm(const int16_t* data, size_t samples, uint32_t rate,
               bool (*interruptCb)(void* ctx) = nullptr, void* ctx = nullptr);
  bool interrupted() const { return _interrupted; }

  // ---- VAD ----
  void vadCalibrate(uint32_t ms);    // 静默环境校准
  bool vadSpeaking() const { return _vadSpeech; }
  float micRms() const { return _fastRms; }
  // 触发阈值乘数（自调优旋钮，RSI⑥）：1.0=默认灵敏度，>1 更不敏感（防环境噪声/回声误触发）
  void setVadThresholdMul(float m) { _vadThMul = (m < 1.0f ? 1.0f : (m > 2.0f ? 2.0f : m)); }
  float vadThresholdMul() const { return _vadThMul; }

  // ---- PA（读-改-写 PCA9557，不动 LCD_CS 位） ----
  void paSet(bool on);
  void paTick();                     // 空闲自动关 PA（主循环调用；流式播放间隔中保持开启）

  // ---- 音量（ES8311 0-100；持久化由 laap_config.volume 承担） ----
  void setVolume(uint8_t v);         // 运行时改音量（Web/串口调用）
  uint8_t volume() const { return _volume; }

  // 播放期间打断监测开关（默认开）
  void bargeInEnable(bool en) { _bargeEn = en; }

  // ---- 麦克风 PGA 增益（ES7210，0..37.5dB）----
  // 实测 30dB 时近场自响只有 RMS 200（底噪 102，仅 6dB 余量）→ 远场说话会被埋掉，
  // 默认提到 37.5dB（最大档）。用 /micgain 可在运行时逐档试。
  bool setMicGainDb(int db);         // 传 0/3/6/.../36/37.5
  int micGainDb() const { return _micGainDb; }

private:
  void pump();                       // 从 I2S 读数据→降采样→VAD/录音缓冲
  bool _ok = false;
  uint8_t _volume = 70;
  int _micGainDb = 375;              // ×10 存（375 = 37.5dB）
  bool _recording = false;
  int16_t* _recBuf = nullptr;
  size_t _recCap = 0, _recLen = 0;
  int32_t _dsAcc = 0; int8_t _dsCnt = 0;      // 3:1 降采样累加器
  float _slowRms = 30, _fastRms = 30;          // 环境基线 / 瞬时
  float _vadThMul = 1.0f;                      // 触发阈值乘数（自调优，钳位 1.0~2.0）
  bool _vadSpeech = false;
  uint32_t _speechStartMs = 0, _silenceMs = 0;
  bool _interrupted = false, _bargeEn = false;   // 默认关：播放期间不收麦（防回环/自触发）
  uint32_t _paOffMs = 0;                       // PA 空闲关断时刻（0=无需关）
};

extern LaapAudio audio;

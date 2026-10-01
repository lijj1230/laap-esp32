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
  bool spkOk = false, micOk = false;

  // ---- 录音（16kHz 16bit 单声道，PSRAM 缓冲） ----
  // 录音头自动回填预滚缓冲（触发前 ~1.5s）：VAD 要先听到 600ms 持续人声才开录，
  // 不回填的话句首必丢——唤醒词/前几个字永远不在录音里，实测只剩四五个字
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
  // VAD 状态复位（v3.51）：一轮录音结束后清残留——容量截断退出时 _vadSpeech 为真，
  // 会被冷却后的 loopTick 判成"还在说话"而误起下一轮
  void vadReset() { _vadSpeech = false; _silenceMs = 0; }
  float micRms() const { return _fastRms; }
  // 触发阈值乘数（自调优旋钮，RSI⑥）：1.0=默认灵敏度，>1 更不敏感（防环境噪声/回声误触发）。
  // 上限 1.6：乘数只升不降的历史曾把它顶到 2.0 → 远场人声全部埋掉（"要凑很近才理人"）
  void setVadThresholdMul(float m) { _vadThMul = (m < 1.0f ? 1.0f : (m > 1.6f ? 1.6f : m)); }
  float vadThresholdMul() const { return _vadThMul; }
  // 说完静音判停(ms)：停顿超过此时长视为说完收音。后台可配（设置页/ vadstop，300-15000）
  void setVadStopMs(uint16_t ms) { _vadStopMs = (ms < 300 ? 300 : (ms > 15000 ? 15000 : ms)); }
  uint16_t vadStopMs() const { return _vadStopMs; }
  // 预滚缓冲：播放完立即清空（自家 TTS 尾音不能进预滚，否则下一轮 ASR 听见自己说话）
  static constexpr size_t kPreRollBytes = 48000;   // 1.5s @ 16kHz 16bit 单声道
  void prerollFlush() { _preLen = 0; }

  // ---- PA（读-改-写 PCA9557，不动 LCD_CS 位） ----
  void paSet(bool on);
  void paTick();                     // 空闲自动关 PA（主循环调用；流式播放间隔中保持开启）

  // ---- 音量（ES8311 0-100；持久化由 laap_config.volume 承担） ----
  void setVolume(uint8_t v);         // 运行时改音量（Web/串口调用）
  uint8_t volume() const { return _volume; }

  // 播放期间打断监测开关（默认开）
  void bargeInEnable(bool en) { _bargeEn = en; }

  // ---- AEC 全双工（v3.75：播放中听真人说话打断，而不是听见自己） ----
  // aecEn: 回声消除启用。参考信号=待写 I2S 的 48k 样本（播放内容），与 mic 采样按 10ms 帧对齐后
  // 喂 esp-sr 纯 DSP AEC（不需要模型分区），输出跑 VAD——远端的人声才会推高 VAD
  bool aecAvailable() const { return _aec; }
  void aecEnable(bool en) { _aecEn = en; }
  bool interruptedVoice() const { return _aecInterrupt; }
  void aecFeedRef(const int16_t* pcm48, size_t samples);  // playPcm 写参考信号（播放内容）

  // ---- 唤醒词喂食（v3.75）：pump 的 16k 降采样流转发给 laapWake（函数指针解耦） ----
  void setWakeFeed(void (*cb)(const int16_t*, size_t)) { _wakeFeed = cb; }

  // 播放中强制打断（唤醒词事件路径）：下一个 I2S 写块检查即停
  void forceInterrupt() { if (_bargeEn) _interrupted = true; }

  // ---- 麦克风 PGA 增益（ES7210，0..37.5dB）----
  // 实测 30dB 时近场自响只有 RMS 200（底噪 102，仅 6dB 余量）→ 远场说话会被埋掉，
  // 默认提到 37.5dB（最大档）。用 /micgain 可在运行时逐档试。
  bool setMicGainDb(int db);         // 传 0/3/6/.../36/37.5
  int micGainDb() const { return _micGainDb; }

private:
  void aecProcessTick();   // 48k 双流对齐→16k 帧→AEC→VAD（pump 内调用）
  void pump();                       // 从 I2S 读数据→降采样→VAD/录音缓冲
  bool _ok = false;
  uint8_t _volume = 70;
  int _micGainDb = 375;              // ×10 存（375 = 37.5dB）
  bool _recording = false;
  int16_t* _recBuf = nullptr;
  size_t _recCap = 0, _recLen = 0;
  int32_t _dsAcc = 0; int8_t _dsCnt = 0;      // 3:1 降采样累加器
  int16_t* _preBuf = nullptr;                  // 预滚线性缓冲（新音频始终追加在尾部）
  size_t _preLen = 0;                          // 预滚有效字节数（0..kPreRollBytes）
  float _slowRms = 30, _fastRms = 30;          // 环境基线 / 瞬时
  float _vadThMul = 1.0f;                      // 触发阈值乘数（自调优，钳位 1.0~1.6）
  uint16_t _vadStopMs = 5000;                  // 说完静音判停（v3.33 起后台可配，默认 5s 宁等勿截）
  bool _vadSpeech = false;
  uint32_t _silenceMs = 0;
  bool _interrupted = false, _bargeEn = false;   // 默认关：播放期间不收麦（防回环/自触发）
  void (*_wakeFeed)(const int16_t*, size_t) = nullptr;  // 16k 样本转发回调（laap_wake）
  // ---- AEC 全双工状态 ----
  bool _aec = false;            // aec_create 成功
  bool _aecEn = false;          // 本轮播放启用 AEC 打断
  bool _aecInterrupt = false;   // AEC-VAD 判定真人插话
  void* _aecHandle = nullptr;   // aec_handle_t*（void* 免头文件进 Arduino 公共面）
  int _aecChunk = 160;          // AEC 帧大小（样本数，10ms@16k，aec_get_chunksize 实测回填）
  int16_t* _aecRef48 = nullptr; // 参考信号累积（48k，与 mic 48k 采样同步钟）
  int16_t* _aecMic48 = nullptr; // 麦克风 48k 采样累积
  size_t _aecRefLen = 0, _aecMicLen = 0;  // 有效字节数
  size_t _aecConsumed = 0;      // 已对齐消费的参考字节游标
  uint32_t _paOffMs = 0;                       // PA 空闲关断时刻（0=无需关）
};

extern LaapAudio audio;

#include "laap_audio.h"
#include <Wire.h>
#include <ESP_I2S.h>
#include "src/vendor/es8311/es8311.h"
#include "src/vendor/es7210/es7210.h"
#include <esp_heap_caps.h>

// ---- AEC 全双工（v3.75） ----
// esp-sr 的独立 AEC 是纯 DSP 自适应滤波（无神经网络、无需 srmodels 分区），
// 符号在 libesp_audio_processor.a，Arduino 链接器全量 -Wl,--whole-archive 这些库时可用。
extern "C" {
#include "esp_aec.h"
}

LaapAudio audio;

// C 桥：vendor 编解码器驱动的 I2C 访问（Wire 在此为 C++）
extern "C" int laap_i2c_write(uint8_t addr, const uint8_t* data, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(data, n);
  return Wire.endTransmission() == 0 ? 0 : -1;
}
extern "C" int laap_i2c_write_read(uint8_t addr, uint8_t reg, uint8_t* out, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if ((size_t)Wire.requestFrom((int)addr, (int)n) != n) return -1;
  for (size_t i = 0; i < n; i++) out[i] = Wire.read();
  return 0;
}

// I2C 编解码器地址（官方例程）
#define ES8311_ADDR 0x18
#define ES7210_ADDR 0x41

static I2SClass i2s;
static es8311_handle_t spk = nullptr;
static es7210_dev_handle_t mic = nullptr;
static es7210_codec_config_t s_cc = {};      // ES7210 当前配置（/micgain 改增益时复用）

// ES7210 PGA 档位（dB×10 → 枚举序号），顺序同 es7210_mic_gain_t
static const int kMicGainDb10[] = {0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330, 345, 360, 375};
static const int kMicGainN = sizeof(kMicGainDb10) / sizeof(kMicGainDb10[0]);

bool LaapAudio::setMicGainDb(int db10) {
  if (!mic) return false;
  int best = 0, bestDiff = 100000;
  for (int i = 0; i < kMicGainN; i++) {
    int d = abs(kMicGainDb10[i] - db10);
    if (d < bestDiff) { bestDiff = d; best = i; }
  }
  s_cc.mic_gain = (es7210_mic_gain_t)best;
  _micGainDb = kMicGainDb10[best];
  return es7210_config_codec(mic, &s_cc) == ESP_OK;
}

bool LaapAudio::begin() {
  // I2S 全双工（Wire 已由 display 初始化）
  // 预滚缓冲先于一切分配：失败也能活（recordStart 回填时判空跳过），只是没有句首保护
  _preBuf = (int16_t*)heap_caps_malloc(kPreRollBytes, MALLOC_CAP_SPIRAM);
  if (!_preBuf) _preBuf = (int16_t*)malloc(kPreRollBytes);
  _preLen = 0;
  i2s.setPins(AUD_I2S_BCLK, AUD_I2S_WS, AUD_I2S_DOUT, AUD_I2S_DIN, AUD_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, AUD_I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[AUD] I2S begin 失败");
    return false;
  }
  // RX: 48k 立体声 → 单声道（ES7210 TDM 两麦）
  i2s.configureRX(AUD_I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                  I2S_RX_TRANSFORM_16_STEREO_TO_MONO);

  // ---- ES8311 喇叭 ----
  spk = es8311_create(0, ES8311_ADDR);
  if (spk) {
    const es8311_clock_config_t clk = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = AUD_I2S_RATE * 256,
      .sample_frequency = AUD_I2S_RATE,
    };
    if (es8311_init(spk, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK) {
      es8311_voice_volume_set(spk, _volume, nullptr);
      spkOk = true;
    } else {
      Serial.println("[AUD] ES8311 init 失败");
    }
  }

  // ---- ES7210 麦克风 ----
  const es7210_i2c_config_t i2cc = { .i2c_port = 0, .i2c_addr = ES7210_ADDR };
  if (es7210_new_codec(&i2cc, &mic) == ESP_OK && mic) {
    s_cc = {
      .sample_rate_hz = AUD_I2S_RATE,
      .mclk_ratio = 256,
      .i2s_format = ES7210_I2S_FMT_I2S,          // 官方例程同为 I2S 格式
      .bit_width = ES7210_I2S_BITS_16B,
      .mic_bias = ES7210_MIC_BIAS_2V87,
      .mic_gain = ES7210_MIC_GAIN_37_5DB,        // 默认拉满：30dB 实测只有 6dB 余量，远场说话会被埋
      .flags = { .tdm_enable = true },
    };
    if (es7210_config_codec(mic, &s_cc) == ESP_OK) micOk = true;
    else Serial.println("[AUD] ES7210 config 失败");
  } else {
    Serial.println("[AUD] ES7210 create 失败");
  }

  paSet(false);
  _ok = spkOk;                                 // 麦克风缺失不阻塞 TTS
  Serial.printf("[AUD] I2S ok spk=%d mic=%d\n", spkOk, micOk);

  // ---- AEC（v3.76b 已回退）：create 跳过（省 ~8KB 内部堆常驻）。代码与缓冲
  // 分配保留在此分支里，重启用=把 if(0) 改回 if(micOk)。阶段四的实测记录见 git 历史
  _aec = false;
  if (false && micOk) {
    aec_config_t ac = {};
    ac.mic_num = 1; ac.ref_num = 1; ac.out_num = 1;
    ac.filter_length = 4;
    ac.sample_rate = 16000;
    ac.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;    // 关键：AEC 工作缓冲路由 PSRAM
    ac.mode = AEC_MODE_SR_LOW_COST;
    ac.nlp_level = AEC_NLP_LEVEL_AGGR;
    _aecHandle = aec_create_from_config(&ac);
    if (_aecHandle) {
      _aecChunk = aec_get_chunksize((aec_handle_t*)_aecHandle);   // 实际帧大小（应=160）
      _aecRef48 = (int16_t*)heap_caps_malloc(480 * 8 * 2, MALLOC_CAP_SPIRAM);   // 80ms@48k
      _aecMic48 = (int16_t*)heap_caps_malloc(480 * 8 * 2, MALLOC_CAP_SPIRAM);
      _aec = _aecRef48 && _aecMic48;
      if (!_aec) Serial.println("[AUD] AEC 缓冲分配失败，打断降级");
    } else {
      Serial.println("[AUD] aec_create 失败，打断降级");
    }
  }
  Serial.printf("[AUD] AEC %s chunk=%d\n", _aec ? "ok" : "off", _aec ? _aecChunk : 0);
  return _ok;
}

void LaapAudio::paSet(bool on) {
  // PCA9557 读-改-写 bit1(PA_EN)，保持 bit0(LCD_CS)=0
  // 幂等提速：已在目标态直接返回——逐帧流式播放时每帧 30ms 延时会拖到 0.6x 实时
  static bool s_paOn = false;
  if (s_paOn == on) return;  Wire.beginTransmission(0x19); Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return;
  uint8_t cur = 0;
  if (Wire.requestFrom((int)0x19, 1) == 1) cur = Wire.read();
  uint8_t nv = on ? (cur | 0x02) : (cur & ~0x02);
  Wire.beginTransmission(0x19); Wire.write(0x01); Wire.write(nv);
  if (Wire.endTransmission() == 0) {
    s_paOn = on;
    if (on) delay(30);   // 仅在关→开沿起振等待一次
  }
}

void LaapAudio::setVolume(uint8_t v) {
  if (v > 100) v = 100;
  _volume = v;
  if (spk) es8311_voice_volume_set(spk, v, nullptr);
}

bool LaapAudio::recordStart(size_t maxSeconds) {
  size_t cap = maxSeconds * 16000 * 2;   // 16kHz 16bit
  // 先清账再分配：失败也不能让上一次的 _recCap/_recLen 残留——调用方按旧长度误判
  // 会拿空缓冲去裁剪/转写（PSRAM 大块被相机/LLM 挤占时可发生）
  _recCap = 0; _recLen = 0; _dsCnt = 0; _dsAcc = 0;
  if (_recBuf) free(_recBuf);
  _recBuf = nullptr;
  _recBuf = (int16_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!_recBuf) { _recBuf = (int16_t*)malloc(cap); }
  if (!_recBuf) return false;
  _recCap = cap;
  // 预滚回填：触发前的最近 ~1.5s 已经在滚，拷进录音头（时间序，尾部=最新）
  size_t pre = _preLen; if (pre > cap) pre = cap;
  if (pre) memcpy(_recBuf, _preBuf, pre);
  _recLen = pre;
  _recording = true;
  return true;
}

void LaapAudio::pump() {
  static int16_t tmp[256];
  int avail = i2s.available();
  if (avail < (int)sizeof(tmp)) {
    if (avail <= 0) return;
  }
  int n = i2s.readBytes((char*)tmp, min((size_t)avail, sizeof(tmp))) / 2; // 单声道 48k 样本数
  if (n <= 0) return;

  // RMS（快/慢自适应）
  uint64_t sum = 0;
  for (int i = 0; i < n; i++) { int32_t v = tmp[i]; sum += (uint64_t)(v * v); }
  float rms = sqrtf((float)(sum / n));
  _fastRms = _fastRms * 0.6f + rms * 0.4f;
  _slowRms = _slowRms * 0.995f + rms * 0.005f;

  // VAD 判定：快均值显著高于慢基线（乘数由自调优旋钮控制，1.0=原始灵敏度）。
  // 基础阈值 2.2/120 → 1.8/80：实测 37.5dB 满增益下远场人声(1m) 快RMS 只有 300±，
  // 旧阈值 344+ 时好时坏，600ms 持续判经常攒不满 → "要凑很近才理人"
  float th = (_slowRms * 1.8f + 80) * _vadThMul;
  if (_fastRms > th) {
    if (!_vadSpeech) { _vadSpeech = true; }
    _silenceMs = 0;
  } else if (_vadSpeech) {
    _silenceMs += n * 1000UL / AUD_I2S_RATE;
    // 判停窗口后台可配（默认 5s：句间斟酌/换气不截断；代价是答话前多等这么久）
    if (_silenceMs > _vadStopMs) _vadSpeech = false;
  }

  // 3:1 降采样到 16k：录音期进录音缓冲；空闲期滚进预滚缓冲（供下次触发回填句首）。
  // 预滚满时丢老一半——按 pump 调用粒度整批搬（每次最多 24KB，约 4.5MB/s，PSRAM 无压力），
  // 千万别按样本搬，否则 16k 样本/s 全都要触发 memmove
  if (!_recording && _preBuf && !_aecEn) {   // AEC 模式下不走预滚（回声残留会污染下轮 ASR）
    size_t need = (n / 3 + 1) * 2;
    if (_preLen + need > kPreRollBytes) {
      size_t keep = kPreRollBytes / 2;
      memmove(_preBuf, (uint8_t*)_preBuf + (kPreRollBytes - keep), keep);
      _preLen = keep;
    }
  }
  for (int i = 0; i < n; i++) {
    _dsAcc += tmp[i]; _dsCnt++;
    if (_dsCnt >= 3) {
      int16_t s = (int16_t)(_dsAcc / 3);
      _dsAcc = 0; _dsCnt = 0;
      if (_recording) {
        if (_recLen + 2 <= _recCap) { _recBuf[_recLen / 2] = s; _recLen += 2; }
      } else if (_preBuf && !_aecEn) {
        _preBuf[_preLen / 2] = s; _preLen += 2;
      }
    }
  }

  // AEC 双流同步累积（v3.75）：参考信号由 playPcm 侧 aecFeedRef 写入（播放内容），
  // 麦克风 48k 在此追加；两流字节数接近时逐帧消费（aecProcessTick 内对齐）
  if (_aecEn && _aecMic48) {
    size_t bytes = n * 2;
    if (_aecMicLen + bytes <= 480 * 8 * 2) {
      memcpy((uint8_t*)_aecMic48 + _aecMicLen, tmp, bytes);
      _aecMicLen += bytes;
    }
    aecProcessTick();
  }

  // 唤醒词引擎喂食（v3.75）：按本批 48k 的每第 3 个样本直接取（与降采样取点规则一致）
  if (_wakeFeed) {
    static int16_t wk[96];
    size_t wn = 0;
    for (int i = 0; i < n && wn < 96; i += 3) wk[wn++] = tmp[i];
    if (wn) _wakeFeed(wk, wn);
  }
}

void LaapAudio::recordTick() { pump(); }

// ---- AEC 帧处理（v3.75）：48k 同步流 → 降采样 → 16k 帧喂 AEC → 输出跑 VAD ----
// 参考信号与麦克风采样同钟（都是 I2S 48k），字节数差=采录不同步的抖动，按字节数对齐消费
void LaapAudio::aecFeedRef(const int16_t* pcm48, size_t samples) {
  if (!_aecEn || !_aecRef48) return;
  size_t bytes = samples * 2;
  if (_aecRefLen + bytes > 480 * 8 * 2) return;   // 溢出丢弃（正常不会到：消费端每帧跟进）
  memcpy((uint8_t*)_aecRef48 + _aecRefLen, pcm48, bytes);
  _aecRefLen += bytes;
}

// 消费已对齐的 48k 双流：3:1 降到 16k 喂 AEC，输出过 VAD
static int16_t s_aecIn[160], s_aecRef[160], s_aecOut[160];
void LaapAudio::aecProcessTick() {
  if (!_aec || !_aecEn) return;
  const size_t chunkIn = _aecChunk;               // 16k 帧样本数（160=10ms）
  const size_t need48 = chunkIn * 3 * 2;          // 对应 48k 每流字节数
  while (_aecRefLen >= need48 && _aecMicLen >= need48) {
    // 3:1 降采样（简单取每第 3 个，与主录音路径的平均法一致量级足够 VAD）
    for (size_t i = 0; i < chunkIn; i++) {
      s_aecIn[i] = _aecMic48[i * 3];
      s_aecRef[i] = _aecRef48[i * 3];
    }
    memmove(_aecMic48, _aecMic48 + need48, _aecMicLen - need48);
    memmove(_aecRef48, _aecRef48 + need48, _aecRefLen - need48);
    _aecMicLen -= need48; _aecRefLen -= need48;

    aec_process((aec_handle_t*)_aecHandle, s_aecIn, s_aecRef, s_aecOut);
    // AEC 输出 RMS——这是"消掉自己回声后剩余的声音"，超阈值=真人插话
    uint64_t sum = 0;
    for (size_t i = 0; i < chunkIn; i++) { int32_t v = s_aecOut[i]; sum += (uint64_t)(v * v); }
    float rms = sqrtf((float)(sum / chunkIn));
    // 打断门限：回声已消，剩余能量主要是人声。600 遮底噪+残余（实测回声消后残余 ~100-200）。
    // 连续 2 帧(20ms)超门限才置位（单帧毛刺防护）
    static uint8_t s_hiCnt = 0;
    if (rms > 600) {
      if (++s_hiCnt >= 2) { _aecInterrupt = true; s_hiCnt = 2; }
    } else if (rms > 350) {
      s_hiCnt = s_hiCnt;   // 中间带：维持不涨不清（滞回）
    } else {
      s_hiCnt = 0;
    }
  }
}

void LaapAudio::vadCalibrate(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { pump(); delay(5); }
}

// 线性插值重采样 rate→48k，写双声道
// I2S 实写字节数（诊断"没声音"：解码正常但这里为 0 就是 I2S/编解码器/功放段的问题）
uint32_t s_i2sBytes = 0;
uint32_t laapI2sBytes() { return s_i2sBytes; }
bool LaapAudio::playPcm(const int16_t* data, size_t samples, uint32_t rate,
                        bool (*interruptCb)(void*), void* ctx) {
  if (!spkOk) return false;
  _interrupted = false;
  paSet(true);               // 起振等待已内置在 paSet 的开沿（逐帧流播不再每帧空转 30ms）

  // v3.76b：AEC 全双工整链回退（用户拍板）——播报期间+TLS 并发时 AEC 的内部堆
  // 开销（chunk 512 工作区+滤波器状态）会把 LLM 的 31KB 连续块挤没（llm_fail 8 次
  // 实证），堆护栏也只护住播报起点、护不住播报中途的 TLS 起飞。打断退回
  // v3.74 形态：bargeInEnable(false)=播放不收麦，冷却期+preerollFlush 防自听见。
  // AEC 代码路径保留（git 历史），重启用=playPcm 里恢复 aec 判定+aec_create。
  bool aec = false;
  if (aec) { _aecRefLen = 0; _aecMicLen = 0; }

  static int16_t out[2 * 480];   // 480 输出样本对(10ms@48k)
  double step = (double)rate / AUD_I2S_RATE;
  double pos = 0;
  size_t idx = 0;

  // 播放漏音基线（前 300ms 采样，供打断判定参考）——AEC 模式下不用（AEC 输出已消回声）
  uint32_t leakT0 = millis();
  float leakRms = 0; int leakN = 0;
  int highCnt = 0;

  while (idx < samples) {
    int m = 0;
    while (m < 480 && idx < samples) {
      size_t i0 = (size_t)pos;
      double frac = pos - i0;
      int32_t s = data[i0];
      if (i0 + 1 < samples) s = (int32_t)(data[i0] * (1.0 - frac) + data[i0 + 1] * frac);
      out[m * 2] = (int16_t)s; out[m * 2 + 1] = (int16_t)s;
      m++; pos += step;
      if ((size_t)pos >= samples) { pos = samples - 1; }
      if (pos <= idx) pos = idx + 1; // 防死循环
      idx = (size_t)pos;
    }
    if (m > 0) s_i2sBytes += i2s.write((uint8_t*)out, m * 4);   // 记账：I2S 实写字节（0=写不进去，硬件层问题）
    if (aec) aecFeedRef(out, m);   // v3.75：刚写进喇叭的内容=参考信号（回声模板）

    // 打断监测：AEC 模式在 pump() 里判定（_aecInterrupt）；旧 RMS 基线法仅在非 AEC 时兜底
    if (_bargeEn) {
      pump();
      if (aec) {
        if (_aecInterrupt) _interrupted = true;
      } else if (millis() - leakT0 < 300) { leakRms += _fastRms; leakN++; }
      else if (!interruptCb) {
        float leak = (leakN ? leakRms / leakN : 400) * 1.9f + 250;
        highCnt = (_fastRms > leak) ? highCnt + 1 : 0;
        if (highCnt >= 3) { _interrupted = true; }
      }
    }
    if (interruptCb && interruptCb(ctx)) _interrupted = true;
    if (_tapProbe && _tapProbe()) _interrupted = true;   // v3.76d：按键点按=停播
    if (_interrupted) break;
  }
  // PA 不再逐块关断：流式播放每 24ms 一块，逐块开关会让功放永远停在启动瞬态（无声根因）。
  // 改为保持开启，静默 400ms 后由 paTick() 关闭。
  // 注意：这里绝不能"排空麦克风"——I2SClass::available() 返回常量而非真实字节数，
  // 逐帧排水会让每个 24ms 音频块阻塞数百毫秒（卡顿根因，2026-09-26 实测回归）。
  // 播放期间不读 RX：FIFO 溢出丢数据无害，陈旧回声由 speak 后的 1200ms 冷却期兜住。
  _aecEn = false;              // 退出全双工：预滚恢复记账（AEC 缓冲里的回声残段不进预滚）
  _paOffMs = millis() + 400;
  return !_interrupted;
}

// 空闲关功放（主循环周期调用）：最后一次播放后 400ms 关断
void LaapAudio::paTick() {
  if (_paOffMs && (int32_t)(millis() - _paOffMs) >= 0) {
    _paOffMs = 0;
    paSet(false);
  }
}

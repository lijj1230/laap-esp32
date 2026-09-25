#include "laap_audio.h"
#include "laap_display.h"
#include <Wire.h>
#include <ESP_I2S.h>
#include "src/vendor/es8311/es8311.h"
#include "src/vendor/es7210/es7210.h"
#include <esp_heap_caps.h>

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

bool LaapAudio::begin() {
  // I2S 全双工（Wire 已由 display 初始化）
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
    const es7210_codec_config_t cc = {
      .sample_rate_hz = AUD_I2S_RATE,
      .mclk_ratio = 256,
      .i2s_format = ES7210_I2S_FMT_I2S,          // 官方例程同为 I2S 格式
      .bit_width = ES7210_I2S_BITS_16B,
      .mic_bias = ES7210_MIC_BIAS_2V87,
      .mic_gain = ES7210_MIC_GAIN_30DB,
      .flags = { .tdm_enable = true },
    };
    if (es7210_config_codec(mic, &cc) == ESP_OK) micOk = true;
    else Serial.println("[AUD] ES7210 config 失败");
  } else {
    Serial.println("[AUD] ES7210 create 失败");
  }

  paSet(false);
  _ok = spkOk;                                 // 麦克风缺失不阻塞 TTS
  Serial.printf("[AUD] I2S ok spk=%d mic=%d\n", spkOk, micOk);
  return _ok;
}

void LaapAudio::paSet(bool on) {
  // PCA9557 读-改-写 bit1(PA_EN)，保持 bit0(LCD_CS)=0
  // 幂等提速：已在目标态直接返回——逐帧流式播放时每帧 30ms 延时会拖到 0.6x 实时
  static bool s_paOn = false;
  if (s_paOn == on) return;
  Wire.beginTransmission(0x19); Wire.write(0x01);
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
  if (_recBuf) free(_recBuf);
  _recBuf = (int16_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!_recBuf) { _recBuf = (int16_t*)malloc(cap); }
  if (!_recBuf) return false;
  _recCap = cap; _recLen = 0; _dsCnt = 0; _dsAcc = 0;
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

  // VAD 判定：快均值显著高于慢基线
  float th = _slowRms * 2.2f + 120;
  if (_fastRms > th) {
    if (!_vadSpeech) { _vadSpeech = true; _speechStartMs = millis(); }
    _silenceMs = 0;
  } else if (_vadSpeech) {
    _silenceMs += n * 1000UL / AUD_I2S_RATE;
    if (_silenceMs > 450) _vadSpeech = false;   // 450ms 静音判停（更快响应）
  }

  // 3:1 降采样到 16k 入录音缓冲
  if (_recording) {
    for (int i = 0; i < n; i++) {
      _dsAcc += tmp[i]; _dsCnt++;
      if (_dsCnt >= 3) {
        if (_recLen + 2 <= _recCap) {
          int16_t s = (int16_t)(_dsAcc / 3);
          _recBuf[_recLen / 2] = s; _recLen += 2;
        }
        _dsAcc = 0; _dsCnt = 0;
      }
    }
  } else { _dsAcc = 0; _dsCnt = 0; }
}

void LaapAudio::recordTick() { pump(); }

void LaapAudio::vadCalibrate(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { pump(); delay(5); }
}

// 线性插值重采样 rate→48k，写双声道
bool LaapAudio::playPcm(const int16_t* data, size_t samples, uint32_t rate,
                        bool (*interruptCb)(void*), void* ctx) {
  if (!spkOk) return false;
  _interrupted = false;
  paSet(true);               // 起振等待已内置在 paSet 的开沿（逐帧流播不再每帧空转 30ms）

  static int16_t out[2 * 480];   // 480 输出样本对(10ms@48k)
  double step = (double)rate / AUD_I2S_RATE;
  double pos = 0;
  size_t idx = 0;

  // 播放漏音基线（前 300ms 采样，供打断判定参考）
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
    if (m > 0) i2s.write((uint8_t*)out, m * 4);

    // 打断监测：仅在 barge-in 开启时才吸麦克风（默认关——播放时不动麦克风，
    // 避免 I2S 全双工 RX/TX 竞争与"自己听见自己"的回环干扰）
    if (_bargeEn) {
      pump();
      if (millis() - leakT0 < 300) { leakRms += _fastRms; leakN++; }
      else if (!interruptCb) {
        float leak = (leakN ? leakRms / leakN : 400) * 1.9f + 250;
        highCnt = (_fastRms > leak) ? highCnt + 1 : 0;
        if (highCnt >= 3) { _interrupted = true; }
      }
    }
    if (interruptCb && interruptCb(ctx)) _interrupted = true;
    if (_interrupted) break;
  }
  // PA 不再逐块关断：流式播放每 24ms 一块，逐块开关会让功放永远停在启动瞬态（无声根因）。
  // 改为保持开启，静默 400ms 后由 paTick() 关闭。
  _paOffMs = millis() + 400;
  // 播放期间未吸麦克风（barge-in 关闭时）→ 排空 RX 积压，避免陈旧回声在恢复聆听时误触发 VAD
  if (!_bargeEn) {
    static int16_t junk[256];
    int guard = 0;
    while (i2s.available() > 0 && guard++ < 64) {
      if (i2s.readBytes((char*)junk, min((size_t)i2s.available(), sizeof(junk))) <= 0) break;
    }
  }
  return !_interrupted;
}

// 空闲关功放（主循环周期调用）：最后一次播放后 400ms 关断
void LaapAudio::paTick() {
  if (_paOffMs && (int32_t)(millis() - _paOffMs) >= 0) {
    _paOffMs = 0;
    paSet(false);
  }
}

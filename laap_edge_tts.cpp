#include "laap_edge_tts.h"
#include "laap_ws.h"
#include "laap_audio.h"
uint32_t laapI2sBytes();   // 诊断：I2S 实写字节（laap_audio.cpp）
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>   // PSRAM 缓冲（别把内部堆切碎）
#include <mbedtls/sha256.h>
#include <time.h>
#include "mp3dec.h"

EdgeTts edgeTts;

static const char* EDGE_HOST = "speech.platform.bing.com";
static const char* TCT = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
static const char* GEC_VER  = "1-143.0.3650.75";

// Sec-MS-GEC：SHA256( (unix+11644473600 向下取整300s)*1e7 的十进制 + TrustedToken ) 大写hex
static String genSecMsGec() {
  time_t now = time(nullptr);          // 需要 NTP 已同步
  uint64_t t = (uint64_t)(now + 11644473600ULL);
  t -= t % 300ULL;
  unsigned long long ft = t * 10000000ULL;
  // 手动十进制（避免 64 位转 String 的平台差异）
  char num[32]; int i = 31; num[31] = 0;
  unsigned long long v = ft;
  String in;
  if (v == 0) in = "0";
  else {
    while (v > 0 && i >= 0) { num[--i] = '0' + (v % 10); v /= 10; }
    in = String(&num[i]);
  }
  in += TCT;
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t*)in.c_str(), in.length(), hash, 0);
  char hex[65];
  for (int k = 0; k < 32; k++) sprintf(hex + k * 2, "%02X", hash[k]);
  hex[64] = 0;
  return String(hex);
}

static String uuidNoDash() {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = esp_random() & 0xFF;
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char s[33];
  for (int i = 0; i < 16; i++) sprintf(s + i * 2, "%02x", b[i]);
  s[32] = 0;
  return String(s);
}

static String jsDate() {
  const char* wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  time_t now = time(nullptr);
  struct tm t; gmtime_r(&now, &t);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %s %02d %d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
           wd[t.tm_wday], mo[t.tm_mon], t.tm_mday, t.tm_year + 1900,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// 流式解码播放器：16KB 环形累积缓冲 + 逐帧解码（小智式流水线）
// 用法：mp3StreamReset() → 循环 feedMp3(块) → 每次喂后 playMp3Stream() → mp3StreamFlush()
// 缓冲放 PSRAM：16KB 常驻内部 RAM 会把内部堆切碎，而 TLS 握手要一整块 ~39KB——
// 实测播报一次后最大连续块 39KB→33KB，此后所有大模型请求都"连接失败"（要等重启）
static uint8_t* mp3Buf() {
  static uint8_t* p = nullptr;
  if (!p) {
    p = (uint8_t*)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    if (!p) p = (uint8_t*)malloc(16 * 1024);     // PSRAM 不可用则退回内部堆（至少还能出声）
  }
  return p;
}
#define mp3StreamBuf mp3Buf()                // 用法保持 `mp3StreamBuf[i]` 不变
#define MP3_BUF_SIZE (16 * 1024)
static size_t mp3StreamLen = 0;

static void mp3StreamReset() { mp3StreamLen = 0; }
static bool playMp3Stream();   // 前向声明：feedMp3 满时会先播腾空间

static void feedMp3(const uint8_t* d, size_t n) {
  if (mp3StreamLen + n > MP3_BUF_SIZE) {
    playMp3Stream();                                     // 满：先播掉腾空间（原整块丢弃=成段杂音）
    if (mp3StreamLen + n > MP3_BUF_SIZE) {
      Serial.printf("[TTS] 单块 %uB 仍超缓冲余量，截尾 %uB（异常大单帧，留意音质）\n",
                    (unsigned)n, (unsigned)(n - (MP3_BUF_SIZE - mp3StreamLen)));
      n = MP3_BUF_SIZE - mp3StreamLen;                   // 仍放不下：截尾保帧头
    }
  }
  if (n == 0) return;
  memcpy(mp3StreamBuf + mp3StreamLen, d, n);
  mp3StreamLen += n;
}

// 把缓冲里所有完整帧解码播掉；半帧残余留在缓冲头等下一块
// 解码器全局持久化：MP3 位池跨帧引用前帧数据，每调用重建丢状态→后续帧解码失败/爆音
static HMP3Decoder s_mp3Dec = nullptr;
// "没声音"诊断：收到多少音频字节 / 解出多少帧 / 样本峰值（0=解出来就是静音）
static uint32_t s_ttsBytes = 0, s_ttsFrames = 0, s_ttsSamples = 0;
static int s_ttsPeak = 0;
// ============================================================
//  自听回环诊断（/asrloop 用）：把解码出的 PCM 攒进外部缓冲（重采样 16k 单声道），
//  调用方随后边播边录，就能让设备"听自己说话"→ 发 ASR。用来把
//  "ASR 请求/服务端" 与 "麦克风拾音" 两件事彻底分开验证。
// ============================================================
static int16_t* s_capBuf = nullptr;
static size_t   s_capCap = 0, s_capSamples = 0;
static bool     s_capOn = false;
static int      s_capSrcRate = 24000;
static double   s_capPos = 0;

void laapTtsCaptureBegin(int16_t* buf, size_t cap, int srcRate) {
  s_capBuf = buf; s_capCap = cap; s_capSamples = 0; s_capPos = 0;
  s_capSrcRate = srcRate > 0 ? srcRate : 24000; s_capOn = true;
}
size_t laapTtsCaptureEnd(void) { s_capOn = false; return s_capSamples; }

// 把一帧解码结果线性插值重采样进捕获缓冲
static void captureFrame(const int16_t* pcm, size_t samples, int rate) {
  if (!s_capOn || !s_capBuf || !s_capCap) return;
  if (rate > 0) s_capSrcRate = rate;
  double step = (double)s_capSrcRate / 16000.0;
  while (s_capPos < (double)samples && s_capSamples < s_capCap) {
    size_t i0 = (size_t)s_capPos;
    double frac = s_capPos - i0;
    int32_t s = pcm[i0];
    if (i0 + 1 < samples) s = (int32_t)(pcm[i0] * (1.0 - frac) + pcm[i0 + 1] * frac);
    s_capBuf[s_capSamples++] = (int16_t)s;
    s_capPos += step;
  }
  s_capPos -= (double)samples;
  if (s_capPos < 0) s_capPos = 0;
}

static bool playMp3Stream() {
  if (!s_mp3Dec) { s_mp3Dec = MP3InitDecoder(); if (!s_mp3Dec) return false; }
  HMP3Decoder dec = s_mp3Dec;
  // PCM 输出缓冲也放 PSRAM（4.6KB 内部 RAM，同理切碎堆）
  static int16_t* pcm = nullptr;
  if (!pcm) {
    pcm = (int16_t*)heap_caps_malloc(2 * 1152 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) pcm = (int16_t*)malloc(2 * 1152 * sizeof(int16_t));
    if (!pcm) return false;
  }
  bool any = false;
  int pos = 0;
  while (pos < (int)mp3StreamLen) {
    int off = MP3FindSyncWord((unsigned char*)mp3StreamBuf + pos, mp3StreamLen - pos);
    if (off < 0) { pos = mp3StreamLen; break; }   // 无同步字：等续
    const uint8_t* p = mp3StreamBuf + pos + off;
    int bytesLeft = mp3StreamLen - pos - off;
    int r = MP3Decode(dec, (unsigned char**)&p, &bytesLeft, pcm, 0);
    if (r == ERR_MP3_INDATA_UNDERFLOW || r == ERR_MP3_MAINDATA_UNDERFLOW) break;  // 帧跨网络分块未收全：留缓冲头等续块（原来扔掉=每 26ms 爆音）
    if (r != ERR_MP3_NONE) { pos += off + 1; continue; }                // 真非法帧：跳过同步字
    int used = mp3StreamLen - pos - off - bytesLeft;
    pos += off + used;
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(dec, &fi);
    size_t samples = fi.outputSamps;
    if (samples > 0) {
      int pk = 0;
      for (size_t i = 0; i < samples; i++) { int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > pk) pk = a; }
      if (pk > s_ttsPeak) s_ttsPeak = pk;
      s_ttsFrames++; s_ttsSamples += samples;
      if (s_capOn) captureFrame(pcm, samples, fi.samprate ? fi.samprate : 24000);
      audio.playPcm(pcm, samples, fi.samprate ? fi.samprate : 24000);
      any = true;
    }
    if (audio.interrupted()) break;
  }
  if (pos > 0) {
    if (mp3StreamLen > (size_t)pos) memmove(mp3StreamBuf, mp3StreamBuf + pos, mp3StreamLen - pos);
    mp3StreamLen -= pos;
  }
  return any;
}

// 收尾：再刷一次（turn.end 后缓冲里可能还有不足一帧的头——实际 mp3 帧独立，最后一次 play 已播光）
static bool mp3StreamFlush() { return playMp3Stream(); }

bool EdgeTts::speak(const String& text, const String& voice, const String& rate, bool interruptible) {
  lastError = "";
  s_ttsBytes = s_ttsFrames = s_ttsSamples = 0; s_ttsPeak = 0;
  time_t now = time(nullptr);
  if (now < 1700000000) { lastError = "NTP 未同步，无法生成鉴权"; return false; }

  String cid = uuidNoDash();
  String path = String("/consumer/speech/synthesize/readaloud/edge/v1?TrustedClientToken=") + TCT +
    "&Sec-MS-GEC=" + genSecMsGec() + "&Sec-MS-GEC-Version=" + GEC_VER + "&ConnectionId=" + cid;

  WsClient ws;
  String cookie = "Cookie: muid=" + uuidNoDash() + ";\r\n";
  String extra = String("Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n") +
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0\r\n" +
    cookie;
  if (!ws.connect(EDGE_HOST, 443, path.c_str(), extra.c_str())) {
    lastError = ws.lastError;
    return false;
  }

  // speech.config
  String ts = jsDate();
  String cfgmsg = "X-Timestamp:" + ts + "\r\nContent-Type:application/json; charset=utf-8\r\nPath:speech.config\r\n\r\n"
    "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{\"sentenceBoundaryEnabled\":\"false\",\"wordBoundaryEnabled\":\"false\"},"
    "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}";
  if (!ws.sendText(cfgmsg)) { lastError = "发送 speech.config 失败"; ws.stop(); return false; }

  // SSML
  String esc;
  esc.reserve(text.length() + 8);
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '&') esc += "&amp;";
    else if (c == '<') esc += "&lt;";
    else if (c == '>') esc += "&gt;";
    else esc += c;
  }
  String ssml = "X-RequestId:" + cid + "\r\nContent-Type:application/ssml+xml\r\nX-Timestamp:" + ts +
    "Z\r\nPath:ssml\r\n\r\n<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='zh-CN'>"
    "<voice name='" + voice + "'><prosody pitch='+0Hz' rate='" + rate + "' volume='+0%'>" + esc +
    "</prosody></voice></speak>";
  if (!ws.sendText(ssml)) { lastError = "发送 SSML 失败"; ws.stop(); return false; }

  // 收音频：流式——到一个块喂一块，喂后立刻解码播放（首帧 ~100ms 出声，小智式流水线）
  mp3StreamReset();
  audio.bargeInEnable(interruptible);
  bool audioRecv = false, turnEnd = false;
  uint32_t lastProgress = millis();          // 收到音频/文本就续期：30s 只限制"无进展空闲"
  while (!turnEnd) {
    if (millis() - lastProgress > 30000) { lastError = "30s 无进展超时"; break; }
    int fr = ws.poll(3000);
    if (fr < 0) { lastError = "连接中断"; break; }
    if (fr == 0) continue;
    if (fr == 1) {
      lastProgress = millis();
      if (ws.textPayload().indexOf("Path:turn.end") >= 0) turnEnd = true;
    } else {
      const uint8_t* d = ws.binPayload();
      size_t n = ws.binLen();
      if (n < 2) continue;
      size_t hdrLen = ((size_t)d[0] << 8) | d[1];
      if (hdrLen + 2 > n) continue;
      s_ttsBytes += (uint32_t)(n - hdrLen - 2);
      feedMp3(d + hdrLen + 2, n - hdrLen - 2);
      if (playMp3Stream()) audioRecv = true;
      lastProgress = millis();
      if (audio.interrupted()) break;
    }
  }
  ws.stop();
  if (!audioRecv) {
    Serial.printf("[TTS] 没解出音频：收到 %uB / 解码 %u 帧（%s）\n",
                  (unsigned)s_ttsBytes, (unsigned)s_ttsFrames,
                  lastError.length() ? lastError.c_str() : "无错误信息");
    lastError = lastError.length() ? lastError : "未收到音频";
    return false;
  }
  if (audio.interrupted()) return true;      // 被打断：算成功（说了半截）
  mp3StreamFlush();
  Serial.printf("[TTS] 音频 %uB → 解码 %u 帧/%u 样本，峰值 %d，I2S 实写 %uB\n",
                (unsigned)s_ttsBytes, (unsigned)s_ttsFrames, (unsigned)s_ttsSamples, s_ttsPeak,
                (unsigned)laapI2sBytes());
  return true;
}

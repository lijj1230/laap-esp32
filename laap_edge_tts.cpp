#include "laap_edge_tts.h"
#include "laap_ws.h"
#include "laap_audio.h"
#include "laap_display.h"   // 仅用于眨眼刷新保持
#include <WiFiClientSecure.h>
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
static uint8_t mp3StreamBuf[16 * 1024];  // MP3 帧最大 ~1.5KB，16KB 很宽裕
static size_t mp3StreamLen = 0;

static void mp3StreamReset() { mp3StreamLen = 0; }
static bool playMp3Stream();   // 前向声明：feedMp3 满时会先播腾空间

static void feedMp3(const uint8_t* d, size_t n) {
  if (mp3StreamLen + n > sizeof(mp3StreamBuf)) {
    playMp3Stream();                                     // 满：先播掉腾空间（原整块丢弃=成段杂音）
    if (mp3StreamLen + n > sizeof(mp3StreamBuf)) n = sizeof(mp3StreamBuf) - mp3StreamLen;  // 仍放不下：截尾保帧头
  }
  if (n == 0) return;
  memcpy(mp3StreamBuf + mp3StreamLen, d, n);
  mp3StreamLen += n;
}

// 把缓冲里所有完整帧解码播掉；半帧残余留在缓冲头等下一块
// 解码器全局持久化：MP3 位池跨帧引用前帧数据，每调用重建丢状态→后续帧解码失败/爆音
static HMP3Decoder s_mp3Dec = nullptr;
static uint32_t s_playedSamps = 0;   // 本次 speak 累计播放样本数（诊断）
static bool playMp3Stream() {
  if (!s_mp3Dec) { s_mp3Dec = MP3InitDecoder(); if (!s_mp3Dec) return false; }
  HMP3Decoder dec = s_mp3Dec;
  static int16_t pcm[2 * 1152];
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
      audio.playPcm(pcm, samples, fi.samprate ? fi.samprate : 24000);
      s_playedSamps += samples;
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
  s_playedSamps = 0;
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
      feedMp3(d + hdrLen + 2, n - hdrLen - 2);
      if (playMp3Stream()) audioRecv = true;
      lastProgress = millis();
      if (audio.interrupted()) break;
    }
  }
  ws.stop();
  Serial.printf("[TTS] 播放 %.1f 秒音频（%u 样本）\n", s_playedSamps / 24000.0, s_playedSamps);
  if (!audioRecv) { lastError = lastError.length() ? lastError : "未收到音频"; return false; }
  if (audio.interrupted()) return true;      // 被打断：算成功（说了半截）
  mp3StreamFlush();
  return true;
}

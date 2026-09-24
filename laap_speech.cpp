#include "laap_speech.h"
#include "laap_config.h"
#include "laap_audio.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>

AsrClient asr;
VolcTts volcTts;

size_t wavWrap(const int16_t* pcm, size_t bytes, uint8_t* out, size_t outCap) {
  // 标准 44 字节 WAV 头（RIFF/WAVE/fmt /data 四块齐全，小端）
  uint32_t sr = 16000, ch = 1, bits = 16;
  uint32_t dataLen = bytes;
  uint32_t byteRate = sr * ch * bits / 8;
  uint8_t hdr[44] = {
    'R','I','F','F', 0,0,0,0,        'W','A','V','E',   // +8 = 36+dataLen
    'f','m','t',' ', 16,0,0,0,                          // fmt 块长 16
    1,0,                   ch&0xFF,(ch>>8)&0xFF,        // PCM 格式、声道
    (sr)&0xFF,(sr>>8)&0xFF,(sr>>16)&0xFF,(sr>>24)&0xFF, // 采样率
    0,0,0,0,                                            // 字节率（下方填）
    2,0, 16,0,                                          // 块对齐 ch*bits/8、位深
    'd','a','t','a', 0,0,0,0,                           // data 块长（下方填）
  };
  auto put32 = [&](int off, uint32_t v) {
    hdr[off] = v & 0xFF; hdr[off+1] = (v>>8)&0xFF; hdr[off+2] = (v>>16)&0xFF; hdr[off+3] = (v>>24)&0xFF;
  };
  put32(4, 36 + dataLen);
  put32(16, byteRate);
  put32(40, dataLen);
  if (44 + dataLen > outCap) dataLen = (outCap > 44) ? outCap - 44 : 0;
  put32(4, 36 + dataLen); put32(40, dataLen);          // 截断后重填两处长度
  memcpy(out, hdr, 44);
  if (dataLen) memcpy(out + 44, pcm, dataLen);
  return 44 + dataLen;
}

// 通用 HTTPS POST，返回 HTTP 状态与响应体
static WiFiClientSecure g_warm;            // ASR 预热连接（录音期间完成 TLS 握手）
static bool g_warmOk = false;
static String g_warmHost;
static int g_warmPort = 443;

// 录音开始前调用：后台把 TLS 握手做完（小智"录传并行"思想的适配——握手最耗时且与录音无依赖）
// 预热连接的互斥保护（asrwarm 任务与主线程 transcribe 并发访问 g_warm）
static SemaphoreHandle_t g_warmMtx = nullptr;
static void warmLock()   { if (!g_warmMtx) g_warmMtx = xSemaphoreCreateMutex(); xSemaphoreTake(g_warmMtx, portMAX_DELAY); }
static void warmUnlock() { xSemaphoreGive(g_warmMtx); }

void AsrClient::warmup() {
  if (WiFi.status() != WL_CONNECTED) return;   // 原 !x==y 优先级错恒 false
  warmLock();
  String base(cfg.s.asrBase);
  while (base.endsWith("/")) base.remove(base.length() - 1);
  bool dash = base.indexOf("dashscope") >= 0;
  String url = base + (dash ? "/api/v1/services/audio/asr/transcription" : "/audio/transcriptions");
  int dp = url.indexOf("://"); if (dp < 0) return;
  int hp = url.indexOf('/', dp + 3);
  g_warmHost = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = url.substring(hp);
  g_warmPort = 443;
  if (g_warmHost.indexOf(':') >= 0) {
    g_warmPort = g_warmHost.substring(g_warmHost.indexOf(':') + 1).toInt();
    g_warmHost = g_warmHost.substring(0, g_warmHost.indexOf(':'));
  }
  (void)path;
  g_warmOk = false;
  g_warm.setTimeout(8);
  g_warm.setInsecure();   // 缺这句 WiFiClientSecure 无证书配置 connect() 恒败（预热曾是死代码）
  if (g_warm.connect(g_warmHost.c_str(), g_warmPort)) g_warmOk = true;  // TLS 握手在此完成
  warmUnlock();
}

static void warmupInvalidate() { warmLock(); g_warmOk = false; g_warm.stop(); warmUnlock(); }

bool AsrClient::warmAlive() {
  warmLock();
  bool alive = g_warmOk && g_warm.connected();
  warmUnlock();
  return alive;
}
// 通用 HTTPS POST（warm=已握手的 ASR 预热连接，用后即失效），返回 HTTP 状态与响应体
static int httpsPost(const String& url, const String& contentType, const uint8_t* body, size_t bodyLen,
                     const char* bearer, String& respOut, WiFiClient* warm = nullptr) {
  int dp = url.indexOf("://");
  if (dp < 0) return -1;
  bool tls = url.startsWith("https");
  int hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  int port = tls ? 443 : 80;
  if (host.indexOf(':') >= 0) {
    port = host.substring(host.indexOf(':') + 1).toInt();
    host = host.substring(0, host.indexOf(':'));
  }
  WiFiClient plain;
  WiFiClientSecure secure;
  WiFiClient* c = tls ? (WiFiClient*)&secure : (WiFiClient*)&plain;
  if (tls) secure.setInsecure();
  c->setTimeout(20);
  bool reused = false;
  if (warm && warm->connected()) {          // 预热连接：TLS 已握手，直接发
    c = warm; reused = true;
  } else if (!c->connect(host.c_str(), port)) return -2;
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
    (bearer ? String("\r\nAuthorization: Bearer ") + bearer : "") +
    "\r\nContent-Type: " + contentType +
    "\r\nContent-Length: " + bodyLen + "\r\nConnection: close\r\n\r\n";
  c->print(req);
  const int CHUNK = 4096;
  static uint8_t buf[CHUNK];
  size_t sent = 0;
  while (sent < bodyLen) {
    int n = (bodyLen - sent > CHUNK) ? CHUNK : (bodyLen - sent);
    memcpy(buf, body + sent, n);
    c->write(buf, n);
    sent += n;
  }
  String resp;
  uint32_t t0ms = millis();
  while (c->connected() && millis() - t0ms < 40000) {  // 差值比较：回绕安全
    while (c->available()) { resp += (char)c->read(); if (resp.length() > 200000) break; }
    if (resp.length() > 200000) break;
    delay(2);
  }
  c->stop();
  int sp = resp.indexOf(' ');
  int code = sp > 0 ? resp.substring(sp + 1, sp + 4).toInt() : 0;
  int bs = resp.indexOf("\r\n\r\n");
  // chunked 简单拼接（复用 LLM 客户端同款逻辑）
  String payload = bs > 0 ? resp.substring(bs + 4) : resp;
  if (payload.indexOf("{\"") < 0 && payload.length() > 0) {
    String clean; int pos = 0;
    while (pos < (int)payload.length()) {
      int nl = payload.indexOf('\n', pos);
      String line = (nl < 0) ? payload.substring(pos) : payload.substring(pos, nl);
      line.trim();
      bool allHex = line.length() > 0;
      for (unsigned int ci = 0; ci < line.length() && allHex; ci++)
        if (!isHexadecimalDigit(line[ci])) allHex = false;
      if (!allHex) clean += line;
      if (nl < 0) break;
      pos = nl + 1;
    }
    payload = clean;
  }
  respOut = payload;
  return code;
}

static bool extractJsonStr(const String& j, const char* key, String& out) {
  String pat = String("\"") + key + "\"";
  int i = j.indexOf(pat);
  while (i >= 0) {
    int q = i + pat.length();
    while (q < (int)j.length() && (j[q] == ' ' || j[q] == ':')) q++;
    if (j[q] == '"') {
      q++;
      String v;
      while (q < (int)j.length()) {
        char c = j[q];
        if (c == '\\' && q + 1 < (int)j.length()) { v += j[q + 1]; q += 2; continue; }
        if (c == '"') break;
        v += c; q++;
      }
      out = v;
      return true;
    }
    i = j.indexOf(pat, q);
  }
  return false;
}

// 单服务商转写：base 为空返回 -1（跳过）；其余同 httpsPost 语义
static int transcribeOnce(const char* baseC, const char* keyC, const char* modelC,
                          const uint8_t* wav, size_t wavLen, String& text, String& err,
                          WiFiClient* warm = nullptr) {
  String base(baseC);
  if (!base.length() || !String(keyC).length()) { err = "未配置"; return -1; }
  while (base.endsWith("/")) base.remove(base.length() - 1);

  bool isDashscope = base.indexOf("dashscope") >= 0;
  // OpenAI 兼容: {base}/audio/transcriptions；百炼原生: {base}/api/v1/services/audio/asr/transcription
  String url = base + (isDashscope ? "/api/v1/services/audio/asr/transcription"
                                   : "/audio/transcriptions");
  String bound = "----laap" + String(esp_random(), HEX);
  String head = String("--") + bound +
    "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + modelC + "\r\n" +
    "--" + bound +
    "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + bound + "--\r\n";
  size_t total = head.length() + wavLen + tail.length();
  uint8_t* body = (uint8_t*)malloc(total);
  if (!body) { err = "内存不足"; return -2; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), wav, wavLen);
  memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());

  String resp;
  int code = httpsPost(url, String("multipart/form-data; boundary=") + bound, body, total, keyC, resp, warm);
  free(body);
  if (code != 200) { err = "HTTP " + String(code) + " " + resp.substring(0, 120); return code; }
  // OpenAI 兼容取 "text"；百炼原生取 "headers"...(响应在 output.text / 或结果数组内)，宽松依次试
  if (!extractJsonStr(resp, "text", text))
    if (!extractJsonStr(resp, "transcript", text))
      extractJsonStr(resp, "result", text);
  if (!text.length()) { err = "响应无 text: " + resp.substring(0, 120); return -3; }
  err = "";
  return 200;
}

String AsrClient::transcribe(const int16_t* pcm16k, size_t bytes, String& err) {
  if (String(cfg.s.asrKey).length() == 0) { err = "ASR Key 未配置"; return ""; }
  // WAV 包装
  size_t wavCap = bytes + 64;
  uint8_t* wav = (uint8_t*)malloc(wavCap);
  if (!wav) { err = "内存不足"; return ""; }
  size_t wavLen = wavWrap(pcm16k, bytes, wav, wavCap);

  String text;
  warmLock();
  WiFiClient* warmConn = g_warmOk ? (WiFiClient*)&g_warm : nullptr;
  int code = transcribeOnce(cfg.s.asrBase, cfg.s.asrKey, cfg.s.asrModel, wav, wavLen, text, err, warmConn);
  if (g_warmOk) { g_warmOk = false; g_warm.stop(); }   // 热连接一次性（Connection: close）
  warmUnlock();
  if (code != 200 && code != -1) {
    // 主 ASR 失败 → 备用 ASR 自动回退（配置了才试）
    Serial.printf("[ASR] 主服务商失败(%s)，尝试备用…\n", err.c_str());
    String text2;
    int code2 = transcribeOnce(cfg.s.asr2Base, cfg.s.asr2Key, cfg.s.asr2Model, wav, wavLen, text2, err);
    if (code2 == 200) { free(wav); return text2; }
  }
  free(wav);
  if (code != 200) return "";
  err = "";
  return text;
}

bool VolcTts::speak(const String& text, String& err) {
  if (String(cfg.s.volcAppid).length() == 0 || String(cfg.s.volcToken).length() == 0) {
    err = "火山 TTS 未配置";
    return false;
  }
  String reqid;
  for (int i = 0; i < 32; i++) reqid += String(esp_random() % 16, HEX);
  String body = String("{\"app\":{\"appid\":\"") + cfg.s.volcAppid + "\",\"token\":\"access_token\",\"cluster\":\"volcano_tts\"},"
    "\"audio\":{\"encoding\":\"wav\",\"voice_type\":\"" + cfg.s.volcVoice + "\",\"speed_ratio\":1.0},"
    "\"request\":{\"reqid\":\"" + reqid + "\",\"text\":\"";
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '"' || c == '\\') body += '\\';
    else if (c == '\n') { body += "\\n"; continue; }   // 控制字符必须转义，否则请求体非法 JSON
    else if (c == '\r') { continue; }
    else if (c == '\t') { body += "\\t"; continue; }
    body += c;
  }
  body += "\",\"operation\":\"query\"}}";

  String resp;
  int code = httpsPost("https://opens.bytedance.com/api/v1/tts", "application/json",
                       (const uint8_t*)body.c_str(), body.length(), cfg.s.volcToken, resp);
  if (code != 200) { err = "HTTP " + String(code); return false; }
  String b64;
  if (!extractJsonStr(resp, "data", b64) || b64.length() < 100) { err = "响应无音频数据"; return false; }

  size_t decLen = 0;
  mbedtls_base64_decode(nullptr, 0, &decLen, (const uint8_t*)b64.c_str(), b64.length());
  uint8_t* wav = (uint8_t*)malloc(decLen + 8);
  size_t olen = 0;
  mbedtls_base64_decode(wav, decLen, &olen, (const uint8_t*)b64.c_str(), b64.length());
  // 解 WAV：找 fmt 采样率与 data 块
  uint32_t rate = 24000; const uint8_t* dp = nullptr; uint32_t dlen = 0;
  for (size_t i = 12; i + 8 <= olen;) {
    uint32_t cid2 = *(uint32_t*)(wav + i);
    uint32_t sz = *(uint32_t*)(wav + i + 4);
    if (cid2 == 0x20746d66) { rate = *(uint32_t*)(wav + i + 12); }
    else if (cid2 == 0x61746164) { dp = wav + i + 8; dlen = sz; break; }
    i += 8 + sz + (sz & 1);
  }
  bool ok = false;
  if (dp && dlen > 44) {
    audio.bargeInEnable(true);
    ok = audio.playPcm((const int16_t*)dp, dlen / 2, rate);
  }
  free(wav);
  if (!ok) err = "播放失败";
  return ok;
}

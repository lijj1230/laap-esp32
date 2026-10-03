#include "laap_speech.h"
#include "laap_config.h"
#include "laap_audio.h"
#include "laap_llm.h"   // laapHttpRead（HTTP 响应按帧精确读取）
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
  put32(28, byteRate);        // 偏移 28 = 字节率。曾误写成 put32(16,...)：那里是 fmt 块长（应为 16），
                              // 结果 fmt 块长被写成 32000、字节率为 0 —— 整份 WAV 非法，
                              // 服务端解码失败一律回 HTTP 500（这就是 ASR 一直不通的根因）
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
static volatile bool g_warming = false;   // v3.76e：预热在飞标志（连按 N 次/loopTick 连发只跑一次握手）
static void warmLock()   { if (!g_warmMtx) g_warmMtx = xSemaphoreCreateMutex(); xSemaphoreTake(g_warmMtx, portMAX_DELAY); }
static void warmUnlock() { xSemaphoreGive(g_warmMtx); }

void AsrClient::begin() { if (!g_warmMtx) g_warmMtx = xSemaphoreCreateMutex(); }   // v3.76e：setup 里显式建锁，消除懒建竞态

void AsrClient::warmup() {
  if (WiFi.status() != WL_CONNECTED) return;   // 原 !x==y 优先级错恒 false
  if (g_warming) return;                       // v3.76e：在飞去重——阻塞期连按 N 次 =
                                               // N 个 12KB 栈任务在锁上排队 + N 轮握手（堆最紧时雪上加霜）
  warmLock();
  if (g_warming) { warmUnlock(); return; }     // 双检：两任务同时过首检的兜底
  g_warming = true;
  String base(cfg.s.asrBase);
  while (base.endsWith("/")) base.remove(base.length() - 1);
  bool dash = base.indexOf("dashscope") >= 0;
  String url = base + (dash ? "/api/v1/services/audio/asr/transcription" : "/audio/transcriptions");
  int dp = url.indexOf("://");
  if (dp < 0) { g_warming = false; warmUnlock(); return; }   // 畸形 asrBase（少打 https://）：持锁早退=永久死锁整个主循环（v3.51 审计）
  int hp = url.indexOf('/', dp + 3);
  g_warmHost = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  g_warmPort = 443;
  if (g_warmHost.indexOf(':') >= 0) {
    g_warmPort = g_warmHost.substring(g_warmHost.indexOf(':') + 1).toInt();
    g_warmHost = g_warmHost.substring(0, g_warmHost.indexOf(':'));
  }
  g_warmOk = false;
  g_warm.setTimeout(8);
  g_warm.setInsecure();   // 缺这句 WiFiClientSecure 无证书配置 connect() 恒败（预热曾是死代码）
  if (g_warm.connect(g_warmHost.c_str(), g_warmPort)) g_warmOk = true;  // TLS 握手在此完成
  g_warming = false;
  warmUnlock();
}

static void warmupInvalidate() { warmLock(); g_warmOk = false; g_warm.stop(); g_warming = false; warmUnlock(); }

bool AsrClient::warmAlive() {
  if (!g_warmMtx) return false;
  if (xSemaphoreTake(g_warmMtx, 0) != pdTRUE) return true;   // v3.76e：握手进行中视作"活"——
                                                             // 拿锁等到握手完会把 loop 拖 ≤9s
  bool alive = g_warmOk && g_warm.connected();
  xSemaphoreGive(g_warmMtx);
  return alive;
}

void AsrClient::warmDrop() {
  warmupInvalidate();
}
// 通用 HTTPS POST（warm=已握手的 ASR 预热连接，用后即失效），返回 HTTP 状态与响应体
// readBudgetMs：响应读预算（v3.76e 总预算的一部分，防网络半死时读满 40s）
static int httpsPost(const String& url, const String& contentType, const uint8_t* body, size_t bodyLen,
                     const char* bearer, String& respOut, WiFiClient* warm = nullptr, uint32_t readBudgetMs = 40000) {
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
    "\r\nUser-Agent: LAAP/3.2\r\nAccept: application/json" +
    "\r\nContent-Length: " + bodyLen + "\r\nConnection: close\r\n\r\n";
  c->print(req);
  const int CHUNK = 4096;
  static uint8_t buf[CHUNK];
  size_t sent = 0;
  while (sent < bodyLen) {
    int n = (bodyLen - sent > CHUNK) ? CHUNK : (bodyLen - sent);
    memcpy(buf, body + sent, n);
    // 短写保护：write 可能少写（socket 缓冲满），必须补写完，否则 multipart 截断→服务端 500
    size_t w = 0;
    while (w < (size_t)n) {
      int k = c->write(buf + w, n - w);
      if (k <= 0) { c->stop(); return -3; }   // 连接断了
      w += k;
    }
    sent += n;
  }
  String hdrs, payload;
  laapHttpRead(c, readBudgetMs, hdrs, payload, 600000);   // 火山 wav+base64 ≈64KB/s：200KB 只够 3 秒，长回复必败（v3.56 审计）
  c->stop();
  int sp = hdrs.indexOf(' ');
  int code = sp > 0 ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (code != 200 && code > 0) {
    // 非 200 打印响应头+正文前 220 字符，服务端错误正文一看便知
    Serial.printf("[NET] %s → HTTP %d\n%.220s\n", path.c_str(), code, (hdrs + payload).c_str());
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
                          WiFiClient* warm = nullptr, uint32_t readBudgetMs = 40000) {
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
  uint32_t tSendStart = millis();
  int code = httpsPost(url, String("multipart/form-data; boundary=") + bound, body, total, keyC, resp, warm, readBudgetMs);
  size_t sentLen = total;
  free(body);
  // 把"发了多大、花了多久、HTTP 几"打出来：服务端排队慢 vs 请求被拒，一眼可分
  // （实测 SiliconFlow 免费 SenseVoiceSmall 排队时，同一个 32KB 请求会在 1s~40s 之间变化）
  Serial.printf("[ASR] %s 上行 %uB 用时 %lums → HTTP %d\n",
                String(baseC).indexOf("dashscope") >= 0 ? "百炼" : "OpenAI兼容",
                (unsigned)sentLen, (unsigned)(millis() - tSendStart), code);
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
  // v3.76e 总预算 45s：主请求（读预算随剩余时间收缩，最少 8s）→ 热连接重试 → 备用
  // 服务商，各自先查剩余预算。原来无总预算时三连发最坏 ≈3×60s——这段全在 loop 上下文，
  // 是"搜新闻后卡死聋哑"的最大单一阻塞源，且看门狗跑在 loop 里对此完全失明。
  // 健康链路实测 2-5s 出结果，45s 只砍病态尾部。
  const uint32_t deadline = millis() + 45000;
  auto remainMs = [&deadline]() -> uint32_t {
    int32_t r = (int32_t)(deadline - millis());
    return r > 8000 ? (uint32_t)r : 8000;   // 至少给 8s：连接+发送+短读还有救
  };
  warmLock();
  WiFiClient* warmConn = g_warmOk ? (WiFiClient*)&g_warm : nullptr;
  bool usedWarm = (warmConn != nullptr);
  int code = transcribeOnce(cfg.s.asrBase, cfg.s.asrKey, cfg.s.asrModel, wav, wavLen, text, err, warmConn, remainMs());
  if (g_warmOk) { g_warmOk = false; g_warm.stop(); }   // 热连接一次性（Connection: close）
  warmUnlock();
  // 预热连接是录音前握好的，服务端 keep-alive 超时/LB 回收可能在这几秒里把它关掉 →
  // 写失败（-3）而不是请求被拒。这种情况必须用新连接重发一次，否则 ASR 会在"看起来
  // 什么都正常"的情况下静默失败（实测坑）。
  if (usedWarm && code != 200 && (int32_t)(deadline - millis()) > 0) {
    Serial.printf("[ASR] 热连接失败(%s) → 改用新连接重试\n", err.c_str());
    String t2b, e2b;
    int c2b = transcribeOnce(cfg.s.asrBase, cfg.s.asrKey, cfg.s.asrModel, wav, wavLen, t2b, e2b, nullptr, remainMs());
    if (c2b == 200) { free(wav); err = ""; return t2b; }
    code = c2b; text = t2b; err = e2b;
  }
  // v3.76e：负数 code = 连接/写失败/URL 畸形——网络本身死了，备用服务商必然同样超时，
  // 只会白吃一个完整读预算；只有 HTTP 状态错误（code>0）才值得试备用。
  if (code > 0 && code != 200 && (int32_t)(deadline - millis()) > 0) {
    // 主 ASR 失败 → 备用 ASR 自动回退（配置了才试）
    Serial.printf("[ASR] 主服务商失败(%s)，尝试备用…\n", err.c_str());
    String errMain = err;                 // 备用失败时会写 err，别让它盖掉主服务的真报错
    String text2;
    int code2 = transcribeOnce(cfg.s.asr2Base, cfg.s.asr2Key, cfg.s.asr2Model, wav, wavLen, text2, err, nullptr, remainMs());
    if (code2 == 200) { free(wav); return text2; }
    err = errMain + " ｜ 备用: " + err;   // 两条都留着，排障时一眼看清是谁的问题
  }
  free(wav);
  if (code != 200) return "";
  err = "";
  return text;
}

String AsrClient::probe(int which) {
  const char* baseC = which ? cfg.s.asr2Base : cfg.s.asrBase;
  const char* keyC  = which ? cfg.s.asr2Key  : cfg.s.asrKey;
  const char* mdlC  = which ? cfg.s.asr2Model : cfg.s.asrModel;
  if (!String(baseC).length() || !String(keyC).length()) return "未配置";
  // 0.3s 440Hz 合成音：真发一次完整 multipart，连 200 响应都验到（空音频转不出字属正常）
  const size_t samples = 4800;
  int16_t* pcm = (int16_t*)malloc(samples * 2);
  if (!pcm) return "内存不足";
  for (size_t i = 0; i < samples; i++)
    pcm[i] = (int16_t)(6000.0f * sinf(i * 2.0f * PI * 440.0f / 16000.0f));
  size_t wavCap = samples * 2 + 64;
  uint8_t* wav = (uint8_t*)malloc(wavCap);
  if (!wav) { free(pcm); return "内存不足"; }
  size_t wavLen = wavWrap(pcm, samples * 2, wav, wavCap);
  free(pcm);
  String text, err;
  int code = transcribeOnce(baseC, keyC, mdlC, wav, wavLen, text, err, nullptr);
  free(wav);
  if (code == 200 || code == -3) return "";   // -3=响应无 text：端点/鉴权已确认，空音频属正常
  return err;
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
  if (decLen == 0 || decLen > 900000) { err = "音频数据长度异常"; return false; }
  uint8_t* wav = (uint8_t*)malloc(decLen + 8);
  if (!wav) { err = "解码缓冲分配失败"; return false; }
  size_t olen = 0;
  if (mbedtls_base64_decode(wav, decLen, &olen, (const uint8_t*)b64.c_str(), b64.length()) != 0 || olen == 0) {
    free(wav);
    err = "音频 base64 解码失败";
    return false;
  }
  // 解 WAV：找 fmt 采样率与 data 块。逐字节拼 32 位读——Xtensa 上 *(uint32_t*) 非对齐
  // 直接 LoadStoreAlignment panic（fmt 块带 cbSize=18 字节时后续块错位 2 字节就踩中）；
  // 块声明长按实际收口，防止 playPcm 越界读
  uint32_t rate = 24000; const uint8_t* dp = nullptr; uint32_t dlen = 0;
  for (size_t i = 12; i + 8 <= olen;) {
    uint32_t cid2 = (uint32_t)wav[i] | ((uint32_t)wav[i+1] << 8) | ((uint32_t)wav[i+2] << 16) | ((uint32_t)wav[i+3] << 24);
    uint32_t sz   = (uint32_t)wav[i+4] | ((uint32_t)wav[i+5] << 8) | ((uint32_t)wav[i+6] << 16) | ((uint32_t)wav[i+7] << 24);
    if (i + 8 + sz > olen) break;                       // 块声明长超过实际数据：残包，到此为止
    if (cid2 == 0x20746d66) {                           // "fmt "
      rate = (uint32_t)wav[i+12] | ((uint32_t)wav[i+13] << 8) | ((uint32_t)wav[i+14] << 16) | ((uint32_t)wav[i+15] << 24);
    } else if (cid2 == 0x61746164) {                    // "data"
      dp = wav + i + 8; dlen = sz; break;
    }
    i += 8 + sz + (sz & 1);
  }
  bool ok = false;
  if (dp && dlen > 44) {
    // 不开 barge-in：37.5dB 麦克风增益下能量门会被自身漏音误触发砍播（Edge 通道同款教训），
    // 且开关残留会影响后续所有播放（诊断命令也遭殃）
    ok = audio.playPcm((const int16_t*)dp, dlen / 2, rate);
  }
  free(wav);
  if (!ok) err = "播放失败";
  return ok;
}

#include "laap_llm.h"
#include "laap_config.h"
#include <esp_heap_caps.h>   // 最大连续块（TLS 握手要一整块，不看总量）
#include <WiFiClientSecure.h>
#include <WiFi.h>

LlmClient llm;

String LlmClient::buildUrl() const {
  String base(cfg.s.llmBase);
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  // 已带 /v1 /v3 /v4 等版本段 → 直接拼 chat/completions。
  // 段长限 2-3 且不可是整段域名（"v1.example.com" 不该被当成版本段——v3.51）
  int slash = base.lastIndexOf('/');
  String last = slash >= 0 ? base.substring(slash + 1) : "";
  bool hasVer = (last.length() >= 2 && last.length() <= 3 && last[0] == 'v' && isDigit(last[1]));
  if (base.indexOf("/chat/completions") >= 0) return base;
  if (hasVer) return base + "/chat/completions";
  return base + "/v1/chat/completions";
}

String LlmClient::jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 16);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "";     break;
      case '\t': o += "\\t";  break;
      default:
        // 其余控制字符必须转义成 \uXXXX（裸 0x01-0x1F 是非法 JSON，DeepSeek 严格校验直接 400；
        // 与 sanitizeUtf8 家族同源教训——v3.51 审计补）
        if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04X", (unsigned char)c); o += b; }
        else o += c;
    }
  }
  return o;
}

// \uXXXX 解码辅助（v3.51）：网关以 ensure_ascii 返回中文时，旧代码整段跳过=缺字/空回复。
// v3.55 起为全仓唯一实现（web 的 jsonField 也用），声明在 laap_llm.h
unsigned laapHex4(const String& s, int i) {
  unsigned v = 0;
  for (int k = 0; k < 4 && i + k < (int)s.length(); k++) {
    char c = s[i + k]; v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
  }
  return v;
}
void laapAppendUtf8(String& o, unsigned cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// 宽松提取 "key":"value"（处理 \" \\ \n \t \uXXXX 转义）
bool LlmClient::extractStringField(const String& json, const char* key, String& out) {
  String pat = String("\"") + key + "\"";
  int i = json.indexOf(pat);
  while (i >= 0) {
    int j = i + pat.length();
    while (j < (int)json.length() && (json[j] == ' ' || json[j] == ':')) j++;
    if (json[j] == '"') {
      j++;
      String v;
      while (j < (int)json.length()) {
        char c = json[j];
        if (c == '\\' && j + 1 < (int)json.length()) {
          char n = json[j + 1];
          if (n == 'n') v += '\n';
          else if (n == 't') v += '\t';
          else if (n == 'u' && j + 5 < (int)json.length()) {
            // \uXXXX 解码（v3.51）：含代理对
            unsigned cp = laapHex4(json, j + 2);
            if (cp >= 0xD800 && cp <= 0xDBFF && j + 11 < (int)json.length() &&
                json[j + 6] == '\\' && json[j + 7] == 'u') {
              unsigned lo = laapHex4(json, j + 8);
              if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j += 6; }
            }
            laapAppendUtf8(v, cp);
            j += 4;
          } else v += n; // \" \\ \/ \b \f
          j += 2;
        } else if (c == '"') break;
        else { v += c; j++; }
      }
      out = v;
      return true;
    }
    i = json.indexOf(pat, j);
  }
  return false;
}

// ================= 连接保活复用（v3.55）=================
// 每次 TLS 握手 = 1-2s 延时 + 一次内部堆峰值。聊天与向量各有一个保活槽位：
// 响应"按帧干净结束"（Content-Length 读满 / chunked 终结块完整）且服务器没说
// close 才保留连接；超时、断连、帧不完整一律丢弃，绝不带着脏状态复用。
// 并发纪律：llmTask 与网页连通性测试可能同时进 chatMsgsContinue——槽位由
// s_connMtx 保护，同一槽位同时只被一个任务持有（inUse 标志在锁内置位）。
struct LlmKeepConn {
  WiFiClient* c = nullptr;
  String host; int port = 0; bool tls = false;
  uint32_t lastUse = 0;
  bool inUse = false;
};
static LlmKeepConn s_chatConn, s_embConn;
static SemaphoreHandle_t s_connMtx = nullptr;   // laapNetInit 里创建（setup 早于一切网络）
static const uint32_t LLM_KEEPALIVE_MS = 180000; // 闲置 3 分钟以上的连接不复用（服务商回收更短也无妨，connected() 已死会被丢弃）

// 新建连接（带 3 次退避重试）：connect 失败不花 token（请求还没发出去），
// 碎片多时等几百毫秒回收往往就通了，比让上层收到一个"连接失败"有用得多
static WiFiClient* llmFreshConnect(bool useTls, const String& host, int port, String& lastError) {
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFiClient* c = useTls ? (WiFiClient*)(new WiFiClientSecure) : new WiFiClient;
    if (useTls) ((WiFiClientSecure*)c)->setInsecure(); // 端侧自签策略见 README
    c->setTimeout(15000); // Stream 超时单位为 ms
    if (c->connect(host.c_str(), port)) return c;
    delete c;
    Serial.printf("[LLM] 连接失败（第 %d 次），最大连续块 %uKB，%dms 后重试\n",
                  attempt + 1,
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                  600 + attempt * 700);
    if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(600 + attempt * 700));   // 最后一次失败不必再白等 2s（v3.51）
  }
  lastError = "连接失败:" + host;
  return nullptr;
}

static WiFiClient* connAcquire(LlmKeepConn& slot, const String& host, int port,
                               bool tls, bool& reused) {
  reused = false;
  if (!s_connMtx || xSemaphoreTake(s_connMtx, pdMS_TO_TICKS(100)) != pdTRUE) return nullptr;
  if (slot.c && !slot.inUse) {
    if (slot.host != host || slot.port != port || slot.tls != tls ||
        millis() - slot.lastUse > LLM_KEEPALIVE_MS) {
      slot.c->stop(); delete slot.c; slot.c = nullptr;      // 过期/不匹配：丢弃
    } else {
      // 存活探测：connected() 内部会拉取下一条 TLS 记录，闲置连接上会一路阻塞到
      // SO_RCVTIMEO——先压到 10ms 探一次：EOF=服务器已关（丢弃），读超时=仍活着。
      // 探测后由调用方 setTimeout 恢复正常读超时（复用成功路径必须恢复）
      slot.c->setTimeout(10);
      if (!slot.c->connected()) { slot.c->stop(); delete slot.c; slot.c = nullptr; }
    }
  }
  WiFiClient* c = nullptr;
  if (slot.c && !slot.inUse) { slot.inUse = true; reused = true; c = slot.c; }
  xSemaphoreGive(s_connMtx);
  return c;
}

// 归还连接：clean=本次响应按帧干净结束。新建连接干净结束且槽位空 → 收养供下次复用。
// c 的生命周期在本函数内一次性处置（v3.56 审计修 P0：原实现对"复用且不干净"先在槽位
// 里 delete、尾部又按 keep=false 再 delete 同一指针 = 双重 delete/堆损坏）。
// 锁用 portMAX_DELAY：持锁段只有 10ms 探测与 stop，有界；拿不到锁就删会让槽位悬垂。
static void connFinish(LlmKeepConn& slot, WiFiClient* c, bool clean,
                       const String& host, int port, bool tls) {
  if (!c) return;
  bool owned = false;                       // true=锁内已处置完毕，尾部不再动
  if (s_connMtx && xSemaphoreTake(s_connMtx, portMAX_DELAY) == pdTRUE) {
    if (c == slot.c) {                      // 复用的那条：干净与否都已在槽位处置
      slot.inUse = false;
      if (clean) slot.lastUse = millis();
      else { slot.c->stop(); delete slot.c; slot.c = nullptr; }
      owned = true;
    } else if (clean && !slot.c && !slot.inUse) {   // 新建干净 + 槽位空 → 收养
      slot.c = c; slot.inUse = false;
      slot.host = host; slot.port = port; slot.tls = tls;
      slot.lastUse = millis();
      owned = true;
    }
    xSemaphoreGive(s_connMtx);
  }
  if (!owned) { c->stop(); delete c; }      // 只有"新建且未收养"走这里
}

// HTTP 响应按帧精确读取（全仓唯一实现：v3.55 的 chat/embed 读法推广到 search/vision/
// tools/speech，替代各自"读到关闭再启发式剥壳"的旧路径）。
// 返回 true = 按帧干净结束（keep-alive 连接可复用的判据）；false = 超时/断连/帧不完整/
// 超 maxBody（Connection:close 的调用方忽略返回值即可）。chunked 在读取时逐块精确剥离。
bool laapHttpRead(WiFiClient* c, uint32_t timeoutMs, String& hdrs, String& payload, int maxBody) {
  uint32_t t0 = millis();                              // 差值比较：49.7 天回绕安全
  hdrs = ""; hdrs.reserve(1024);
  while (millis() - t0 < timeoutMs) {                  // 阶段一：读到头结束 \r\n\r\n
    int ch = c->read();
    if (ch < 0) { if (!c->connected()) return false; delay(2); continue; }
    hdrs += (char)ch;
    if (hdrs.endsWith("\r\n\r\n")) break;
    if (hdrs.length() > 8192) return false;            // 头异常：当坏连接处理
  }
  if (!hdrs.endsWith("\r\n\r\n")) return false;

  String low = hdrs; low.toLowerCase();                // 头字段统一小写后检索（锚定行首，防 X-Content-Length 之类误匹配）
  bool srvClose = low.indexOf("\nconnection: close") >= 0 || low.indexOf("\nconnection:close") >= 0;
  bool chunked = false;
  { int i = low.indexOf("\ntransfer-encoding:");
    if (i >= 0 && low.indexOf("chunked", i) >= 0) chunked = true; }

  payload = ""; payload.reserve(12288);   // 与旧口径一致：聊天响应常 5-20KB，预分配免翻倍再分配链
  bool framedDone = false;
  if (chunked) {
    // <hex>[;ext]\r\n <data> \r\n ... 0\r\n [尾随头行...] \r\n
    int st = 0; long remain = 0; char sz[16]; int sl = 0;
    while (!framedDone && millis() - t0 < timeoutMs) {
      int ch = c->read();
      if (ch < 0) { if (!c->connected()) break; delay(2); continue; }
      if (st == 0) {                                   // 块大小行
        if (ch == '\n') {
          if (sl >= 0) sz[sl] = 0;                     // ';' 处已终止时 sz 原样可用
          remain = strtol(sz, nullptr, 16);
          sl = 0; st = (remain == 0) ? 3 : 1;
        } else if (ch == '\r') { /* 行尾 */ }
        else if (ch == ';') { if (sl >= 0) sz[sl] = 0; sl = -1; }  // 块扩展：hex 已定，后续不收集
        else if (sl >= 0 && sl < 15) sz[sl++] = (char)ch;
      } else if (st == 1) {                            // 块数据
        payload += (char)ch;
        if (payload.length() > (unsigned int)maxBody) return false;
        if (--remain == 0) st = 2;
      } else if (st == 2) {                            // 块尾 \r\n
        if (ch == '\n') st = 0;
      } else {                                         // 终结块后：尾随头直到空行
        if (ch == '\n') { if (sl == 0) framedDone = true; sl = 0; }
        else if (ch != '\r' && sl < 4096) sl++;
      }
    }
  } else {
    int i = low.indexOf("\ncontent-length:");
    if (i >= 0) {
      long cl = strtol(low.c_str() + i + 16, nullptr, 10);
      long got = 0;
      while (got < cl && millis() - t0 < timeoutMs) {
        int ch = c->read();
        if (ch < 0) { if (!c->connected()) break; delay(2); continue; }
        payload += (char)ch; got++;
        if (payload.length() > (unsigned int)maxBody) break;
      }
      framedDone = (got == cl);
    } else {
      // 无 Content-Length 也无 chunked：读到对端关闭为止（旧行为），连接不可复用
      while (millis() - t0 < timeoutMs) {
        int ch = c->read();
        if (ch < 0) { if (!c->connected()) break; delay(2); continue; }
        payload += (char)ch;
        if (payload.length() > (unsigned int)maxBody) break;
      }
    }
  }
  // 不在这里调 connected()：它会拉取下一条 TLS 记录、闲置时阻塞到读超时（见 connAcquire
  // 的探测注释）。服务器实际已关闭的情况，由下次 connAcquire 的 10ms 探测兜住
  return framedDone && !srvClose;
}

// ---- v3.70 流式 SSE 读取（对话提速核心）：逐 data: 行解析 delta.content 增量，
// 边收边按句切分，成句立即回调（调用方入播报队列 → TTS 流水线，首句 0.5~1s 出声）。
// skipFirstLine=true：首行（情绪词 happy/curious/…）整段跳过不外播，等出现 '\n' 才切句。
// 返回 true=收到 [DONE]（正常流尾）；false=超时/断连（调用方按 gotAny 决定用已收内容或回退）。
static bool readSseStream(WiFiClient* c, uint32_t timeoutMs, int maxBody, String& out,
                          LlmSentenceCb cb, void* ctx, bool skipFirstLine, bool& gotAny) {
  gotAny = false;
  uint32_t t0 = millis();
  String hdrs; hdrs.reserve(1024);
  while (millis() - t0 < timeoutMs) {                  // 头阶段：与 laapHttpRead 同读法
    int ch = c->read();
    if (ch < 0) { if (!c->connected()) return false; delay(2); continue; }
    hdrs += (char)ch;
    if (hdrs.endsWith("\r\n\r\n")) break;
    if (hdrs.length() > 8192) return false;
  }
  if (!hdrs.endsWith("\r\n\r\n")) return false;
  { int sp = hdrs.indexOf(' ');                        // 非 200：错误体是 JSON，交调用方回退非流式拿 message
    if (sp <= 0 || hdrs.substring(sp + 1, sp + 4).toInt() != 200) return false; }

  out = ""; out.reserve(4096);
  String line; line.reserve(768);
  size_t spokenUpTo = 0;                               // 已外播水位（out 内偏移）
  int emoCut = skipFirstLine ? -1 : 0;                 // -1=情绪行未结束；0=无需跳过
  bool sawDone = false;
  while (!sawDone && millis() - t0 < timeoutMs) {
    int ch = c->read();
    if (ch < 0) { if (!c->connected()) break; delay(2); continue; }
    if (ch != '\n') { if (line.length() < 4096) line += (char)ch; continue; }
    String ln = line; line = "";
    ln.trim();                                         // 行尾 \r 一并去掉
    if (!ln.startsWith("data:")) continue;             // 注释/event:/空行忽略
    String d = ln.substring(5); d.trim();
    if (d == "[DONE]") { sawDone = true; break; }
    String piece;
    if (!LlmClient::extractStringField(d, "content", piece) || !piece.length()) continue;
    gotAny = true;
    out += piece;
    if ((int)out.length() > maxBody) break;
    if (emoCut < 0) {
      // 情绪行结束判定放宽：模型常把表情词与正文连写（"curious。章鱼有三个心脏…"），
      // 换行不存在时就找"首个英文情绪词后紧跟的中日标点/空白"作为切点
      static const char* kEmo[] = {"happy","curious","excited","lonely","anxious","tired","calm"};
      int head = 0; while (head < (int)out.length() && (out[head]==' '||out[head]=='\r')) head++;
      bool emoHead = false;
      for (auto w : kEmo) {
        int wl = strlen(w);
        if (out.length() >= (unsigned)(head + wl)) {
          String h2 = out.substring(head, head + wl); h2.toLowerCase();
          if (h2 == w) { emoHead = true; head += wl; break; }
        }
      }
      if (emoHead) {
        while (head < (int)out.length() &&
               (out[head]==' '||out[head]=='\r'||out[head]=='。'||out[head]=='，'||
                out[head]=='.'||out[head]==','||out[head]=='\n')) head++;
        emoCut = head;                                  // 正文从标点串之后开始
      } else {
        int nl = out.indexOf('\n');
        // v3.72b 关键：无换行时**保持 -1 继续等**——首增量常只有情绪词的前几个字节
        // （如 "t"），此时 emoHead 必然匹配不上；提前定 0 会让后续到齐的 "tired\n"
        // 永远不被剥离，"tired" 直接成首句播出去（v3.72 实测回归）
        if (nl >= 0) emoCut = nl + 1;
        else if (out.length() >= 8) emoCut = 0;         // 8B 仍无换行且无情绪词头：真·无情绪行
        else continue;                                  // 太短：等下一个增量再判
      }
    }
    if ((int)spokenUpTo < emoCut) spokenUpTo = emoCut;  // 情绪段永不外播
    if (emoCut < 0) continue;                          // 情绪行还没结束：先不切句（防把 happy 念出来）
    while ((int)spokenUpTo < (int)out.length()) {
      int start = (int)spokenUpTo;
      int end = -1;
      for (int i = start; i < (int)out.length(); i++) {
        char c2 = out[i];
        if (c2 == '\n' || c2 == '。' || c2 == '！' || c2 == '？' ||
            c2 == '!' || c2 == '?' || c2 == '；' || c2 == ';') { end = i + 1; break; }
      }
      if (end < 0 && (int)out.length() - start >= 90) {  // 长句不等句末符：90 字节强制切（退到字符边界）
        int cut = start + 90;
        while (cut > start && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        end = cut;
      }
      if (end < 0) break;                              // 等更多增量
      String s = out.substring(start, end); s.trim();
      spokenUpTo = end;
      if (s.length() >= 4 && cb) cb(s, ctx);
    }
  }
  if (cb && (int)spokenUpTo < (int)out.length()) {     // 流尾残余（无句末符的收尾句）
    String s = out.substring((int)spokenUpTo); s.trim();
    if (s.length() >= 4) cb(s, ctx);
  }
  return sawDone;
}

LlmReply LlmClient::chat(const String& systemPrompt, const String& userPrompt,
                          int maxTokens, float temperature) {
  LlmMsg msgs[2] = { {"system", systemPrompt}, {"user", userPrompt} };
  return chatMsgs(msgs, 2, maxTokens, temperature);
}

LlmReply LlmClient::chatMsgs(const LlmMsg* msgs, int count,
                              int maxTokens, float temperature) {
  int maxCont = cfg.s.llmContinue > 3 ? 3 : cfg.s.llmContinue;
  return chatMsgsContinue(msgs, count, maxTokens, temperature, maxCont);
}

LlmReply LlmClient::chatMsgsStream(const LlmMsg* msgs, int count, int maxTokens,
                                   float temperature, LlmSentenceCb cb, void* ctx,
                                   bool skipFirstLine) {
  int maxCont = cfg.s.llmContinue > 3 ? 3 : cfg.s.llmContinue;
  return chatMsgsContinue(msgs, count, maxTokens, temperature, maxCont, cb, ctx, skipFirstLine);
}

// depth=剩余可续写轮数（0=不再续）
LlmReply LlmClient::chatMsgsContinue(const LlmMsg* msgs, int count,
                                      int maxTokens, float temperature, int depth,
                                      LlmSentenceCb cb, void* ctx, bool skipFirstLine) {
  LlmReply r;
  if (String(cfg.s.llmKey).length() == 0) { lastError = "API Key 未配置"; return r; }
  // 内存碎片：TLS 握手要一块连续内存，只看"总空闲"会被碎片骗（实测 heap 75KB、
  // 最大连续块只有 30 多 KB 时 connect 直接失败，而主线程新连接却能成）。
  // v3.55 起 TLS 收发缓冲走 PSRAM（tlsheap 钩子），握手只需 ~8-10KB 内部块
  // （mbedtls 上下文+握手临时量），门槛从 24KB 同步下调到 14KB；
  // 真正的兜底交给下面 connect 失败后的重试+退避。
  for (int i = 0; i < 8 && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 14000; i++)
    vTaskDelay(pdMS_TO_TICKS(250));
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 14000) {
    lastError = String("内存碎片过多（最大连续块仅 ") +
                String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024) + "KB），稍后重试";
    return r;
  }

  String url = buildUrl();
  int dp = url.indexOf("://"); if (dp < 0) { lastError = "URL 无效"; return r; }
  int hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  bool useTls = url.startsWith("https");
  int port = useTls ? 443 : 80;   // http:// 未写端口时别再连 443（v3.51）
  if (host.indexOf(':') >= 0) { // 自定义端口 http(s)://host:port
    port = host.substring(host.indexOf(':') + 1).toInt();
    host = host.substring(0, host.indexOf(':'));
  }

  String body = String("{\"model\":\"") + cfg.s.llmModel + "\",\"messages\":[";
  for (int i = 0; i < count; i++) {
    if (i) body += ",";
    body += String("{\"role\":\"") + msgs[i].role + "\",\"content\":\"" + jsonEscape(msgs[i].content) + "\"}";
  }
  body += String("],\"max_tokens\":") + maxTokens +
    ",\"temperature\":" + String(temperature, 2) +
    ",\"stream\":" + (cb ? "true" : "false");   // v3.70：流式路径要求服务端 SSE
  // 关掉思考：思考型模型（v4 系）在这些短任务上会把预算全花在推理上、正文返回空
  // （实测 60~1600 token 都可能拿到 content=""），关掉后既快又稳。
  // 不认这个字段的服务商一般忽略它；若报 400 就在设置页关掉这个开关。
  if (cfg.s.llmNoThink) body += ",\"thinking\":{\"type\":\"disabled\"}";
  body += "}";

  // 连接获取：优先复用保活连接（省 1-2s 握手与一次内部堆峰值），没有再新建+退避重试
  bool reused = false;
  WiFiClient* client = connAcquire(s_chatConn, host, port, useTls, reused);
  if (client) {
    client->setTimeout(15000);   // 复用路径必须恢复读超时（探测把它压到了 10ms）
    Serial.println("[LLM] 复用保活连接");
  }
  if (!client) {
    client = llmFreshConnect(useTls, host, port, lastError);
    if (!client) return r;
  }

  // Host 头必须带非默认端口（自建网关 https://gw.lan:8443/v1 是最典型填法，v3.51 审计）
  String hostHdr = (port == 443 || port == 80) ? host : host + ":" + String(port);
  // keep-alive：服务器无视它直接关闭也没关系——读取侧按帧判断，关闭=不复用（v3.55）
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + hostHdr +
    "\r\nAuthorization: Bearer " + cfg.s.llmKey +
    "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
    "\r\nConnection: keep-alive\r\n\r\n" + body;
  size_t sent = client->print(req);
  if (sent != req.length() && reused && sent == 0) {
    // 复用连接已死且一个字节都没发出去：新建连接重发是安全的（服务器什么都没收到）
    connFinish(s_chatConn, client, false, host, port, useTls);
    Serial.println("[LLM] 保活连接已失效，新建重发");
    client = llmFreshConnect(useTls, host, port, lastError);
    if (!client) return r;
    reused = false;
    sent = client->print(req);
  }
  if (sent != req.length()) {
    // 部分发出后写失败：不能盲重试（重发可能重复计费），如实报错
    connFinish(s_chatConn, client, false, host, port, useTls);
    lastError = "发送失败:" + host;
    return r;
  }

  String hdrs, payload, content;
  if (cb) {
    // ---- v3.70 流式路径：SSE 增量 + 句子级回调（首句即成句入播报队列） ----
    bool gotAny = false;
    bool fin = readSseStream(client, 60000, 40000, content, cb, ctx, skipFirstLine, gotAny);
    // 流式帧尾结构因商而异：连接一律不复用（新建握手 1~2s 藏在下轮生成时间里）
    connFinish(s_chatConn, client, false, host, port, useTls);
    if (!gotAny) {
      // 一点正文都没收到（非 200/协议不符/断连）：回退非流式一次（拿真实错误/正常答复）
      Serial.println("[LLM·流] 流式无数据 → 回退非流式");
      lastError = "";
      return chatMsgsContinue(msgs, count, maxTokens, temperature, depth > 0 ? depth - 1 : 0);
    }
    if (!fin) Serial.println("[LLM·流] 流式提前结束（按已收内容继续）");
    content.trim();
    if (!content.length()) { lastError = "流式无正文"; return r; }
    r.httpStatus = 200;
    {   // 原文取证（与整段路径同口径）
      String fin0; fin0 = fin ? "stop" : "cut";
      Serial.printf("[LLM·流] 原文(finish=%s, 流式): %.160s\n", fin0.c_str(), content.c_str());
    }
  } else {
    bool clean = laapHttpRead(client, 30000, hdrs, payload, 40000);
    connFinish(s_chatConn, client, clean, host, port, useTls);
    if (!clean) Serial.println("[LLM] 连接未按帧干净结束，已丢弃");

    int sp = hdrs.indexOf(' ');
    r.httpStatus = (sp > 0) ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;

    if (r.httpStatus != 200) {
      String emsg;
      if (extractStringField(payload, "message", emsg)) lastError = "HTTP " + String(r.httpStatus) + ": " + emsg;
      else {
        // 服务商报错形状五花八门（error.message / error.code / 纯文本），
        // 解析不到 message 就带一段响应体原文——"HTTP 400" 三个字排查不了任何问题
        lastError = "HTTP " + String(r.httpStatus);
        String snip = payload; snip.trim();
        if (snip.length()) lastError += "｜" + snip.substring(0, 160);
      }
      // 400 且开着"关闭思考"：多半是这家服务商不认 thinking 字段 → 直接告诉用户怎么关
      if (r.httpStatus == 400 && cfg.s.llmNoThink)
        lastError += "（若报未知参数，请在设置页关掉「关闭模型思考」）";
      // v3.71 诊断：invalid unicode 时把最后一条消息（本次提示词）整条 hex 打出，本地定位毒码点
      if (r.httpStatus == 400 && payload.indexOf("invalid unicode") >= 0) {
        const String& cc = msgs[count - 1].content;
        Serial.printf("[LLM·400] 最后消息 %uB 全量 hex：\n", (unsigned)cc.length());
        for (unsigned int i = 0; i < cc.length(); i++) {
          Serial.printf("%02X", (unsigned char)cc[i]);
          if (i % 32 == 31) Serial.println();
        }
        Serial.println();
      }
      return r;
    }
    if (!extractStringField(payload, "content", content)) {
      lastError = "响应无 content";
      // 诊断：思考模型（v4 系列默认思考）可能把正文放 reasoning_content，
      // 把原始响应头打出来，便于判断"答非所问/空回复"是模型侧还是解析侧
      Serial.printf("[LLM] 响应无 content，payload 头: %.200s\n", payload.c_str());
      return r;
    }

    {   // 原文取证：判断"答非所问/截断/空回复"的第一现场
      String fin;
      extractStringField(payload, "finish_reason", fin);
      Serial.printf("[LLM] 原文(finish=%s): %.160s\n", fin.c_str(), content.c_str());
    }
  }
  // 空正文按失败处理：思考型模型（v4 系列默认开思考）会把 max_tokens 全花在思考上、
  // content 返回空串。旧代码当成功 → 网页拿到空白气泡、TTS 无话可说（实测 2026-09-26）
  if (content.length() == 0) {
    String fin0, rc;
    extractStringField(payload, "finish_reason", fin0);
    // 取证：思考型模型的推理过程在 reasoning_content 里，长度说明"思考吃掉了多少预算"
    if (extractStringField(payload, "reasoning_content", rc))
      Serial.printf("[LLM] reasoning_content %u 字节（预算 %d）\n", (unsigned)rc.length(), maxTokens);
    else
      Serial.printf("[LLM] payload 头(无 reasoning 字段): %.400s\n", payload.c_str());
    // 只有"被截断"才值得加钱重试：上限翻倍再要一次（真·空回复重试也没用）
    // finish=length 之外 finish=stop 也可能是同一回事：模型思考完就收尾（正文留空），
    // 所以不再挑 finish_reason，空正文一律给一次更大的额度（实测独白成文步就是 stop+空）
    if (depth > 0 && maxTokens < 1600) {
      int bigger = maxTokens * 2; if (bigger > 1600) bigger = 1600;
      Serial.printf("[LLM] content 空（finish=%s）→ 上限 %d→%d 重试一次\n",
                    fin0.c_str(), maxTokens, bigger);
      LlmReply r2 = chatMsgsContinue(msgs, count, bigger, temperature, depth - 1);
      if (r2.ok) return r2;
      // r2 也失败：保留它的真实错误（连接失败/HTTP 错误已写入 lastError）——
      // 旧代码无条件覆盖成"content 为空"，ping() 据此把网络故障误报成"连通正常"（v3.51 审计）
      if (!lastError.length() || lastError.indexOf("content 为空") >= 0)
        lastError = "响应 content 为空（思考模型吃满 max_tokens？把单次回复上限调大试试）";
      return r;
    }
    lastError = "响应 content 为空（思考模型吃满 max_tokens？把单次回复上限调大试试）";
    return r;
  }
  // 期望两行: 表情词\n要说的话（宽松解析）。首行**确实是**情绪词才剥——
  // 夜间清单/梦的多行输出首行是非情绪词正文，曾被无条件吞掉（v3.49 审计修复：
  // 关系抽取每晚必丢第一条、情绪标注丢首条映射、梦丢首句，全部静默）
  content.trim();
  int nl = content.indexOf('\n');
  if (nl > 0) {
    String e = content.substring(0, nl); e.trim(); e.toLowerCase();
    e.replace(" ", ""); e.replace(",", ""); e.replace("。", "");
    if (e == "happy" || e == "curious" || e == "excited" || e == "lonely" ||
        e == "anxious" || e == "tired" || e == "calm") {
      r.expr = e;
      r.say = content.substring(nl + 1);
    }
  }
  if (r.expr.length() == 0) r.expr = "calm";
  if (r.say.length() == 0) r.say = content;
  r.say.trim();

  // 截断续发：finish_reason=length 且还有续写深度 → 带前文再续（messages≤10 硬守卫）
  bool extended = false;
  String fin;
  if (depth > 0 && count + 2 <= 12 &&
      extractStringField(payload, "finish_reason", fin) && fin == "length") {
    Serial.println("[LLM] 输出被截断，自动续写…");
    LlmMsg msgs2[14];
    for (int i = 0; i < count; i++) msgs2[i] = msgs[i];
    msgs2[count] = {"assistant", content};          // 含表情行的完整前文
    msgs2[count + 1] = {"user", "（接着说，从中断处继续，不要重复已说的）"};
    LlmReply r2 = chatMsgsContinue(msgs2, count + 2, maxTokens, temperature, depth - 1);
    if (r2.ok && r2.say.length()) {
      r.say += r2.say;                              // 续段直接拼接（续写不含表情行）
      extended = true;
    }
  }

  // 上限：常规 600B≈200 汉字（原 240B 会切半句话）；有续写则放宽到 1000B——
  // 否则首段已超 600B 时续写内容 100% 被截掉，白花一次 API 调用（v3.51 审计）
  int sayCap = extended ? 1000 : 600;
  if (r.say.length() > sayCap) r.say = utf8Cut(r.say, sayCap);
  r.ok = true;
  lastError = "";
  // 回绕改造后无起始时刻需要
  return r;
}

bool LlmClient::ping(String& reply) {
  LlmReply r = chat("你是测试助手。只回复两个字：正常", "ping", 16, 0.1f);
  if (r.ok) { reply = r.say; return true; }
  // 空正文（思考模型吃满 max_tokens）说明"网络与鉴权都通、只是这次没吐字"——连通性算通过
  if (lastError.indexOf("content 为空") >= 0) {
    reply = "连接与鉴权正常，但模型这次没吐正文（多半是思考型模型把 max_tokens 花在思考上，建议把「单次回复上限」调大）";
    return true;
  }
  reply = lastError;
  return false;
}

// 跨任务网络客户端互斥：搜索/视觉/连通性测试共用一把锁。
// 独立成全局（而不是各模块私有）是因为"独白流水线"会连续用搜索+视觉，
// 而主线程的聊天可能同时用它们；共用一把锁才能把两个任务真正隔开。
static SemaphoreHandle_t s_netMtx = nullptr;
void laapNetInit() {                            // setup 里显式建锁：懒创建的 check-then-create
  if (!s_netMtx) s_netMtx = xSemaphoreCreateMutex();   // 若两任务同时首进会各建一把=互斥失效+泄漏
  if (!s_connMtx) s_connMtx = xSemaphoreCreateMutex(); // keep-alive 槽位元数据锁（v3.55，同上理由）
}
bool laapNetLock(uint32_t ms) {
  if (!s_netMtx) {
    s_netMtx = xSemaphoreCreateMutex();          // 兜底懒创建（正常路径已被 laapNetInit 覆盖）
    if (!s_netMtx) return true;                  // 创建失败：不阻塞功能（退回原行为）
  }
  return xSemaphoreTake(s_netMtx, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void laapNetUnlock() { if (s_netMtx) xSemaphoreGive(s_netMtx); }

// UTF-8 安全截断：len 字节上限处回退到字符边界（不切碎中文）
String utf8Cut(const String& s, int len) {
  if ((int)s.length() <= len) return s;
  int cut = len;
  while (cut > 0 && (s[cut] & 0xC0) == 0x80) cut--;   // 落在续字节上→退到首字节
  return s.substring(0, cut);
}

// UTF-8 兜底清洗：丢掉非法字节（孤立续字节、被截断的多字节序列）
// v3.67 严格化：GB18030/GBK 字节对常伪装成合法 UTF-8 结构（如 0xCF 0xB5=ε），
// 但 0xC0/0xC1 首字节（过长编码）、代理区 ED A0-BF、>U+10FFFF（F5+）是任何解码器
// 都拒收的——大模型 API 会整包 400「invalid unicode code point」，把聊天/表达全线
// 打进本地兜底。结构校验必须 reject 这四类。
// v3.71 再补一刀：**非字符码点**（U+FDD0~FDEF、各平面 U+FFFE/U+FFFF）结构合法但
// 严格 JSON/Unicode 解析器同样拒收——GBK 0xEF 0xBF 0xBE 伪装成 U+FFFE 就是当时
// 修完 v3.67 仍 400 的真凶。
String sanitizeUtf8(const String& s) {
  String out; out.reserve(s.length());
  unsigned int i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    int need;
    bool reject = false;
    if      (c < 0x80)           need = 0;
    else if (c == 0xC0 || c == 0xC1) { reject = true; need = 0; }   // 过长编码首字节
    else if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if (c == 0xF0)          need = 3;
    else if (c == 0xF4)          need = 3;
    else if ((c & 0xF8) == 0xF0) { reject = true; need = 3; }       // F5-FF：>U+10FFFF / 非法
    else { i++; continue; }                       // 孤立续字节 / 非法首字节
    bool ok = !reject && (i + (unsigned)need < s.length());
    unsigned cp = 0;
    for (int k = 1; ok && k <= need; k++)
      if (((uint8_t)s[i + k] & 0xC0) != 0x80) ok = false;
    if (ok) {                                     // 解码码点：非字符/代理区整类拒收
      cp = (unsigned)c & (unsigned)(0x7F >> (need == 0 ? 7 : need));
      for (int k = 1; k <= need; k++) cp = (cp << 6) | ((unsigned)s[i + k] & 0x3F);
      if (cp >= 0xFDD0 && cp <= 0xFDEF) ok = false;                       // 非字符区
      if (cp >= 0xFFFE && (cp & 0xFFFF) >= 0xFFFE) ok = false;            // 各平面 FFFE/FFFF
    }
    if (ok && need == 2 && (uint8_t)s[i] == 0xED && ((uint8_t)s[i+1] & 0xE0) == 0xA0) ok = false;  // 代理区
    if (ok && (uint8_t)s[i] == 0xF0 && ((uint8_t)s[i+1] & 0xE0) == 0x80) ok = false;  // 过长 4 字节
    if (ok && (uint8_t)s[i] == 0xF4 && (uint8_t)s[i+1] >= 0x90) ok = false;           // >U+10FFFF
    if (!ok) {
      // v3.71c 关键修正：跳过**整个候选序列**（含续字节），绝不把 GBK 双字节的第二字节
      // 留给下一轮当"合法序列的开头"重新组队——GBK(小)=CF B8 的 B8 与后随 GBK 首字节
      // 组成 E? 形态假序列，正是"清了又毒"的根源。孤立续字节只需跳 1 字节，其余全跳 need+1。
      i += (c >= 0x80 && c < 0xC2) ? 1 : (need + 1);
      continue;
    }
    for (int k = 0; k <= need; k++) out += s[i + k];
    i += need + 1;
  }
  return out;
}

// ================= 语义向量：硅基流动 bge-m3（记忆智能召回用） =================
static const int EMB_DIM_L = 1024;

// bin：EMB_DIM_L 个 float 的原始字节；ok=false 时内容无意义。失败静默（记忆层自动退关键词通道）。
String laapEmbed(const String& text, bool& ok) {
  ok = false;
  if (WiFi.status() != WL_CONNECTED) return "";
  // 向量通道配置：独立配置（embBase/embKey/embModel）优先，留空则复用 ASR 的 base/key。
  // 这样 ASR 换服务商不会把识海召回悄悄带死（bge-m3 只有硅基流动等家有）。
  String base(cfg.s.embBase[0] ? cfg.s.embBase : cfg.s.asrBase);
  String key(cfg.s.embKey[0] ? cfg.s.embKey : cfg.s.asrKey);
  String model(cfg.s.embModel[0] ? cfg.s.embModel : "BAAI/bge-m3");
  if (!key.length()) return "";
  int dm = base.indexOf("/v1");
  String ep = (dm > 8 ? base.substring(0, dm) : base) + "/v1/embeddings";

  int dp = ep.indexOf("://"); if (dp < 0) return "";
  int hp = ep.indexOf('/', dp + 3);
  String host = (hp < 0) ? ep.substring(dp + 3) : ep.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : ep.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) { port = host.substring(host.indexOf(':') + 1).toInt(); host = host.substring(0, host.indexOf(':')); }

  String body = String("{\"model\":\"") + model + "\",\"input\":[\"" + LlmClient::jsonEscape(text) + "\"]}";
  // 保活复用（v3.55）：向量是最高频的 TLS 调用（每次记忆写入都要），复用收益最直接。
  // 帧不干净/写失败一律丢弃连接——失败静默返回空（记忆层自动退关键词通道，与旧行为一致）
  bool reused = false;
  WiFiClient* client = connAcquire(s_embConn, host, port, true, reused);
  if (client) client->setTimeout(12000);   // 复用路径必须恢复读超时（探测把它压到了 10ms）
  (void)reused;   // embed 高频且静默：复用成功不打日志，这里只接收 out 参数
  if (!client) {
    client = new WiFiClientSecure;
    ((WiFiClientSecure*)client)->setInsecure();
    client->setTimeout(12000);
    if (!client->connect(host.c_str(), port)) { delete client; return ""; }
  }
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
    "\r\nAuthorization: Bearer " + key +
    "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
    "\r\nConnection: keep-alive\r\n\r\n" + body;
  if (client->print(req) != req.length()) {
    connFinish(s_embConn, client, false, host, port, true);
    return "";
  }
  String hdrs, payload;
  bool clean = laapHttpRead(client, 20000, hdrs, payload, 40000);
  connFinish(s_embConn, client, clean, host, port, true);
  int sp = hdrs.indexOf(' ');
  int code = sp > 0 ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (code != 200) return "";

  // 解析 "embedding":[0.123,-0.456,...]（1024 个浮点）
  int epos = payload.indexOf("\"embedding\"");
  if (epos < 0) return "";
  int lb = payload.indexOf('[', epos);
  int rb = payload.indexOf(']', lb);
  if (lb < 0 || rb < 0) return "";
  String bin; bin.reserve(EMB_DIM_L * 4);
  int n = 0, i = lb + 1;
  char buf[32];
  while (i < rb && n < EMB_DIM_L) {
    int k = 0;
    while (i < rb && (isDigit(payload[i]) || payload[i] == '-' || payload[i] == '.' ||
                      payload[i] == 'e' || payload[i] == 'E' || payload[i] == '+')) {
      if (k < 31) buf[k++] = payload[i];
      i++;
    }
    buf[k] = 0;
    if (k > 0) {
      float v = atof(buf);
      const char* pc = (const char*)&v;
      for (int b = 0; b < 4; b++) bin += pc[b];
      n++;
    }
    int before = i;                                   // 死循环保险：本轮游标必须前进
    while (i < rb && (payload[i] == ',' || payload[i] == ' ' || payload[i] == '\r' || payload[i] == '\n')) i++;
    if (i == before && k == 0) i++;                   // 未知字符也前进，绝不原地打转
  }
  if (n != EMB_DIM_L) return "";
  ok = true;
  return bin;
}

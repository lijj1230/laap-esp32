#include "laap_llm.h"
#include "laap_config.h"
#include <esp_heap_caps.h>   // 最大连续块（TLS 握手要一整块，不看总量）
#include <WiFiClientSecure.h>
#include <WiFi.h>

LlmClient llm;

String LlmClient::endpoint() const { return buildUrl(); }

String LlmClient::buildUrl() const {
  String base(cfg.s.llmBase);
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  // 已带 /v1 /v3 /v4 等版本段 → 直接拼 chat/completions
  int slash = base.lastIndexOf('/');
  String last = slash >= 0 ? base.substring(slash + 1) : "";
  bool hasVer = (last.length() >= 2 && last[0] == 'v' && isDigit(last[1]));
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
      default:   o += c;
    }
  }
  return o;
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
            // \uXXXX —— 常见仅为标点/emoji，跳过
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

// depth=剩余可续写轮数（0=不再续）
LlmReply LlmClient::chatMsgsContinue(const LlmMsg* msgs, int count,
                                      int maxTokens, float temperature, int depth) {
  LlmReply r;
  if (String(cfg.s.llmKey).length() == 0) { lastError = "API Key 未配置"; return r; }
  // 内存碎片：TLS 握手要一块连续内存，只看"总空闲"会被碎片骗（实测 heap 75KB、
  // 最大连续块只有 30 多 KB 时 connect 直接失败，而主线程新连接却能成）。
  // 开机常态最大块就 ~39KB，所以门槛只拦"真的没救"的情况（24KB），
  // 真正的兜底交给下面 connect 失败后的重试+退避。
  for (int i = 0; i < 8 && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24000; i++)
    vTaskDelay(pdMS_TO_TICKS(250));
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24000) {
    lastError = String("内存碎片过多（最大连续块仅 ") +
                String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024) + "KB），稍后重试";
    return r;
  }

  String url = buildUrl();
  int dp = url.indexOf("://"); if (dp < 0) { lastError = "URL 无效"; return r; }
  int hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) { // 自定义端口 http(s)://host:port
    port = host.substring(host.indexOf(':') + 1).toInt();
    host = host.substring(0, host.indexOf(':'));
  }
  bool useTls = url.startsWith("https");

  String body = String("{\"model\":\"") + cfg.s.llmModel + "\",\"messages\":[";
  for (int i = 0; i < count; i++) {
    if (i) body += ",";
    body += String("{\"role\":\"") + msgs[i].role + "\",\"content\":\"" + jsonEscape(msgs[i].content) + "\"}";
  }
  body += String("],\"max_tokens\":") + maxTokens +
    ",\"temperature\":" + String(temperature, 2) + ",\"stream\":false";
  // 关掉思考：思考型模型（v4 系）在这些短任务上会把预算全花在推理上、正文返回空
  // （实测 60~1600 token 都可能拿到 content=""），关掉后既快又稳。
  // 不认这个字段的服务商一般忽略它；若报 400 就在设置页关掉这个开关。
  if (cfg.s.llmNoThink) body += ",\"thinking\":{\"type\":\"disabled\"}";
  body += "}";

  // 连接重试：connect 失败不花 token（请求还没发出去），值得带退避重试两次——
  // 碎片多时等几百毫秒回收往往就通了，比让上层收到一个"连接失败"有用得多
  WiFiClient *client = nullptr;
  for (int attempt = 0; attempt < 3; attempt++) {
    client = useTls ? (WiFiClient*)(new WiFiClientSecure) : new WiFiClient;
    if (useTls) ((WiFiClientSecure*)client)->setInsecure(); // 端侧自签策略见 README
    client->setTimeout(15000); // Stream 超时单位为 ms
    if (client->connect(host.c_str(), port)) break;
    delete client; client = nullptr;
    Serial.printf("[LLM] 连接失败（第 %d 次），最大连续块 %uKB，%dms 后重试\n",
                  attempt + 1,
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                  600 + attempt * 700);
    vTaskDelay(pdMS_TO_TICKS(600 + attempt * 700));
  }
  if (!client) { lastError = "连接失败:" + host; return r; }

  uint32_t t0 = millis();
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
    "\r\nAuthorization: Bearer " + cfg.s.llmKey +
    "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
    "\r\nConnection: close\r\n\r\n" + body;
  client->print(req);

  // 读响应
  String resp; resp.reserve(4096);
  uint32_t t0ms = millis();
  while (client->connected() && millis() - t0ms < 30000) {  // 差值比较：49.7 天回绕安全
    while (client->available()) {
      resp += (char)client->read();
      if (resp.length() > 40000) break;
    }
    if (resp.length() > 40000) break;
    delay(2);
  }
  client->stop();
  delete client;

  int sp = resp.indexOf(' ');
  r.httpStatus = (sp > 0) ? resp.substring(sp + 1, sp + 4).toInt() : 0;
  int bodyStart = resp.indexOf("\r\n\r\n");
  String payload = (bodyStart > 0) ? resp.substring(bodyStart + 4) : resp;
  // 处理 chunked 传输：把行间 chunk 头去掉
  if (payload.indexOf("{\"id\"") < 0 && payload.length() > 0) {
    String clean; int pos = 0;
    while (pos < (int)payload.length()) {
      int nl = payload.indexOf('\n', pos);
      String line = (nl < 0) ? payload.substring(pos) : payload.substring(pos, nl);
      line.trim();
      bool allHex = line.length() > 0;
      for (unsigned int ci = 0; ci < line.length() && allHex; ci++) {
        char ch = line[ci];
        if (!isHexadecimalDigit(ch)) allHex = false;
      }
      if (!allHex) clean += line;
      if (nl < 0) break;
      pos = nl + 1;
    }
    payload = clean;
  }

  String content;
  if (r.httpStatus != 200) {
    String emsg;
    if (extractStringField(payload, "message", emsg)) lastError = "HTTP " + String(r.httpStatus) + ": " + emsg;
    else lastError = "HTTP " + String(r.httpStatus);
    // 400 且开着"关闭思考"：多半是这家服务商不认 thinking 字段 → 直接告诉用户怎么关
    if (r.httpStatus == 400 && cfg.s.llmNoThink)
      lastError += "（若报未知参数，请在设置页关掉「关闭模型思考」）";
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
    }
    lastError = "响应 content 为空（思考模型吃满 max_tokens？把单次回复上限调大试试）";
    return r;
  }
  // 期望两行: 表情词\n要说的话（宽松解析）
  content.trim();
  int nl = content.indexOf('\n');
  if (nl > 0) {
    String e = content.substring(0, nl); e.trim(); e.toLowerCase();
    e.replace(" ", ""); e.replace(",", ""); e.replace("。", "");
    if (e == "happy" || e == "curious" || e == "excited" || e == "lonely" ||
        e == "anxious" || e == "tired" || e == "calm") r.expr = e;
    r.say = content.substring(nl + 1);
  }
  if (r.expr.length() == 0) r.expr = "calm";
  if (r.say.length() == 0) r.say = content;
  r.say.trim();

  // 截断续发：finish_reason=length 且还有续写深度 → 带前文再续（messages≤10 硬守卫）
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
    }
  }

  // 上限 600B≈200 汉字：原来 240B 会把正常回答切在半句话上（"台词被腰斩"）
  if (r.say.length() > 600) r.say = utf8Cut(r.say, 600);
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
bool laapNetLock(uint32_t ms) {
  if (!s_netMtx) {
    s_netMtx = xSemaphoreCreateMutex();          // 懒创建（首次调用必在主线程 setup 之后）
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
String sanitizeUtf8(const String& s) {
  String out; out.reserve(s.length());
  unsigned int i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    int need;
    if      (c < 0x80)           need = 0;
    else if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if ((c & 0xF8) == 0xF0) need = 3;
    else { i++; continue; }                       // 孤立续字节 / 非法首字节
    bool ok = (i + (unsigned)need < s.length());
    for (int k = 1; ok && k <= need; k++)
      if (((uint8_t)s[i + k] & 0xC0) != 0x80) ok = false;
    if (!ok) { i++; continue; }                   // 截断序列：丢首字节，续字节下一轮同样被丢
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
  if (String(cfg.s.asrKey).length() == 0 || WiFi.status() != WL_CONNECTED) return "";
  // 硅基流动 embeddings 端点与 ASR 同域同 key
  String base(cfg.s.asrBase);
  int dm = base.indexOf("/v1");
  String ep = (dm > 8 ? base.substring(0, dm) : base) + "/v1/embeddings";

  int dp = ep.indexOf("://"); if (dp < 0) return "";
  int hp = ep.indexOf('/', dp + 3);
  String host = (hp < 0) ? ep.substring(dp + 3) : ep.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : ep.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) { port = host.substring(host.indexOf(':') + 1).toInt(); host = host.substring(0, host.indexOf(':')); }

  String body = String("{\"model\":\"BAAI/bge-m3\",\"input\":[\"") + LlmClient::jsonEscape(text) + "\"]}";
  WiFiClientSecure cli; cli.setInsecure(); cli.setTimeout(12000);
  if (!cli.connect(host.c_str(), port)) return "";
  String req = String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
    "\r\nAuthorization: Bearer " + cfg.s.asrKey +
    "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
    "\r\nConnection: close\r\n\r\n" + body;
  cli.print(req);
  String resp; resp.reserve(12288);
  uint32_t dl = millis() + 20000;
  while (cli.connected() && millis() < dl) {
    while (cli.available()) { resp += (char)cli.read(); if (resp.length() > 60000) break; }
    if (resp.length() > 60000) break;
    delay(2);
  }
  cli.stop();
  int sp = resp.indexOf(' ');
  int code = sp > 0 ? resp.substring(sp + 1, sp + 4).toInt() : 0;
  int bs = resp.indexOf("\r\n\r\n");
  String payload = bs > 0 ? resp.substring(bs + 4) : "";
  if (code != 200) return "";

  // chunked 块头去掉（与 chat 路径同法：纯十六进制行删）。原实现漏了这步，
  // 块大小行的 hex 字母会让下面的解析游标卡死 → llmTask 死循环 → 看门狗复位
  if (payload.indexOf('{') < 0 || payload.indexOf('\n') >= 0) {
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

#include "laap_llm.h"
#include "laap_config.h"
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
    ",\"temperature\":" + String(temperature, 2) + ",\"stream\":false}";

  WiFiClient *client = useTls ? (WiFiClient*)(new WiFiClientSecure) : new WiFiClient;
  if (useTls) ((WiFiClientSecure*)client)->setInsecure(); // 端侧自签策略见 README
  client->setTimeout(15000); // Stream 超时单位为 ms

  uint32_t t0 = millis();
  if (!client->connect(host.c_str(), port)) {
    lastError = "连接失败:" + host; delete client; return r;
  }
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
    return r;
  }
  if (!extractStringField(payload, "content", content)) {
    lastError = "响应无 content";
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

  if (r.say.length() > 240) r.say = utf8Cut(r.say, 240);      // 续写拼接后放宽上限（回退到 UTF-8 边界）
  r.ok = true;
  lastError = "";
  // 回绕改造后无起始时刻需要
  return r;
}

bool LlmClient::ping(String& reply) {
  LlmReply r = chat("你是测试助手。只回复两个字：正常", "ping", 16, 0.1f);
  reply = r.ok ? r.say : lastError;
  return r.ok;
}

// UTF-8 安全截断：len 字节上限处回退到字符边界（不切碎中文）
String utf8Cut(const String& s, int len) {
  if ((int)s.length() <= len) return s;
  int cut = len;
  while (cut > 0 && (s[cut] & 0xC0) == 0x80) cut--;   // 落在续字节上→退到首字节
  return s.substring(0, cut);
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

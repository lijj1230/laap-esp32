#include "laap_metrics.h"
#include "laap_llm.h"   // utf8Cut（按字符边界截，substring 会把汉字拦腰切断→请求体非法 UTF-8）
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>

LaapMetrics metrics;

// 就地清洗非法 UTF-8（历史文件里的半截汉字/坏字节）：合法序列原样保留，坏字节丢弃。
// DeepSeek 对请求体做严格 UTF-8 校验，一个坏字节 = HTTP 400 invalid unicode code point。
static String cleanUtf8(const String& s) {
  String o; o.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); ) {
    unsigned char c = (unsigned char)s[i];
    int len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    else if (c >= 0x80) { i++; continue; }            // 孤立续字节：丢
    if (i + len > s.length()) { i++; continue; }      // 尾部截断：丢
    bool ok = true;
    for (int k = 1; k < len; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
    if (!ok) { i++; continue; }
    for (int k = 0; k < len; k++) o += s[i + k];
    i += len;
  }
  return o;
}

// JSON 字符串转义（问答原文里什么都有：引号/换行/表情）
static void escTo(String& o, const String& s) {
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "";
    else if (c == '\t') o += ' ';
    else o += c;
  }
}

// 单条反馈一行 JSONL。文件封顶 ~24KB，超了就重写保留最近 80 条：
// 反馈是给"夜间反思当素材 / 人工复盘"用的，留最近的比留全部更有用。
bool LaapMetrics::feedback(int v, const String& user, const String& reply) {
  if (v > 0) fbUp++; else fbDown++;
  File f = LittleFS.open("/mem/feedback.jsonl", "a");
  if (!f) { Serial.println("[METRICS] 反馈落盘失败（feedback.jsonl 打不开）"); return false; }
  time_t now = time(nullptr);
  String line = String("{\"t\":") + (now > 1600000000 ? String((long)now) : String("-1")) +
                ",\"v\":" + v +
                ",\"u\":\"";  escTo(line, cleanUtf8(utf8Cut(user, 120)));
  line += "\",\"a\":\""; escTo(line, cleanUtf8(utf8Cut(reply, 240)));
  line += "\"}\n";
  bool ok = f.print(line) == line.length();
  f.close();
  if (ok && LittleFS.exists("/mem/feedback.jsonl")) {
    File s = LittleFS.open("/mem/feedback.jsonl", "r");
    size_t sz = s ? s.size() : 0;
    if (s) s.close();
    if (sz > 24 * 1024) {
      // 简单封顶：整读 → 只留最后 80 行 → 重写。反馈量级很小（几 KB），不值得更巧
      File r = LittleFS.open("/mem/feedback.jsonl", "r");
      String keep; int lines = 0;
      while (r && r.available()) {
        String ln = r.readStringUntil('\n');
        if (ln.length()) { keep += ln + "\n"; if (++lines > 80) { keep = keep.substring(keep.indexOf('\n') + 1); lines--; } }
      }
      if (r) r.close();
      // 原子写：反馈历史也是日志资产，别在 open("w") 截断后掉电丢光
      File w = LittleFS.open("/mem/feedback.jsonl.tmp", "w");
      if (w) { w.print(keep); w.close();
        LittleFS.remove("/mem/feedback.jsonl");
        LittleFS.rename("/mem/feedback.jsonl.tmp", "/mem/feedback.jsonl");
      }
      Serial.printf("[METRICS] 反馈文件封顶重写：留 %d 条\n", lines);
    }
  }
  Serial.printf("[METRICS] 反馈 %s 已记录（👍%lu 👎%lu）\n",
                v > 0 ? "👍" : "👎", (unsigned long)fbUp, (unsigned long)fbDown);
  return ok;
}

String LaapMetrics::json() const {
  String j = String("\"vad_triggers\":") + vadTriggers +
    ",\"no_speech\":" + noSpeech +
    ",\"asr_try\":" + asrTry +
    ",\"asr_fail\":" + asrFail +
    ",\"wake_miss\":" + wakeMiss +
    ",\"cooldown_skip\":" + cooldownSkip +
    ",\"interrupts\":" + interrupts +
    ",\"tool_direct\":" + toolDirect +
    ",\"vision_ok\":" + visionOk + ",\"vision_fail\":" + visionFail +
    ",\"search_ok\":" + searchOk + ",\"search_fail\":" + searchFail +
    ",\"llm_ok\":" + llmOk + ",\"llm_fail\":" + llmFail +
    ",\"fb_up\":" + fbUp + ",\"fb_down\":" + fbDown +
    ",\"rsp_llm_ms\":" + String(rspLlmMs(), 0) +
    ",\"rsp_dir_ms\":" + String(rspDirMs(), 0);
  return j;
}

// ---- 失败记录环 ----
void LaapMetrics::failNote(const String& s) {
  if (!s.length()) return;
  String note = s;
  note.trim();
  if (note.length() > 90) note = utf8Cut(note, 90);   // 字符边界截断（substring 半截汉字→归纳请求体 400）
  if (_failCnt && _failRing[(_failIdx + 5) % 6] == note) return;   // 连败同错误只记一条
  _failRing[_failIdx] = note;
  _failIdx = (_failIdx + 1) % 6;
  if (_failCnt < 6) _failCnt++;
}

String LaapMetrics::failDigest() const {
  if (!_failCnt) return "";
  String out;
  for (uint8_t i = 0; i < _failCnt; i++) {
    const String& s = _failRing[(_failIdx + 6 - _failCnt + i) % 6];
    if (i) out += "\n";
    out += String(i + 1) + ". " + s;
  }
  return out;
}

// 反馈文件末尾 N 行：文件可能到 24KB，逐行流式读、只留最近 N 行（不整读进堆）
String LaapMetrics::feedbackDigest(int maxLines) {
  File f = LittleFS.open("/mem/feedback.jsonl", "r");
  if (!f) return "";
  String ring[8];
  if (maxLines > 8) maxLines = 8;
  int idx = 0, cnt = 0;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (!ln.length()) continue;
    ring[idx] = cleanUtf8(ln);   // 老文件里可能有半截汉字：读取时就地清洗（否则请求体 400）
    idx = (idx + 1) % maxLines;
    if (cnt < maxLines) cnt++;
  }
  f.close();
  if (!cnt) return "";
  String out;
  for (int i = 0; i < cnt; i++) {
    const String& ln = ring[(idx + maxLines - cnt + i) % maxLines];
    if (i) out += "\n";
    out += ln;
  }
  return out;
}

// ---- 上一段会话快照（NVS blob：计数器 + 失败环）----
// 两档写入（磨损账：~700B/次 × 288 次/天，与 uptime 同量级，可忽略）：
//   ① 5 分钟批量（与累计运行时长同节拍）；② 失败计数 +1 时限速 60s 即时写——
// 崩溃/自愈重启前的失败原因几乎必留底，事后排查的命根子。
void LaapMetrics::persist() {
  _lastPersistMs = millis();
  static const size_t CAP = 4 + 20 * 4 + 6 * (1 + 96);
  uint8_t buf[CAP];
  size_t n = 0;
  memcpy(buf, "LMS1", 4); n = 4;
  const uint32_t c[20] = { vadTriggers, noSpeech, asrTry, asrFail, wakeMiss, cooldownSkip,
    interrupts, toolDirect, visionOk, visionFail, searchOk, searchFail,
    llmOk, llmFail, fbUp, fbDown, rspLlmMsSum, rspLlmN, rspDirMsSum, rspDirN };
  memcpy(buf + n, c, sizeof(c)); n += sizeof(c);
  for (int i = 0; i < 6; i++) {                       // 失败环按"最老→最新"存
    uint8_t len = 0;
    const char* s = nullptr;
    if (_failCnt) {
      const String& r = _failRing[(_failIdx + 6 - _failCnt + i) % 6];
      len = (uint8_t)min(r.length(), (unsigned int)96);
      s = r.c_str();
    }
    buf[n++] = len;
    if (len) { memcpy(buf + n, s, len); n += len; }
  }
  Preferences p;
  if (!p.begin("laapmtr", false)) return;
  p.putBytes("snap", buf, n);
  p.end();
}

void LaapMetrics::loadPrev() {
  Preferences p;
  if (!p.begin("laapmtr", true)) return;
  size_t n = p.getBytesLength("snap");
  static const size_t CAP = 4 + 20 * 4 + 6 * (1 + 96);
  uint8_t buf[CAP];
  if (n < 8 + 20 * 4 || n > sizeof(buf) || p.getBytes("snap", buf, sizeof(buf)) != n) { p.end(); return; }
  if (memcmp(buf, "LMS1", 4) != 0) { p.end(); return; }
  size_t o = 4;
  uint32_t c[20];
  memcpy(c, buf + o, sizeof(c)); o += sizeof(c);
  LaapMetrics* m = new LaapMetrics();                 // 上一段会话（含失败环）整体还原
  m->vadTriggers = c[0];  m->noSpeech = c[1];  m->asrTry = c[2];  m->asrFail = c[3];
  m->wakeMiss = c[4];     m->cooldownSkip = c[5];
  m->interrupts = c[6];   m->toolDirect = c[7];
  m->visionOk = c[8];     m->visionFail = c[9];
  m->searchOk = c[10];    m->searchFail = c[11];
  m->llmOk = c[12];       m->llmFail = c[13];
  m->fbUp = c[14];        m->fbDown = c[15];
  m->rspLlmMsSum = c[16]; m->rspLlmN = c[17];
  m->rspDirMsSum = c[18]; m->rspDirN = c[19];
  for (int i = 0; i < 6 && o < n; i++) {
    uint8_t len = buf[o++];
    if (!len) continue;
    if (o + len > n) break;
    String s;
    s.concat((const char*)(buf + o), len);
    o += len;
    m->_failRing[i] = cleanUtf8(s);
    m->_failIdx = (i + 1) % 6;
    m->_failCnt = i + 1;
  }
  delete _prev;
  _prev = m;
}

void LaapMetrics::failSticky() {
  if (_lastPersistMs && millis() - _lastPersistMs < 60000) return;   // 失败写限速 60s
  persist();
}

// ---- 重启原因（v3.41）：主动重启前打标，开机读回 ----
void laapReboot(const char* why) {
  Preferences p;
  if (p.begin("laapmtr", false)) { p.putString("why", String(why)); p.end(); }
  Serial.printf("[LAAP] 主动重启 → %s\n", why);
  ESP.restart();
}

String laapBootReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "上电启动";
    case ESP_RST_SW: {
      Preferences p;
      String why = p.begin("laapmtr", true) ? p.getString("why", "") : String();
      p.end();
      return why.length() ? String("软件重启·") + why : String("软件重启");
    }
    case ESP_RST_PANIC:    return "程序崩溃(panic)";
    case ESP_RST_INT_WDT:  return "中断看门狗";
    case ESP_RST_TASK_WDT: return "任务看门狗";
    case ESP_RST_WDT:      return "硬件看门狗";
    case ESP_RST_BROWNOUT: return "电压跌落（供电不稳）";
    case ESP_RST_DEEPSLEEP:return "深睡唤醒";
    default:               return "复位码" + String((int)esp_reset_reason());
  }
}

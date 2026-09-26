#include "laap_metrics.h"
#include "laap_llm.h"   // utf8Cut（按字符边界截，substring 会把汉字拦腰切断→请求体非法 UTF-8）
#include <LittleFS.h>
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
      File w = LittleFS.open("/mem/feedback.jsonl", "w");
      if (w) { w.print(keep); w.close(); }
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

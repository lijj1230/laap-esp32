#include "laap_metrics.h"
#include <LittleFS.h>
#include <time.h>

LaapMetrics metrics;

// JSON 字符串转义（问答原文里什么都有：引号/换行/表情）
static void escTo(String& o, const String& s) {
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "";
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
                ",\"u\":\"";  escTo(line, user.substring(0, 120));
  line += "\",\"a\":\""; escTo(line, reply.substring(0, 240));
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

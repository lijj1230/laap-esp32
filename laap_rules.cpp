#include "laap_rules.h"
#include "laap_snap.h"
#include "laap_llm.h"   // utf8Cut
#include <LittleFS.h>

LaapRules rules;

static const char* RULES_PATH = "/mem/rules.txt";
static const int RULES_MAX = 6;
static const size_t RULE_BYTES = 90;   // ≈30 个汉字（UTF-8 3B/字），超了截断

void LaapRules::ensureLoaded() {
  if (_loaded) return;
  _loaded = true;
  File f = LittleFS.open(RULES_PATH, "r");
  if (!f) return;
  _cache = f.readString();
  f.close();
  _cache.trim();
}

String LaapRules::text() {
  ensureLoaded();
  return _cache;
}

int LaapRules::count() {
  ensureLoaded();
  if (!_cache.length()) return 0;
  int n = 1;
  for (unsigned int i = 0; i < _cache.length(); i++) if (_cache[i] == '\n') n++;
  return n;
}

String LaapRules::promptLine() {
  ensureLoaded();
  if (!_cache.length()) return "";
  String s = _cache;
  s.replace("\n", "\n- ");   // 每行都要带列表符
  return String("[行为规则（自我进化沉淀，必须遵守）]\n- ") + s;
}

bool LaapRules::apply(const String& llmOutput) {
  // 行式解析：去掉常见前缀（- • * 1. ①等），只留像规则的行
  String picked[RULES_MAX];
  int n = 0;
  int start = 0;
  while (start < (int)llmOutput.length() && n < RULES_MAX) {
    int nl = llmOutput.indexOf('\n', start);
    String ln = (nl < 0) ? llmOutput.substring(start) : llmOutput.substring(start, nl);
    start = (nl < 0) ? llmOutput.length() : nl + 1;
    ln.trim();
    if (!ln.length()) continue;
    // 剥前缀：- / • / * / 1. / 1、 / ①
    if (ln[0] == '-' || ln[0] == '*') { ln = ln.substring(1); ln.trim(); }
    else if (ln.startsWith("•")) { ln = ln.substring(strlen("•")); ln.trim(); }   // '•' 是多字节字面量：ln[0]=='•' 恒假（v3.51 审计），改字符串比较
    else if (ln[0] >= '0' && ln[0] <= '9') {
      // 数字必须紧跟 ". / 、 )" 才算序号：否则"12点睡觉"会被啃成"点睡觉"
      int d = 0;
      while (d < (int)ln.length() && ln[d] >= '0' && ln[d] <= '9') d++;
      if (d > 0 && d < (int)ln.length() && d <= 3 &&
          (ln[d] == '.' || ln[d] == '、' || ln[d] == ')')) {
        ln = ln.substring(d + 1); ln.trim();
      }
    }
    else if ((unsigned char)ln[0] == 0xE2 && ln.length() > 3) { ln = ln.substring(3); ln.trim(); } // ①等 U+2000-2FFF 起 3B
    // 过滤：<8B 不成话、>160B 丢弃（v3.51 审计：注释曾写"截到上限"，实际是丢行——以代码为准）
    if (ln.length() < 8 || ln.length() > 160) continue;
    if (ln.startsWith("规则") || ln.startsWith("输出") || ln.startsWith("以下") || ln.startsWith("好的")) continue;
    picked[n++] = utf8Cut(ln, RULE_BYTES);
  }
  if (!n) return false;                       // 无产出：保留现有规则
  String merged;
  for (int i = 0; i < n; i++) { if (i) merged += '\n'; merged += picked[i]; }
  ensureLoaded();
  if (merged == _cache) return false;         // 无变化：不写盘（省磨损）
  laapSnapMake("rules", false);               // 覆盖前拍快照（自动档 12h 节流）
  // 原子写：掉电落在 open("w") 截断之后 = 行为规则全丢（v3.31 不变量补齐）
  String tmpPath = String(RULES_PATH) + ".tmp";
  File f = LittleFS.open(tmpPath, "w");
  if (!f) { Serial.println("[RULES] 规则落盘失败"); return false; }
  f.print(merged);
  f.close();
  LittleFS.remove(RULES_PATH);
  LittleFS.rename(tmpPath, RULES_PATH);
  _cache = merged;
  return true;
}

void LaapRules::clear() {
  laapSnapMake("rules", false);   // 清空前拍快照（v3.51：与 apply 同规格，/rules clear 也是破坏性操作）
  LittleFS.remove(RULES_PATH);
  _cache = "";
  _loaded = true;
}

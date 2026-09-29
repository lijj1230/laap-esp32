#include "laap_skills.h"
#include "laap_snap.h"
#include "laap_llm.h"   // utf8Cut
#include <LittleFS.h>

LaapSkills skills;

static const char* SKILLS_PATH = "/mem/skills.txt";
static const int SKILLS_MAX = 12;

void LaapSkills::ensureLoaded() {
  if (_loaded) return;
  _loaded = true;
  File f = LittleFS.open(SKILLS_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int b1 = ln.indexOf('|');
    if (b1 <= 0) continue;
    int b2 = ln.indexOf('|', b1 + 1);
    String trig = ln.substring(0, b1);
    String instr = (b2 > 0) ? ln.substring(b1 + 1, b2) : ln.substring(b1 + 1);
    uint16_t hits = (b2 > 0) ? (uint16_t)ln.substring(b2 + 1).toInt() : 0;
    if (!trig.length() || !instr.length()) continue;
    if (_n < SKILLS_MAX) {
      _s[_n].trig = trig; _s[_n].instr = instr; _s[_n].hits = hits; _n++;
    } else {
      // 满了：与"淘汰最冷"同口径——顶掉当前 hits 最低者，而不是静默丢后面的行
      // （v3.51：原来读满 12 条即停，文件里新写的技能重启后消失）
      int coldest = 0;
      for (int k = 1; k < _n; k++) if (_s[k].hits < _s[coldest].hits) coldest = k;
      if (hits > _s[coldest].hits) { _s[coldest].trig = trig; _s[coldest].instr = instr; _s[coldest].hits = hits; }
    }
  }
  f.close();
}

void LaapSkills::save() {
  // 原子写：掉电落在 open("w") 截断之后 = 技能库全丢（v3.31 不变量补齐）
  String tmpPath = String(SKILLS_PATH) + ".tmp";
  File f = LittleFS.open(tmpPath, "w");
  if (!f) { Serial.println("[SKILLS] 技能落盘失败"); return; }
  for (int i = 0; i < _n; i++)
    f.printf("%s|%s|%u\n", _s[i].trig.c_str(), _s[i].instr.c_str(), _s[i].hits);
  f.close();
  LittleFS.remove(SKILLS_PATH);
  LittleFS.rename(tmpPath, SKILLS_PATH);
}

int LaapSkills::find(const String& trigger) const {
  for (int i = 0; i < _n; i++) if (_s[i].trig == trigger) return i;
  return -1;
}

bool LaapSkills::teach(const String& trigger, const String& instruction) {
  ensureLoaded();
  String trig = utf8Cut(trigger, 30);        // ≤10 字
  String instr = utf8Cut(instruction, 90);   // ≤30 字
  // 存储格式是 trigger|instruction|hits：'|' 与换行会破坏行结构（重载解析错位/丢条目）
  trig.replace("|", " "); trig.replace("\n", " ");
  instr.replace("|", " "); instr.replace("\n", " ");
  trig.trim(); instr.trim();
  if (trig.length() < 6 || instr.length() < 8) return false;   // 太短不成技能
  int i = find(trig);
  if (i >= 0) {                              // 同触发词 = 重教 → 覆盖指令
    _s[i].instr = instr;
  } else {
    if (_n >= SKILLS_MAX) {                  // 满：淘汰 hits 最低的（0 优先）
      int coldest = 0;
      for (int k = 1; k < _n; k++) if (_s[k].hits < _s[coldest].hits) coldest = k;
      _s[coldest] = _s[_n - 1]; _n--;
    }
    _s[_n].trig = trig; _s[_n].instr = instr; _s[_n].hits = 0;
    _n++;
  }
  laapSnapMake("skills", false);             // 覆盖/新增前拍快照（自动档节流）
  save();
  return true;
}

void LaapSkills::hit(const String& userText) {
  ensureLoaded();
  bool changed = false;
  for (int i = 0; i < _n; i++)
    if (_s[i].trig.length() && userText.indexOf(_s[i].trig) >= 0) {
      if (_s[i].hits < 60000) _s[i].hits++;   // 饱和加法：uint16 回绕会把最热技能变成"最冷"被淘汰（v3.51 审计）
      changed = true;
    }
  if (changed) save();
}

void LaapSkills::clear() {
  laapSnapMake("skills", false);   // 清空前拍快照（v3.51：与 teach 同规格）
  _n = 0;
  LittleFS.remove(SKILLS_PATH);
}

String LaapSkills::text() {
  ensureLoaded();
  if (!_n) return "";
  String out;
  for (int i = 0; i < _n; i++) {
    out += String("说「") + _s[i].trig + "」→ " + _s[i].instr + "（命中" + _s[i].hits + "次）";
    if (i < _n - 1) out += "\n";
  }
  return out;
}

String LaapSkills::promptLine() {
  ensureLoaded();
  if (!_n) return "";
  String out = "[口令技能（主人亲手教你的，主人的话命中触发词就必须照做）]";
  for (int i = 0; i < _n; i++)
    out += String("\n- 说的话里带「") + _s[i].trig + "」→ " + _s[i].instr;
  return out;
}

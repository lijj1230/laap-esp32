#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 口令技能库（RSI 第 4 条：Voyager 式，用户亲手教的）
//   对它说「以后每当我说 X 你就 Y」→ 后台 LLM 提取 触发词|指令
//   → 存 /mem/skills.txt（一行一条：trigger|instruction|hits）。
//   生效：buildSystemPrompt 注入整张技能表，由 LLM 模糊匹配触发词
//   （比端侧 indexOf 硬匹配宽容——"来一个"和"来一个笑话"都能中）。
//   hits 只做热度记录（满了淘汰最冷的、网页展示排序用）。
//   截断全部走 utf8Cut（substring 半截汉字 = 请求体 400，教训×2）。
// ============================================================
class LaapSkills {
public:
  String promptLine();                 // 注入段（空表返回空串）
  String text();                       // 人读文本（串口 /skills、网页）
  // 教学落库：触发词 2~10 字、指令 ≤30 字；同触发词=替换；满 12 条淘汰 hits 最低
  bool teach(const String& trigger, const String& instruction);
  void hit(const String& userText);    // userText 含某触发词 → 该技能 hits+1（落盘）
  void clear();

private:
  struct Skill {
    String trig;     // 触发词
    String instr;    // 指令
    uint16_t hits = 0;
  };
  Skill _s[12];
  int _n = 0;
  bool _loaded = false;
  void ensureLoaded();
  void save();
  int find(const String& trigger) const;
};

extern LaapSkills skills;

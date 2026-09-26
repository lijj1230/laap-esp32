#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 行为规则集（RSI 第 2 条：规则自进化的载体）
//   /mem/rules.txt 一行一条规则（LLM 夜间用"反馈+失败记录"整理产出，
//   纯行式文本——设备端手写解析只认行，比让模型吐 JSON 稳得多）。
//   注入：buildSystemPrompt 把规则段追加给每次请求（有缓存，落盘后 reload）。
//   约束：最多 6 条、单条 ≤90 字节（≈30 汉字）；无产出/无变化不写盘（省磨损）。
//   快照：保存前走 laapSnapMake("rules")，进化跑偏可回滚。
// ============================================================
class LaapRules {
public:
  // 注入用的一整段（含标题；没有规则时返回空串）。懒加载，落盘后自动刷新缓存
  String promptLine();
  String text();                       // 原始规则文本（串口 /rules、提示词素材）
  int count();                         // 现有条数
  // 把 LLM 产出的规则文本解析落盘（筛选行/去前缀/限量）。返回 true=确实更新了
  bool apply(const String& llmOutput);
  void clear();                        // /rules clear：全部删除

private:
  String _cache;                       // 磁盘内容缓存（'\n' 分隔，无尾换行）
  bool _loaded = false;
  void ensureLoaded();
};

extern LaapRules rules;

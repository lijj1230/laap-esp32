#pragma once
#include <Arduino.h>

// ============================================================
//  本地意图工具表（小智 MCP 物模型思想的 Arduino 简化版）
//  意图=结构化声明（触发词/动作语/参数类型/handler），表驱动匹配。
//  与 LLM tools 的区别：全部端侧执行零调用零延迟；新增意图只需
//  在 kTools[] 加一行 + 写一个 handler，不再写独立正则函数。
// ============================================================

// 参数解析结果（从语句中抠数字；无数字时用步进推算）
struct ToolMatch {
  bool    hit = false;       // 语句命中本意图
  bool    hasValue = false;  // 语句带具体数值（音量50/亮度调到30）
  int     value = 0;         // 数值本身
  bool    up = false;        // 方向词：大/亮/高 → true，小/暗/低 → false
  bool    fine = false;      // "一点/稍微" → 步进减半
  bool    extreme = false;   // "最大/最亮" 或 "静音/最暗" → 取端点
  int     extremeVal = 0;    // 端点值
};

// 一个可被语音声控的设备能力
struct VoiceTool {
  const char* name;          // 工具名（日志/调试）
  const char* triggers[8];   // 任一命中即候选（如"音量","声音","静音"）
  const char* actions[8];    // 动作语（数字/调/太/点/些/最大...），需命中一个才执行
  int         minV, maxV;    // 参数范围
  int         step;          // 方向词步进
  // 执行：返回给主人听的确认语（<40字）；value 已钳制到 [minV,maxV]
  String (*apply)(int value, int prev, const ToolMatch& m);
};

// 语句 → ToolMatch（命中某工具返回 true）
bool toolMatchSentence(const VoiceTool& t, const String& text, ToolMatch& m);

// 注册表（laap_tools.cpp 定义），loop 前缀入口
void laapToolsInit();
String laapToolsDispatch(const String& text);   // 命中执行返回确认语，否则空串

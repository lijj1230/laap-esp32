#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 评估埋点（RSI 闭环的"评估端"）
//   自我进化 = 生成→评估→迭代，瓶颈在评估器：先让"它做得好不好"可测量。
//   计数器 RAM 常驻（重启清零——调参信号本来就按"本次开机"看，写入成本≈0）；
//   网页 👍/👎 反馈是唯一落盘的（/mem/feedback.jsonl，长期留证据，
//   以后夜间反思可以直接把"被踩的回复"当反思素材）。
//   查看：/api/metrics、/api/status 的 metrics 字段、串口 /metrics
// ============================================================
struct LaapMetrics {
  uint32_t vadTriggers = 0;   // 真正发起的对话轮（过冷却门后）
  uint32_t noSpeech = 0;      // 触发了但没听到话（VAD 误触发/太快放手）
  uint32_t asrTry = 0;        // 真正送到 ASR 的录音
  uint32_t asrFail = 0;       // ASR 返回空（服务问题或没录全）
  uint32_t wakeMiss = 0;      // 说了话但不含唤醒词（被门拦下）
  uint32_t cooldownSkip = 0;  // 播报冷却期丢弃的触发（防自听见）
  uint32_t interrupts = 0;    // 播放被打断（barge-in 次数）
  uint32_t toolDirect = 0;    // 本地工具直答（没走 LLM）
  uint32_t visionOk = 0, visionFail = 0;
  uint32_t searchOk = 0, searchFail = 0;
  uint32_t llmOk = 0, llmFail = 0;            // 后台 LLM 任务成败（含独白/反思）
  uint32_t fbUp = 0, fbDown = 0;              // 网页 👍/👎
  uint32_t rspLlmMsSum = 0, rspLlmN = 0;      // 提问→聊天成品 端到端毫秒
  uint32_t rspDirMsSum = 0, rspDirN = 0;      // 工具/视觉直答耗时（对照用）

  void vadTrigger()            { vadTriggers++; }
  void noSpeechHeard()         { noSpeech++; }
  void asr(bool ok)            { asrTry++; if (!ok) asrFail++; }
  void wakeReject()            { wakeMiss++; }
  void cooldownDrop()          { cooldownSkip++; }
  void interruptedPlay()       { interrupts++; }
  void tool()                  { toolDirect++; }
  void vision(bool ok)         { ok ? visionOk++ : visionFail++; }
  void search(bool ok)         { ok ? searchOk++ : searchFail++; }
  void llm(bool ok)            { ok ? llmOk++ : llmFail++; }
  void rspLlm(uint32_t ms)     { rspLlmMsSum += ms; rspLlmN++; }
  void rspDir(uint32_t ms)     { rspDirMsSum += ms; rspDirN++; }
  float rspLlmMs() const       { return rspLlmN ? (float)rspLlmMsSum / rspLlmN : 0; }
  float rspDirMs() const       { return rspDirN ? (float)rspDirMsSum / rspDirN : 0; }

  // 👍/👎 反馈：追加 /mem/feedback.jsonl（含当轮问答原文）。v: +1 / -1
  bool feedback(int v, const String& user, const String& reply);
  // "asr_try":N,... 形式（不带花括号），/api/metrics 与 /api/status 共用
  String json() const;
};

extern LaapMetrics metrics;

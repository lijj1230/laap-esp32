#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 评估埋点（RSI 闭环的"评估端"）
//   自我进化 = 生成→评估→迭代，瓶颈在评估器：先让"它做得好不好"可测量。
//   计数器 RAM 常驻 + 双份可见：本次醒来（实时）+ 上一段会话（NVS 快照）。
//   落盘策略两档，保 Flash 寿命：
//     ① 5 分钟批量快照（与累计运行时长同一节拍，~700B/次，磨损可忽略）；
//     ② 失败计数器（llm/asr/vision/search）增加时限速 60s 即时写——
//        事故（崩溃/自愈重启）前的失败原因几乎必留底，事后排查的命根子。
//   网页 👍/👎 反馈是另一路持久化（/mem/feedback.jsonl，长期留证据）。
//   查看：/api/metrics（含 prev 块）、/api/status 的 metrics 字段、串口 /metrics
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
  uint32_t webChats = 0;                      // 网页对话轮（handleChat 受理即计；旧 UI 的"对话N轮"只算语音VAD，网页聊天永不进账——v3.64 修复）
  uint32_t fbUp = 0, fbDown = 0;              // 网页 👍/👎（终身累计，NVS 持久）
  float fbUp24 = 0, fbDown24 = 0;             // 滑窗反馈（12h 半衰，夜反思守卫下探用，v3.61）
  void decayFeedback24();                     // 5 分钟拍调用一次
  uint32_t rspLlmMsSum = 0, rspLlmN = 0;      // 提问→聊天成品 端到端毫秒
  uint32_t rspDirMsSum = 0, rspDirN = 0;      // 工具/视觉直答耗时（对照用）

  void vadTrigger()            { vadTriggers++; }
  void noSpeechHeard()         { noSpeech++; }
  void asr(bool ok)            { asrTry++; if (!ok) { asrFail++; failSticky(); } }
  void wakeReject()            { wakeMiss++; }
  void cooldownDrop()          { cooldownSkip++; }
  void interruptedPlay()       { interrupts++; }
  void tool()                  { toolDirect++; }
  void vision(bool ok)         { if (ok) visionOk++; else { visionFail++; failSticky(); } }
  void search(bool ok)         { if (ok) searchOk++; else { searchFail++; failSticky(); } }
  void llm(bool ok)            { if (ok) llmOk++; else { llmFail++; failSticky(); } }
  void rspLlm(uint32_t ms)     { rspLlmMsSum += ms; rspLlmN++; }
  void rspDir(uint32_t ms)     { rspDirMsSum += ms; rspDirN++; }
  float rspLlmMs() const       { return rspLlmN ? (float)rspLlmMsSum / rspLlmN : 0; }
  float rspDirMs() const       { return rspDirN ? (float)rspDirMsSum / rspDirN : 0; }

  // 👍/👎 反馈：追加 /mem/feedback.jsonl（含当轮问答原文）。v: +1 / -1
  bool feedback(int v, const String& user, const String& reply);
  // "asr_try":N,... 形式（不带花括号），/api/metrics 与 /api/status 共用
  String json() const;

  // ---- 上一段会话快照（NVS，重启不清零的"跨会话历史"） ----
  // persist()：当前计数器+失败环 → NVS blob（5 分钟节拍/失败限速 60s/主动重启前）；
  // loadPrev()：开机读回为 _prev。prev() 即"上一段醒来"的完整指标（同结构同口径）
  void persist();
  void loadPrev();
  const LaapMetrics* prev() const { return _prev; }   // 无上一段会话时为 null
  bool hasPrev() const { return _prev != nullptr; }

  // ---- 失败记录环（规则自进化的"素材端"）----
  // 记最近 6 条有文本的失败（ASR/视觉/搜索/LLM 的报错原文），RAM 环不落盘：
  // 它是给夜间规则归纳当样本用的，重启丢失可接受（feedback.jsonl 才是持久证据）。
  void failNote(const String& s);     // 与上一条相同自动去重（连败刷屏只算一条）
  String failDigest() const;          // "1. ...\n2. ..."；空=无失败
  // 反馈文件末尾 maxLines 行原文（含 👍/👎 与问答），给夜间规则归纳当素材
  String feedbackDigest(int maxLines = 6);

private:
  String _failRing[6];
  uint8_t _failIdx = 0, _failCnt = 0;
  uint32_t _lastPersistMs = 0;        // 失败限速节流（60s）
  uint32_t _lastFailMs = 0;           // 失败即时写自身的节流基准（v3.51：与周期快照解耦，
                                      // 否则 5 分钟快照刚落盘后 60s 内的失败不会即时写）
  LaapMetrics* _prev = nullptr;       // 上一段会话快照（loadPrev 填充；指针防自嵌套）
  void failSticky();                  // 失败计数+1 后的节流即时落盘
};

extern LaapMetrics metrics;

// ---- 重启原因可观测性 ----
// esp_reset_reason() 只能分大类；"软件重启"下的自愈打盹/OTA/网页重启/恢复出厂
// 靠重启前在 NVS 打标细分，开机 laapBootReason() 读回（/api/status 的 boot_reason 字段）。
// 主动重启一律走 laapReboot(原因)，别直接 ESP.restart()——否则下次"为什么重启"只能靠失败环反推。
void laapReboot(const char* why);
String laapBootReason();

// ---- 黑匣子（v3.65）----
// 无声重启（复位码11=USB外设复位）连 panic 打印都来不及输出，死前在干什么无从知晓。
// RTC 慢内存 survives 软复位/看门狗复位且零 flash 磨损：关键路径顺手写"最后活动"标签
// （带当时的堆水位），重启后 /api/status 的 bb 字段与开机串口直接读回。
// 写入廉价（strlcpy+两次堆查询 ~10µs），只埋在慢路径（LLM/TTS/视觉/心跳），别放进每帧循环。
void laapBlackBox(const char* tag);
String laapBlackBoxText(bool consume = false);   // v3.76f：consume=true 读取并清除崩溃现场（Web 用）
void laapPanicCaptureInit();   // v3.76f：注册 panic 钩子（setup 早段；崩溃现场存 RTC，重启后黑匣子带回）
// v3.69 微标签：loop() 各子阶段写一个 RTC 微相位（普通内存存储，零开销；~1ms 分辨率）。
// 无声崩溃后 bb 显示的相位=死点所在子阶段，把排查从"整个 loop"缩到"某一段"。
void laapBBPhase(uint8_t ph);

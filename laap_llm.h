#pragma once
#include <Arduino.h>

// ============================================================
// LLM 客户端（LAAP 的语言中枢，对应 aris_lm）
// OpenAI 兼容 /chat/completions：
//   DeepSeek:  https://api.deepseek.com
//   智谱GLM:   https://open.bigmodel.cn/api/paas/v4
//   Kimi:      https://api.moonshot.cn/v1
//   OpenAI:    https://api.openai.com/v1
//   中转/new-api: 任意 base
// ============================================================

struct LlmReply {
  bool ok = false;
  String say;    // 正文
  String expr;   // 表情关键词 calm/happy/curious/excited/lonely/anxious/tired
  int httpStatus = 0;
};

// 多轮对话的单条消息
struct LlmMsg {
  const char* role;    // "system" / "user" / "assistant"
  String content;
};

// v3.70 流式回调：SSE 正文每凑满一句可播报的句子时同步回调（在 LLM 任务语境）——
// 回调里只做入队等轻操作，禁止网络/播放/长阻塞。sentence 已 trim、≥4 字节。
typedef void (*LlmSentenceCb)(const String& sentence, void* ctx);

class LlmClient {
public:
  LlmReply chat(const String& systemPrompt, const String& userPrompt,
                int maxTokens = 200, float temperature = 0.9f);
  // 多轮版本（自发独白/搜索追问用）：roles 必须以 system 开头
  LlmReply chatMsgs(const LlmMsg* msgs, int count,
                    int maxTokens = 200, float temperature = 0.9f);
  // v3.70 流式（SSE）：正文增量接收 + 句子级回调——首句 0.5~1s 即可开始 TTS（对话提速核心）。
  // skipFirstLine=true 时首行（情绪词）不外播；流式拿不到任何正文时自行回退非流式一次。
  LlmReply chatMsgsStream(const LlmMsg* msgs, int count, int maxTokens, float temperature,
                          LlmSentenceCb cb, void* ctx, bool skipFirstLine = true);
  // 内部：带剩余续写深度的多轮请求（depth=0 不再续；chatMsgs 按 cfg.s.llmContinue 初始化）
  // v3.70：cb 非空时走流式路径；续写/回退调用一律 cb=nullptr（保持原契约）
  LlmReply chatMsgsContinue(const LlmMsg* msgs, int count,
                            int maxTokens, float temperature, int depth,
                            LlmSentenceCb cb = nullptr, void* ctx = nullptr, bool skipFirstLine = true);
  // 后台"测试连接"用
  bool ping(String& reply);
  String lastError;
  // JSON 工具（laap_vision 直连视觉模型时复用）
  static String jsonEscape(const String& s);
  static bool extractStringField(const String& json, const char* key, String& out);
private:
  String buildUrl() const;
};

// \uXXXX 解码对（全仓唯一实现，web 的 jsonField 共用）
unsigned laapHex4(const String& s, int i);
void laapAppendUtf8(String& o, unsigned cp);
// UTF-8 安全截断（laap_llm.cpp 实现，全局可用）：len 字节上限处回退到字符边界
String utf8Cut(const String& s, int len);
// UTF-8 兜底清洗：丢掉非法字节（历史遗留的"切半汉字"）。写盘与出 JSON 前过一遍，
// 否则半个汉字会让整个 JSON 或 LLM 请求体非法（/api/memory 曾因此吐不出合法 JSON）
String sanitizeUtf8(const String& s);

// HTTP 响应按帧精确读取（全仓唯一实现）：状态行+头 → hdrs，体 → payload（chunked
// 读取时精确去壳）。返回值 = 按帧干净结束（keep-alive 复用判据）；
// Connection:close 的调用方忽略返回值即可。maxBody=响应体上限（防失控）。
#include <WiFi.h>
bool laapHttpRead(WiFiClient* c, uint32_t timeoutMs, String& hdrs, String& payload, int maxBody = 40000);

// 跨任务网络客户端互斥（搜索/视觉/连通性测试共用）：后台独白任务与主线程
// 都会用同一批客户端对象，而它们的 lastError 等成员是 String——
// 两任务并发写 = String 撕裂 → 堆损坏/莫名重启。拿不到就优雅降级，绝不长阻塞。
bool laapNetLock(uint32_t ms = 400);
void laapNetUnlock();
void laapNetInit();                 // setup 里显式建锁（懒创建的 check-then-create 有竞态）

extern LlmClient llm;

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

class LlmClient {
public:
  LlmReply chat(const String& systemPrompt, const String& userPrompt,
                int maxTokens = 200, float temperature = 0.9f);
  // 多轮版本（自发独白/搜索追问用）：roles 必须以 system 开头
  LlmReply chatMsgs(const LlmMsg* msgs, int count,
                    int maxTokens = 200, float temperature = 0.9f);
  // 内部：带剩余续写深度的多轮请求（depth=0 不再续；chatMsgs 按 cfg.s.llmContinue 初始化）
  LlmReply chatMsgsContinue(const LlmMsg* msgs, int count,
                            int maxTokens, float temperature, int depth);
  // 后台"测试连接"用
  bool ping(String& reply);
  String lastError;
  String endpoint() const;
  // JSON 工具（laap_vision 直连视觉模型时复用）
  static String jsonEscape(const String& s);
  static bool extractStringField(const String& json, const char* key, String& out);
private:
  String buildUrl() const;
};

// UTF-8 安全截断（laap_llm.cpp 实现，全局可用）：len 字节上限处回退到字符边界
String utf8Cut(const String& s, int len);
// UTF-8 兜底清洗：丢掉非法字节（历史遗留的"切半汉字"）。写盘与出 JSON 前过一遍，
// 否则半个汉字会让整个 JSON 或 LLM 请求体非法（/api/memory 曾因此吐不出合法 JSON）
String sanitizeUtf8(const String& s);

extern LlmClient llm;

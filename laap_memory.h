#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 三层记忆系统
//   工作记忆  ← RAM，最近 12 条（当前对话上下文）
//   情景记忆  ← LittleFS /mem/episodes.jsonl，最近 300 条
//   语义记忆  ← /mem/semantic.txt，LLM 周期性压缩出的"自我认知"
//   对应 laap-AGI: aris_episodic_memory + laap_memory_hierarchy
// ============================================================
class MemorySystem {
public:
  bool begin();
  void logEvent(const char* role, const String& text);   // user|aris|event → 双写
  String recentContext(int maxChars = 600);              // 工作记忆（近→远）
  // F3: 工作记忆原始条目（近→远顺序, 最多 max 条, 只含 user|aris 角色）
  // 近 max 轮对话（远→近），roles 并行输出说话人：0=主人 1=它自己（调用方据此标 role，别再靠奇偶猜）
  int recentTurns(String* out, uint8_t* roles, int max) const;
  String searchEpisodic(const String& query, int maxChars = 300); // 关键词回忆
  // 智能回忆（Mem0 式多信号）：优先语义向量召回（embedOk 时），退关键词+重要度加权
  String recallSmart(const String& query, int maxChars = 300);
  void   rememberBoost(const String& fragment);   // 用户问起=该记忆重要（升级权重）
  // 语义向量（bge-m3 经硅基流动；异步缓存，无向量时 recallSmart 自动退关键词）
  void   embedTick();                              // loop 调用：给未嵌入的记忆补向量（限速）
  bool   embedOk() const { return _embFail < 3; }  // 连败3次后本轮停用（退关键词）
  String semantic() const;
  void setSemantic(const String& s);
  uint32_t eventCount() const { return _count; }
  String episodicTail(int n);                            // 最近 n 条（Web 查看）
  void clearAll();

private:
  void appendEpisodic(const char* role, const String& text);
  void rewriteEpisodicByScore();                   // 淘汰：按 (权重,时间) 排序丢低分
  float* embVec(const String& line);                // 该记忆行的向量缓存（无则nullptr）
  // 工作记忆环：40 条×单条 160 字上限 ≈ 最坏 25KB 堆（常驻）。
  // 槽位复用（assign 覆写）避免 40 个 String 反复分配/析构造成堆碎片。
  static const int WORK_MAX = 40;
  String _work[WORK_MAX];
  int _workHead = 0, _workLen = 0;
  uint32_t _count = 0;
  // 语义向量缓存：/mem/emb.bin（1024×float=4KB/条，与 episodes.jsonl 行序对齐）
  uint32_t _embCount = 0;      // 已有向量条数
  uint8_t _embFail = 0;        // 连续失败计数
  uint32_t _embLastMs = 0;     // 限速用
};

extern MemorySystem memory;

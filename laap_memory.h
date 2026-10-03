#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 三层记忆系统
//   工作记忆  ← RAM，最近 12 条（当前对话上下文）
//   情景记忆  ← LittleFS /mem/episodes.jsonl，最近 300 条
//   语义记忆  ← /mem/semantic.txt，LLM 周期性压缩出的"自我认知"
//   关系记忆  ← /mem/relations.jsonl，偏好/承诺/边界（夜间从经历抽取，v3.43）
//   对应 laap-AGI: aris_episodic_memory + laap_memory_hierarchy
// ============================================================
class MemorySystem {
public:
  bool begin();
  void logEvent(const char* role, const String& text);   // user|aris|event → 双写
  String recentContext(int maxChars = 600);              // 工作记忆（近→远）
  // 同上，但跳过含 exclude 关键字的条目（独白出题/意图生成断"自己喂自己"环）
  String recentContextExcluding(int maxChars, const char* exclude);
  // F3: 工作记忆原始条目（近→远顺序, 最多 max 条, 只含 user|aris 角色）
  // 近 max 轮对话（远→近），roles 并行输出说话人：0=主人 1=它自己（调用方据此标 role，别再靠奇偶猜）
  int recentTurns(String* out, uint8_t* roles, int max) const;
  // 智能回忆（Mem0 式多信号）：语义向量为主，权重/新鲜度/心里目标/情绪同色调加权；无向量退关键词
  String recallSmart(const String& query, int maxChars = 300);
  void   rememberBoost(const String& fragment);   // 用户问起=该记忆重要（升级权重）
  // 关系记忆层（v3.43，借鉴"识海手稿"）：主人的偏好/答应的事/要守住的边界
  String relationsFor(const String& query, const String& goal, int maxLines = 2); // 相关条目并入召回
  int  relationsApply(const String& llmText);     // 夜间抽取结果落盘（去重/封顶40），返回新增条数
  String relationsText() const;                   // 全部关系事实（/api/relations、串口 /relations）
  // 情绪精标注（v3.46，手稿"情绪权重"精确版）："行号|情绪" → 改写记忆行的 m 字段。
  // 行号=episodicNumberedTail 绝对行号（按非空行计）；重写保持行数不变（不破 emb.bin 行序对齐）
  int  moodApply(const String& llmText);
  // 语义向量（bge-m3 经硅基流动；异步缓存，无向量时 recallSmart 自动退关键词）
  void   embedTick();                              // loop 调用：给未嵌入的记忆补向量（限速）
  bool   embedFused() const { return _embFail >= 3; } // 语义通道熔断中（/api/status 暴露）
  uint32_t embedCount() const { return _embCount; }   // 已有向量的记忆条数（与 emb.bin 行数一致）
  String semantic() const;
  void setSemantic(const String& s);
  uint32_t eventCount() const { return _count; }
  String episodicTail(int n);                            // 最近 n 条（Web 查看）
  // 夜间记忆整理（Letta 式 sleep-time compute）：带绝对行号的尾部导出 + 执行 LLM 给出的删除清单。
  // 只允许"删"这一种操作且固件侧验证上限——绝不把记忆正文交回模型重写（防幻觉篡改人格）
  String episodicNumberedTail(int n);                    // "12. {json}"（绝对行号）
  void applyTidyOps(const String& opsJson);              // [{"n":行号,"op":"del"}]，≤12 条
  float noveltyOf(const String& text);                   // 新颖度 0..1（-1=无法评估）
  void clearAll();
  // 记忆搬家（备份/恢复/换分区）：分段纯文本，含情景+语义+性格进化。
  // 导入先落临时文件再逐行解析，整份不进内存（300 行也只要几 KB 缓冲）
  String exportDump();
  bool   importBegin();                                  // 打开 /mem/import.txt 写
  bool   importWrite(const uint8_t* d, size_t n);        // 追加（Web 上传分片调用）
  void   importEnd();
  bool   applyImport(String& msg);                       // 解析并落盘（覆盖现有记忆）
  void   reloadWork();                                   // 从盘上重建工作记忆环（导入后用）
  void   onRestored();                                   // v3.76e：快照恢复后重数记忆/清向量计数（防恢复→重启窗口错位）

private:
  void appendEpisodic(const char* role, const String& text);
  void rewriteEpisodicByScore();                   // 淘汰：按 (权重,时间) 排序丢低分
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

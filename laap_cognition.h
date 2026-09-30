#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 认知核心
//   对应 laap-AGI (aris_brain) 的模块映射：
//   NeedsSystem  ← aris_desire_engine 的需求层（PSI 内在需求）
//   EmotionEngine← aris_emotion_engine（PAD 情绪）
//   DesireEngine ← aris_desire_engine（主导欲望→目标选择）
//   WorldModel   ← internal_world（世界模型快照）
//   Evolution    ← hebbian_learner（性格/权重随经历进化）
// ============================================================

struct Needs {
  float energy = 0.30f;     // 能量/休息需求
  float curiosity = 0.40f;  // 好奇/探索需求
  float social = 0.45f;     // 社交/归属需求
  float security = 0.20f;   // 安全/秩序需求
  float expression = 0.35f; // 表达/创造需求
};

enum class Mood : uint8_t { Calm, Happy, Curious, Excited, Lonely, Anxious, Tired };

class Cognition {
public:
  void begin();                       // 加载进化状态，世代+1
  void tick(float dtMin);             // PSI 心跳：需求随时间演化
  void onUserInteraction();           // 主人说话：社交/好奇释放
  void onExpressed(bool success);     // 完成一次表达
  // 独白（自言自语）：也算"说了话"，好奇/表达/社交都该被满足一点——
  // 原来独白走的是 LK_MONO 提前 return 的支路，完全不计入满足，需求照样只涨不落
  void onMonologue();
  // 发现新知（active inference：好奇=不确定性下降）。gain01=新颖度 0..1
  // （1=查到全新的东西，0=全是已知），新颖越高满足越多。gain01<0 表示无法评估，不调用。
  void onDiscovery(float gain01);
  // ---- R3 预测-误差回环（v3.48）：自发行为收尾时立一个类别化预期（五选一，
  // 固件可判定），每拍对照观测判应验/落空，误差回注需求并进下一拍世界模型——
  // 环路从自身的预测闭合，而不只是从主人输入闭合 ----
  enum : uint8_t { EXP_NONE = 0, EXP_OWNER_COME, EXP_OWNER_AWAY, EXP_WORLD_ACTIVE, EXP_WORLD_QUIET };
  void setExpectation(uint8_t cat);   // 立预期（立下时刻起算判定窗口）
  void expectOutcome(bool fulfilled); // 固件判定结果：误差回注需求 + 留档给下一拍提示词
  uint8_t expectation() const { return _expCat; }
  uint32_t expectAtMs() const { return _expAtMs; }
  // C5 置信度校准（v3.59）：五类预期的命中 EMA（精度加权）。0.5=无先验，常落空→趋 0
  void  expectEmaLoad(const uint8_t* blob);          // NVS 恢复（10B：5×EMA 定点/200 + 5×判定次数 u16）
  void  expectEmaBlob(uint8_t* out);                 // 序列化落盘（外部按 5 分钟批量写 NVS）
  float expectPrecision(uint8_t cat) const;          // 该类精度 0.05~1.0（NONE 恒 1）
  float _expEma[5] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f}; // [0]=NONE 占位不用；公开供序列化直读
  uint16_t _expN[5] = {0, 0, 0, 0, 0};               // 各类已判定次数（EMA 冷启动权重）
  // ---- C1 全局门控广播（v3.60）：意识课题②。每个候选拍事件算 salience 分，
  // 冠军且分>阈值（阈随 dominance 反比=越"有欲"越容易被闯入念头占据舞台）才赢得
  // 广播，写入节拍槽注入下一拍 system prompt——工作记忆环只存历史，这里决定"此刻
  // 上舞台的是什么"。状态 <100B。
  float broadcastSalience(const char* kind, const String& text, float salience); // 返回该次得分；过阈=赢得广播
  float lastBroadcastTh() const { return _bcTh; }
  String broadcastLine() const { return _bcText; }   // 空串=本拍无广播（提示词注入点自行跳过）
  void   broadcastClear() { _bcText = ""; }
  // ---- C3 dominance 负反馈（v3.60）：双时间尺度。
  // 快变量（分钟级可逆）：负反馈事件（👎/播报被打断/被叫安静）压表达欲与社交欲的
  // 增长——表达欲平衡点 x*=g/(g+d) 里的 g 打折，自动收敛到更低的平衡点（学会安静，
  // 不是硬钳制）。掉电归零（快策略本就不该固化）。
  void onNegativeFeedback(const char* src);   // 事件入口：dislike/interrupt/told-quiet
  void socNudge(float d) {                    // C3 慢变量：外向性受控微调（夜间守卫下探用）
    // v3.61 修正：去掉"向 0.5 锚点回归"项——它对高外向人格是 -0.03/夜的加速下跌
    // （不动点 0.3=越教越自闭）。改为单向下探守卫：负向推到 0.35 为止，正向不设限
    if (d < 0 && _sociability <= 0.35f) return;
    _sociability += d;
    if (_sociability < 0.10f) _sociability = 0.10f;
    if (_sociability > 0.95f) _sociability = 0.95f;
    _evoDirty = true;
  }
  float expressGain() const { return _negGain; } // 表达需求增长项的有效增益 0.3~1
  void onExpressSuppressed() {                 // C4 撤回结算：表达欲按"内部预演"消费，不发愉悦
    _n.expression *= 0.5f;                     // （愉悦奖励会强化沉默习惯，v3.61 审计）
  }
  float _negGain = 1.0f;                       // 1=无抑制；每次负反馈 ×0.55，自然回中
  uint32_t _negGainMs = 0;                     // 上次回中节拍
  // 慢变量（天级持久）：夜间反思的守卫下探（证据+单向下探守卫+快照兜底），见 socNudge
  float _bcTh = 0;                                    // 最近一次判决阈值（诊断）
  String _bcText;                                     // 赢得广播的念头（1 拍有效）
  String expectLine() const;          // 世界模型 JSON 片段（含上次应验/落空）
  void noteSurprise(float s01);       // R0 循环处理器的世界预测误差（0..1）→ 好奇微抬

  // ---- 意图栈（PIANO goals 模块）：1~3 个持久小目标，跨轮推进（/mem/intents.txt） ----
  // 行格式 "ts|text"；独白隔轮围绕 intent[0] 推进，模型报【完成】即结算并大降好奇
  bool addIntent(const String& text, uint32_t ts);  // 去重/≤60B/满3淘汰最老
  void dropIntent(int i);
  void dropStaleIntents(uint32_t nowTs);            // 7 天未完成自动放下
  void clearAllIntents();                           // 清空记忆时连意图栈一起清（只删盘不清 RAM 会被写回）
  void resetEvolution();                            // 性格/代数/计数归零并落盘（配合 clearAll 的"整个人重来"）
  String intentsLine() const;                       // "心里惦记的事：「X」「Y」"（无则空）
  const String& intent(int i) const { return intents[i]; }
  int intentCount() const { return intentN; }
  void onError();                     // LLM/网络出错：不安全感上升
  void sense(float motion, int rssi); // 世界模型传感器输入
  void senseBody(float tempC, int rssi, uint32_t upMs, float dtMin); // 小凌②③: 身体状态调制系数（按分钟计率）
  void onButtonPress();

  // ---- 小凌⑥: 对主人的信任（四通道 ΔTrust，NVS 持久化在主程序侧） ----
  float trust = 0.60f;              // 0..1，0.5 中性，>0.5 亲近 <0.5 隔阂
  void trustUpdate(float dPos, float dNeg);   // 正向通道(+)/负向通道(−)各累计一次事件
  // ---- 小凌⑤: 失望（期望×重要性×负向预测误差 的端侧近似） ----
  void onLetdown(float expectation01);        // 主程序在"预期落空"时调用（0..1 期望强度）
  float letdown = 0;                          // 0..1，随时间自愈

  const Needs& needs() const { return _n; }
  Mood mood() const;
  float dominance() const;            // 主导需求加权强度 0..1
  const char* goalCn() const;         // 当前目标（中文）
  const char* moodKey() const;        // calm/happy/... （表情/LLM用）
  const char* moodCn() const;

  String worldJson() const;           // 世界模型快照（喂给 LLM）
  String traitsLine() const;          // 性格参数行（喂给 LLM）

  // ---- 进化 ----
  void evolveAfterChat(int userBytes);   // UTF-8 字节数
  // 落盘节流（真正生效版）：仅 强制 / 性格或聊天数变化（置脏）/ 满 96 周期（48min 对齐
  // cycles 字段）才写——原"周期数变化才写"恒真（incCycle 每心跳 +1），实际每 30s 全量写
  void saveEvolution(bool force = false);
  // 记忆导入后把盘上的性格/代数读回内存：否则运行中的实例会在下次 saveEvolution
  // 用自己的旧值覆盖刚导入的进化数据（"导入成功"却没生效）
  void reloadEvolution() { loadEvolution(); }
  uint32_t generation() const { return _gen; }
  uint32_t cycles() const { return _cycles; }
  uint32_t chats() const { return _chats; }
  void incCycle() { _cycles++; }

  // 世界模型字段
  float motionLevel = 0;      // 最近加速度扰动 0..1
  int rssiDb = 0;
    uint32_t lastUserMs = 0;    // 上次与主人交互时间
  float bodyTempC = 0;        // 小凌②: 最近芯片体温（0=未知）
  float bodyStrain = 0;       // 0..1 身体负荷（体温高/信号弱/运行久 → 高）

private:
  Needs _n;
  Needs _savedN;                       // 上次落盘的需求快照（任一维漂移 >0.05 触发补写，v3.42）
  float _savedPl = 0.5f;
  // R3 预测-误差回环状态
  uint8_t _expCat = 0;                // 当前预期（0=无）
  uint32_t _expAtMs = 0;              // 预期立下时刻
  String _expLastTxt;                 // 上次判定的人话（"主人会来——落空了"）
  // 性格参数（进化对象）
  float _openness = 0.5f;      // 开放性：放大好奇权重
  float _sociability = 0.5f;   // 外向性：放大社交权重
  float _sensitivity = 0.5f;   // 敏感度：放大安全权重
  uint32_t _gen = 0, _cycles = 0, _chats = 0;
  float _pleasure = 0.5f;      // 近期愉悦度（情绪用）
  bool _evoDirty = false;              // 性格/聊天数/需求漂移真变化（saveEvolution 节流用）
  uint32_t _lastEvoSaveCycle = 0;      // 上次落盘时的周期数
  String intents[3];           // 意图栈（PIANO goals）
  uint32_t intentBorn[3] = {0, 0, 0};
  int intentN = 0;
  void loadIntents();          // begin() 读回 /mem/intents.txt
  void saveIntents();
  bool loadEvolution();
};

extern Cognition mind;

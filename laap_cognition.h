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

  // ---- 意图栈（PIANO goals 模块）：1~3 个持久小目标，跨轮推进（/mem/intents.txt） ----
  // 行格式 "ts|text"；独白隔轮围绕 intent[0] 推进，模型报【完成】即结算并大降好奇
  bool addIntent(const String& text, uint32_t ts);  // 去重/≤60B/满3淘汰最老
  void dropIntent(int i);
  void dropStaleIntents(uint32_t nowTs);            // 7 天未完成自动放下
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
  void saveEvolution();
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
  // 性格参数（进化对象）
  float _openness = 0.5f;      // 开放性：放大好奇权重
  float _sociability = 0.5f;   // 外向性：放大社交权重
  float _sensitivity = 0.5f;   // 敏感度：放大安全权重
  uint32_t _gen = 0, _cycles = 0, _chats = 0;
  float _pleasure = 0.5f;      // 近期愉悦度（情绪用）
  String intents[3];           // 意图栈（PIANO goals）
  uint32_t intentBorn[3] = {0, 0, 0};
  int intentN = 0;
  void loadIntents();          // begin() 读回 /mem/intents.txt
  void saveIntents();
  bool loadEvolution();
};

extern Cognition mind;

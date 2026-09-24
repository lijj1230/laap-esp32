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
  void onError();                     // LLM/网络出错：不安全感上升
  void sense(float motion, int rssi); // 世界模型传感器输入
  void senseBody(float tempC, int rssi, uint32_t upMs); // 小凌②③: 身体状态调制系数
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
  void evolveAfterChat(int userWords);
  void saveEvolution();
  uint32_t generation() const { return _gen; }
  uint32_t cycles() const { return _cycles; }
  uint32_t chats() const { return _chats; }
  void incCycle() { _cycles++; }

  // 世界模型字段
  uint32_t bootId = 1;
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
  bool loadEvolution();
};

extern Cognition mind;

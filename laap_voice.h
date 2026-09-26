#pragma once

// F8: 按键对讲触发前调用，绕过一次唤醒词门（定义在 laap_voice.cpp）
void laapVoiceSetManualOnce();
#include <Arduino.h>

// ============================================================
// 语音编排器：TTS 多通道回退 + 听取 + 唤醒轮询
//   TTS: Edge(免费默认) → 火山(官方备选) → 静音降级(只显示文字)
//   听: 按住BOOT对讲 / VAD自动模式(听到人声即开始, 静音0.7s结束)
//   打断: 播放期间麦克风能量门限(近似AEC, 安静房间够用)
// ============================================================

enum class VoiceMode : uint8_t { Off = 0, Button = 1, Vad = 2 };

class LaapVoice {
public:
  void begin();
  bool ready() const { return _ready; }

  // 说话（多通道回退）。expr 用于说话时的屏幕表情
  void speak(const String& text, const char* expr = nullptr);

  // 一轮完整对话: 录音→ASR→走主程序 laapInteract→speak 回复
  // 返回识别到的用户文本（空=未识别/取消）
  String converse();

  // 应答段（converse 的后半截）：把一句"听到的话"交给主程序并决定怎么念。
  // 抽出来是为了让诊断命令 /voicetest 能走**同一条**路径（不起麦也能验"只说一次"）。
  String respond(const String& heard);

  // 空闲轮询（VAD 模式下检测到人声→自动开启一轮对话）
  void loopTick();

  // 参数自调优（RSI⑥，psiTick 心跳调用，内部按 5 分钟窗口评估）：
  // A 播报冷却 1200~3000ms（冷却期误触发多→拉长）；B VAD 阈值乘数 1.0~2.0
  // （"触发但没听清"多→抬）。RAM 常驻每次开机回默认，所有调整打 [TUNE] 日志。
  void tuneTick();
  uint32_t cooldownDur() const { return _cooldownDur; }
  float vadMul() const { return _vadMul; }

  bool busy() const { return _busy; }
  String lastError;

  // VAD 自动聆听的暂停/恢复（BOOT 短按 / Web 均可控制）
  bool vadPaused() const { return _vadPaused; }
  void setVadPaused(bool paused);

private:
  bool listenAndTranscribe(String& heard);
  bool _ready = false;
  bool _busy = false;
  uint32_t _cooldownMs = 0;   // 播报后冷却截止时刻，避免自听见
  uint32_t _cooldownDur = 1200; // 冷却时长（自调优旋钮 A，1200~3000ms）
  float _vadMul = 1.0f;         // VAD 阈值乘数（自调优旋钮 B，1.0~2.0）
  bool _vadHold = false;
  uint32_t _vadHoldStart = 0;
  bool _vadPaused = false;
};

extern LaapVoice voice;

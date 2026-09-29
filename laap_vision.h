#pragma once
#include <Arduino.h>

// ============================================================
//  F9 多模态之眼：GC0308 摄像头（DVP）→ JPEG → base64，双模式：
//  ① 手机桥：visionBase 填 vision_bridge.py 地址（/vision）→ {desc}
//  ② 直连：visionBase 留空 + visionKey/llmKey → OpenAI 兼容
//     多模态接口（默认 OpenRouter，模型默认 gemini-flash-1.5）
//  两种都没配置时模块静默不工作，不影响其它功能。
// ============================================================

class LaapVision {
public:
  bool begin();                 // 初始化 esp32-camera（GC0308 引脚表）
  bool available() const { return _ok; }
  // 抓一帧并让视觉后端描述；返回描述文本（失败返回 ""，lastError 带原因）。
  // 内部带跨任务互斥：后台独白起意前也会 look()，与主线程聊天/CLI 重叠时退化返回
  String look(const String& question = "");
  // 把最近一次所见注入记忆（世界模型"视觉"通道）
  void logSight(const String& desc);
  String debugPngB64(size_t& outLen);   // 诊断：走同一条抓帧+PNG+base64 链路并把结果交出来
  String lastError;
private:
  String lookLocked(const String& question);   // look() 的无锁真身
  bool _ok = false;
};

extern LaapVision vision;

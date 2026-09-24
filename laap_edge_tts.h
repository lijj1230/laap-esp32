#pragma once
#include <Arduino.h>

// ============================================================
// Edge TTS 通道（免费无 Key，逆向微软 Edge 朗读服务）
//   wss://speech.platform.bing.com ... Sec-MS-GEC 鉴权（NTP 准时必需）
//   输出 audio-24khz-48kbitrate-mono-mp3 → libhelix 解码 → 48k 播放
// ============================================================

class EdgeTts {
public:
  // text: 要说的话 voice: 如 zh-CN-XiaoxiaoNeural rate: "+0%"
  // 成功播放返回 true。interruptible: 播放期间允许能量门打断
  bool speak(const String& text, const String& voice, const String& rate, bool interruptible = true);
  String lastError;
};

extern EdgeTts edgeTts;

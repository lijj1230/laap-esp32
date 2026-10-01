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
  // 默认 false：泄漏基线 ×1.9 的能量门会被语音自身的动态范围误触发，把 TTS 砍成"无声成功"
  bool speak(const String& text, const String& voice, const String& rate, bool interruptible = false);
  // v3.74 连接预取：提前完成 WS-TLS 握手（LLM 吐字期间并行），speak 优先收养省 ~0.5s。
  // 可在任意任务语境调用（内部自带互斥与竞态处置）；失败静默（speak 照旧现场握手）。
  void preconnect();
  String lastError;
};

extern EdgeTts edgeTts;

// 自听回环诊断（/asrloop）：合成期间把解码后的 PCM 也攒进 buf（重采样成 16k 单声道），
// 调用方拿到样本数后即可边播边录。用来把"ASR 请求/服务端"与"麦克风拾音"分开验证。
void laapTtsCaptureBegin(int16_t* buf, size_t cap, int srcRate);
size_t laapTtsCaptureEnd(void);

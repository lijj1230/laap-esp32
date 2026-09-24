#pragma once
#include <Arduino.h>

// ============================================================
// ASR: OpenAI 兼容 /v1/audio/transcriptions（multipart 上传 WAV）
//   免费/可用后端：SiliconFlow SenseVoiceSmall（免费）、new-api 网关、
//   OpenAI whisper-1。返回识别文本。
// TTS 备选: 火山引擎（豆包）HTTP TTS，返回 base64 WAV
// ============================================================

class AsrClient {
public:
  // pcm16k: 16kHz 16bit 单声道；返回空串表示失败
  String transcribe(const int16_t* pcm16k, size_t bytes, String& err);
  // 录音开始前调用：后台完成 TCP+TLS 握手（小智"录传并行"思想，握手被录音时长吸收）
  void warmup();
  // 预热连接是否还活着（活则无需重握手）
  bool warmAlive();
};

class VolcTts {
public:
  bool speak(const String& text, String& err);
};

extern AsrClient asr;
extern VolcTts volcTts;

// WAV 头构造（16k mono 16bit）
size_t wavWrap(const int16_t* pcm, size_t bytes, uint8_t* out, size_t outCap);

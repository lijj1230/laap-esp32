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
  // v3.76e：setup 里调用一次——创建预热互斥量（懒建是 check-then-create 竞态）
  void begin();
  // pcm16k: 16kHz 16bit 单声道；返回空串表示失败
  String transcribe(const int16_t* pcm16k, size_t bytes, String& err);
  // 连通性探针（设置页"逐项体检"用）：合成 0.3s 正弦音发给指定服务商，
  // HTTP 200 即通（空音频转不出字属正常，连通+鉴权已确认）。which: 0=主 1=备用。
  // 返回空串=通，非空=失败原因（"未配置"=跳过）
  String probe(int which);
  // 录音开始前调用：后台完成 TCP+TLS 握手（小智"录传并行"思想，握手被录音时长吸收）
  void warmup();
  // 预热连接是否还活着（活则无需重握手）
  bool warmAlive();
  // 主动掐掉预热连接：TLS 要 ~31KB+ 连续内部内存，LLM 起飞前堆紧时请它让位
  void warmDrop();
};

class VolcTts {
public:
  bool speak(const String& text, String& err);
};

extern AsrClient asr;
extern VolcTts volcTts;

// WAV 头构造（16k mono 16bit）
size_t wavWrap(const int16_t* pcm, size_t bytes, uint8_t* out, size_t outCap);

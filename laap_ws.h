#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// ============================================================
// 极简 WebSocket/WSS 客户端（RFC6455）
//   用于 Edge TTS（wss）。客户端帧强制掩码，支持文本/二进制/
//   ping-pong/close，忽略分片续帧（本用途用不到）。
// ============================================================
class WsClient {
public:
  // extraHeaders: 每行 "Key: value\r\n"
  bool connect(const char* host, int port, const char* path, const char* extraHeaders, uint32_t timeoutMs = 15000);
  void stop();
  ~WsClient();   // v3.65：补 _bin 释放（原无析构=每次 TTS 泄漏最大帧+512B，聊几轮堆见底）

  bool sendText(const String& text);

  // 收一帧。返回: 1=文本 2=二进制 0=无数据/超时 -1=错误/关闭
  int poll(uint32_t timeoutMs);
  const String& textPayload() const { return _text; }
  const uint8_t* binPayload() const { return _bin; }
  size_t binLen() const { return _binLen; }
  String lastError;

private:
  bool sendFrame(uint8_t opcode, const uint8_t* data, size_t len);
  bool _tls = true;
  WiFiClient* _nc = nullptr;
  WiFiClientSecure _secure;
  WiFiClient _plain;
  String _text;
  uint8_t* _bin = nullptr; size_t _binLen = 0, _binCap = 0;
  bool ensureBin(size_t n);
};

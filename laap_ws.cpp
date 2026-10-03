#include "laap_ws.h"
#include <mbedtls/base64.h>

static String wsKey() {
  uint8_t rnd[16];
  for (int i = 0; i < 16; i++) rnd[i] = esp_random() & 0xFF;
  uint8_t out[32]; size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out), &olen, rnd, 16);
  return String((char*)out).substring(0, olen);
}

bool WsClient::connect(const char* host, int port, const char* path, const char* extraHeaders, uint32_t timeoutMs) {
  _tls = (port == 443 || port == 8443 || port == 2053 || port == 2087 || port == 2096);  // 常见 TLS 端口
  if (_tls) {
    _secure.setInsecure();
    _secure.setTimeout(timeoutMs / 1000 + 1);
    if (!_secure.connect(host, port)) { lastError = "TCP/TLS 连接失败"; return false; }
    _nc = (WiFiClient*)&_secure;
  } else {
    _plain.setTimeout(timeoutMs / 1000 + 1);
    if (!_plain.connect(host, port)) { lastError = "TCP 连接失败"; return false; }
    _nc = (WiFiClient*)&_plain;
  }
  String key = wsKey();
  String req = String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
    "\r\nSec-WebSocket-Version: 13\r\n";
  if (extraHeaders) req += extraHeaders;
  req += "\r\n";
  size_t want = req.length();
  size_t wrote = _nc->print(req);
  if (wrote != want) { lastError = "握手请求发送不完整"; stop(); return false; }

  // 读到响应头结束
  String resp; uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs && _nc->connected()) {
    while (_nc->available()) {
      char c = _nc->read();
      resp += c;
      if (resp.endsWith("\r\n\r\n")) goto hdrDone;
    }
    delay(2);
  }
  lastError = "握手超时";
  stop();
  return false;
hdrDone:
  if (resp.indexOf(" 101 ") < 0) {
    int sp = resp.indexOf(' ');
    lastError = "握手被拒: " + resp.substring(0, resp.indexOf('\r'));
    stop();
    return false;
  }
  lastError = "";
  return true;
}

void WsClient::stop() { if (_nc) _nc->stop(); _nc = nullptr; }

// v3.65：_bin 是裸指针缓冲（ensureBin realloc），原类没有析构 → Edge TTS 每次播报
// 泄漏"最大二进制帧+512B"（实测每轮聊天内部堆掉 1.5~2KB，纯 LLM 路径零泄漏）。
// stop() 不动 _bin（重连场景可复用），连接对象销毁时统一释放。
WsClient::~WsClient() {
  stop();
  free(_bin);
  _bin = nullptr; _binLen = 0; _binCap = 0;
}

bool WsClient::sendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!_nc || !_nc->connected()) return false;
  uint8_t hdr[14]; int h = 0;
  hdr[h++] = 0x80 | opcode;
  uint8_t mask[4];
  for (int i = 0; i < 4; i++) mask[i] = esp_random() & 0xFF;
  if (len < 126) {
    hdr[h++] = 0x80 | (uint8_t)len;
  } else if (len < 65536) {
    hdr[h++] = 0x80 | 126;
    hdr[h++] = (len >> 8) & 0xFF; hdr[h++] = len & 0xFF;
  } else {
    hdr[h++] = 0x80 | 127;
    uint64_t l = len;
    for (int i = 7; i >= 0; i--) { hdr[h++] = (l >> (8 * i)) & 0xFF; }
  }
  hdr[h++] = mask[0]; hdr[h++] = mask[1]; hdr[h++] = mask[2]; hdr[h++] = mask[3];
  if (_nc->write(hdr, h) != (size_t)h) return false;
  // 掩码 payload 分块发送
  const int CHUNK = 2048;
  static uint8_t buf[CHUNK];
  size_t sent = 0;
  while (sent < len) {
    int n = (len - sent > CHUNK) ? CHUNK : (len - sent);
    for (int i = 0; i < n; i++) buf[i] = data[sent + i] ^ mask[(sent + i) % 4];
    if (_nc->write(buf, n) != (size_t)n) return false;
    sent += n;
  }
  return true;
}

bool WsClient::sendText(const String& t) { return sendFrame(0x1, (const uint8_t*)t.c_str(), t.length()); }

bool WsClient::ensureBin(size_t n) {
  if (_binCap >= n) return true;
  uint8_t* nb = (uint8_t*)realloc(_bin, n + 512);
  if (!nb) return false;
  _bin = nb; _binCap = n + 512;
  return true;
}

int WsClient::poll(uint32_t timeoutMs) {
  if (!_nc || !_nc->connected()) return -1;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (_nc->available() >= 2) {
      uint8_t h0 = _nc->read();
      uint8_t h1 = _nc->read();
      bool fin = h0 & 0x80;
      uint8_t opcode = h0 & 0x0F;
      bool masked = h1 & 0x80;
      uint64_t len = h1 & 0x7F;
      auto waitBytes = [&](int need) -> bool {
        uint32_t wt = millis();
        while (_nc->available() < need) {
          if (millis() - wt > 3000 || !_nc->connected()) { lastError = "帧头超时"; stop(); return false; }
          delay(1);
        }
        return true;
      };
      if (len == 126) {
        if (!waitBytes(2)) return -1;
        len = (_nc->read() << 8) | _nc->read();
      } else if (len == 127) {
        if (!waitBytes(8)) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | _nc->read();
      }
      uint8_t mask[4] = {0};
      if (masked) {
        uint32_t mt = millis();
        while (_nc->available() < 4) {           // 半开连接防线：等掩码也要有超时
          if (millis() - mt > 3000 || !_nc->connected()) { lastError = "帧掩码超时"; stop(); return -1; }
          delay(1);
        }
        for (int i = 0; i < 4; i++) mask[i] = _nc->read();
      }
      // v3.76e：帧长上限 6MB→128KB（Edge TTS 单帧实际 ~16KB；6MB 的 ensureBin 在 200KB
      // 堆上必败，超大声明帧本就不合法）
      if (len > 128 * 1024) { lastError = "帧过大"; stop(); return -1; }
      if (opcode == 0x8) { stop(); return -1; }               // close
      // v3.76e 控制帧（ping/pong）统一走有界丢弃：原来的 pong 丢弃是逐字节阻塞 read
      // 无超时无连接检查、ping 只吃 64B 造成流错步——网络半死时单次 poll 可挂死小时级
      // （2026-10-03 搜新闻卡死的实锤根因），服务器大 ping 一来 TTS 就冻结。
      if (opcode == 0x9 || opcode == 0xA) {
        uint8_t pb[64] = {0}; size_t take = len < 64 ? (size_t)len : 64;
        size_t got = 0; uint32_t ct = millis();
        while (got < (size_t)len) {
          while (_nc->available() && got < (size_t)len) {
            uint8_t b = _nc->read();
            if (got < take) pb[got] = b ^ mask[got % 4];
            got++;
          }
          if (got < (size_t)len) {
            if (!_nc->connected() || millis() - ct > 3000) { lastError = "控制帧载荷超时"; stop(); return -1; }
            delay(1);
          }
        }
        if (opcode == 0x9) sendFrame(0xA, pb, take);   // ping → pong（吃全载荷不丢字节）
        return 0;
      }

      // 数据帧（文本/二进制）
      if (!ensureBin((size_t)len + 1)) { lastError = "内存不足"; return -1; }
      uint64_t got = 0; uint32_t ft = millis();
      while (got < len) {
        while (_nc->available()) {
          uint8_t b = _nc->read();
          if (masked) b ^= mask[got % 4];   // v3.76e：掩码索引显式化（原复合表达式依赖求值顺序）
          _bin[got++] = b;
          if (got >= len) break;
        }
        if (got < len) {
          if (!_nc->connected() || millis() - ft > 20000) { lastError = "帧读取超时"; return -1; }
          delay(2);
        }
      }
      (void)fin;
      if (opcode == 0x1) {
        _bin[len] = 0;
        _text = String((const char*)_bin);
        return 1;
      }
      _binLen = (size_t)len;
      return 2;
    }
    delay(2);
  }
  return 0;
}

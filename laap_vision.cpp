#include "laap_vision.h"
#include "laap_config.h"
#include "laap_llm.h"
#include "laap_memory.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <base64.h>

LaapVision vision;

// —— GC0308 引脚表（官方 07-lcd_camera 例程 esp32_s3_szp.h 实测核实）——
#define V_PIN_XCLK   5
#define V_PIN_PCLK   7
#define V_PIN_VSYNC  3
#define V_PIN_HREF   46
#define V_PIN_D0     16
#define V_PIN_D1     18
#define V_PIN_D2     8
#define V_PIN_D3     17
#define V_PIN_D4     15
#define V_PIN_D5     6
#define V_PIN_D6     4
#define V_PIN_D7     9
// SCCB 复用主 I2C（SDA=1 SCL=2，camera sccb_i2c_port=0）
// PWDN 由 PCA9557 bit2 控制（进入本模块前 pca9557 已初始化，这里只拉高退出掉电）

static bool pca9557Pwdn(bool level) {
  // 复用 laap_display 的 PCA9557 通道：直接写扩展芯片寄存器（0x19, output port 0x01, bit2）
  // RMW 防止踩掉 LCD_CS(bit0)/PA_EN(bit1)
  Wire.beginTransmission(0x19);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)0x19, 1) != 1) return false;
  uint8_t cur = Wire.read();
  cur = level ? (cur | 0x04) : (cur & ~0x04);
  Wire.beginTransmission(0x19);
  Wire.write(0x01); Wire.write(cur);
  return Wire.endTransmission() == 0;
}

// 摄像头 SCCB 复用主 I2C（SDA=1 SCL=2）。曾试过自建 port1 legacy 总线 +
// sccb_i2c_port=1（xiaozhi 同款），但 Arduino 构建的 esp32-camera 预编译库与之
// 不兼容直接 abort——回退到 port0 复用（识别可能失败但不崩，视觉后端本就未配置）。
bool LaapVision::begin() {
  _ok = false;
  pca9557Pwdn(false);                   // 上电：PWDN 拉低退出掉电（xiaozhi SetOutputState(2,0) 同款极性）
  delay(10);
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_1;      // LEDC_CH0 归屏幕背光
  c.ledc_timer   = LEDC_TIMER_1;
  c.pin_d0 = V_PIN_D0;  c.pin_d1 = V_PIN_D1;  c.pin_d2 = V_PIN_D2;  c.pin_d3 = V_PIN_D3;
  c.pin_d4 = V_PIN_D4;  c.pin_d5 = V_PIN_D5;  c.pin_d6 = V_PIN_D6;  c.pin_d7 = V_PIN_D7;
  c.pin_xclk = V_PIN_XCLK; c.pin_pclk = V_PIN_PCLK;
  c.pin_vsync = V_PIN_VSYNC; c.pin_href = V_PIN_HREF;
  c.pin_sccb_sda = -1;                  // SCCB 复用已初始化的 I2C（识别失败不崩，见上注）
  c.pin_sccb_scl = 2;
  c.sccb_i2c_port = 0;
  c.pin_pwdn = -1; c.pin_reset = -1;    // PWDN 手动经 PCA9557
  c.xclk_freq_hz = 24000000;
  c.pixel_format = PIXFORMAT_RGB565;    // GC0308 不支持片上 JPEG（实测报错），RGB565 直出
  c.frame_size   = FRAMESIZE_QVGA;      // 320x240，RGB565=150KB/帧，PSRAM 装得下
  c.fb_count = 1;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    lastError = "camera init 0x" + String(err, HEX);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_vflip(s, 1);            // 实战派 DVP 安装方向（官方例程同款）
  _ok = true;
  Serial.println("[VISION] GC0308 就绪");
  return true;
}

// 去 chunked 传输的行间块头（纯十六进制行），与 laap_llm 同法
static String deChunk(const String& payload) {
  String clean; int pos = 0;
  while (pos < (int)payload.length()) {
    int nl = payload.indexOf('\n', pos);
    String line = (nl < 0) ? payload.substring(pos) : payload.substring(pos, nl);
    line.trim();
    bool allHex = line.length() > 0;
    for (unsigned int ci = 0; ci < line.length() && allHex; ci++)
      if (!isHexadecimalDigit(line[ci])) allHex = false;
    if (!allHex) clean += line;
    if (nl < 0) break;
    pos = nl + 1;
  }
  return clean;
}

String LaapVision::look(const String& question) {
  lastError = "";
  if (!_ok) { lastError = "摄像头未就绪"; return ""; }
  if (WiFi.status() != WL_CONNECTED) { lastError = "WiFi 未连接"; return ""; }
  // 双模式：visionBase 填了走手机桥；留空则直连 OpenAI 兼容多模态接口
  bool bridge = cfg.s.visionBase[0] != 0;
  if (!bridge && !cfg.s.visionKey[0] && !cfg.s.llmKey[0]) {
    lastError = "视觉未配置（填手机桥 URL 或直连 Key）"; return "";
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { lastError = "抓帧失败"; return ""; }
  String b64 = base64::encode(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (b64.length() > 260000) { lastError = "帧过大"; return ""; }   // body 上限保护（RGB565 QVGA≈200KB）

  String url, body, auth;                       // auth 非空则带 Bearer 头（直连用）
  if (bridge) {
    // 模式一：手机桥 {image, question} → {desc}
    url = cfg.s.visionBase;
    body.reserve(b64.length() + question.length() + 64);
    body = "{\"image\":\""; body += b64;
    body += "\",\"question\":\""; body += LlmClient::jsonEscape(question);
    body += "\"}";
  } else {
    // 模式二：直连 OpenAI 兼容多模态（OpenRouter 的 GLM-4V / Gemini Flash 等）
    url = cfg.s.visionLlmBase[0] ? cfg.s.visionLlmBase : "https://openrouter.ai/api/v1/chat/completions";
    const char* model = cfg.s.visionModel[0] ? cfg.s.visionModel : "google/gemini-flash-1.5";
    auth = String("Authorization: Bearer ") + (cfg.s.visionKey[0] ? cfg.s.visionKey : cfg.s.llmKey);
    String prompt = question.length()
      ? ("你是随身数字生命的一只眼睛，用一两句中文看图回答：" + LlmClient::jsonEscape(question))
      : String("你是随身数字生命的一只眼睛，用一两句中文简述照片里的场景。");
    body.reserve(b64.length() + prompt.length() + 400);
    body = "{\"model\":\""; body += model;
    body += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
    body += prompt;
    body += "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    body += b64;
    body += "\"}}]}],\"max_tokens\":300}";
  }

  if (!url.startsWith("http")) { lastError = "URL 无效"; return ""; }
  bool tls = url.startsWith("https");
  int dp = url.indexOf("://"), hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) { port = host.substring(host.indexOf(':') + 1).toInt(); host = host.substring(0, host.indexOf(':')); }

  WiFiClient* cli = tls ? (WiFiClient*)(new WiFiClientSecure) : new WiFiClient;
  if (tls) ((WiFiClientSecure*)cli)->setInsecure();
  cli->setTimeout(20000);
  if (!cli->connect(host.c_str(), port)) { lastError = "连接失败:" + host; delete cli; return ""; }
  cli->print(String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
             (auth.length() ? ("\r\n" + auth) : "") +
             "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
             "\r\nConnection: close\r\n\r\n" + body);
  String resp; resp.reserve(4096);
  uint32_t dl = millis() + 45000;
  while (cli->connected() && millis() < dl) {
    while (cli->available()) { resp += (char)cli->read(); if (resp.length() > 40000) break; }
    if (resp.length() > 40000) break;
    delay(2);
  }
  cli->stop(); delete cli;

  int bs = resp.indexOf("\r\n\r\n");
  String payload = (bs > 0) ? resp.substring(bs + 4) : "";
  if (resp.indexOf("chunked") >= 0) payload = deChunk(payload);
  int st = 0, sp2 = resp.indexOf(' ');
  if (sp2 > 0) st = resp.substring(sp2 + 1, sp2 + 4).toInt();
  if (st != 200) {
    String emsg;
    if (LlmClient::extractStringField(payload, "message", emsg) && emsg.length())
      lastError = "HTTP " + String(st) + ": " + emsg.substring(0, 120);
    else lastError = "HTTP " + String(st);
    return "";
  }
  String out;
  if (bridge) {
    if (!LlmClient::extractStringField(payload, "desc", out)) { lastError = "后端无 desc"; return ""; }
  } else {
    if (!LlmClient::extractStringField(payload, "content", out) || !out.length()) {
      lastError = "响应无 content"; return "";
    }
    out.trim();
  }
  _lastDesc = out;
  return out;
}

void LaapVision::logSight(const String& desc) {
  if (desc.length()) memory.logEvent("world", "看见: " + desc.substring(0, 100));
}

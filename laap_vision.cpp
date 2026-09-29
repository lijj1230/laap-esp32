#include "laap_vision.h"
#include "laap_config.h"
#include "laap_llm.h"
#include "laap_memory.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <mbedtls/base64.h>   // 直接编码进 PSRAM 缓冲（核心库的 base64::encode 只返回 String，205KB 会挤爆内部堆）

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
  c.xclk_freq_hz = 20000000;
  // 24MHz 曾导致 cam_hal 持续刷 "FB-SIZE: xxx != 153600"（DMA 欠载交付截断帧，
  // 高度只剩 50%~90%，S3 DVP+高像素时钟的经典病）；20MHz 是 esp32-camera 例程默认值，
  // GC0308 内部 PLL 自适应，帧率略降但帧完整
  c.pixel_format = PIXFORMAT_RGB565;    // GC0308 不支持片上 JPEG（实测报错），RGB565 直出
  c.frame_size   = FRAMESIZE_QVGA;      // 320x240，RGB565=150KB/帧，PSRAM 装得下（双缓冲 300KB）
  // fb_count 必须 ≥2：cam_hal 里 en=1 表示"该缓冲可被 DMA 抓取"，抓完一帧置 en=0 就
  // 没有再抓的缓冲了。fb_count=1 时驱动"取一帧→停→归还后才抓下一帧"：
  //   ① 开机后第一次取到的是 init 瞬间抓的那帧（AEC 未收敛，实测均值 28/255 全黑）；
  //   ② 之后每次取到的都是"上次取帧后立刻抓的那帧"= 上一次看的时候的画面，不是现在。
  // 双缓冲让 DMA 一直抓、队列里始终是最新帧（实测帧龄从 ~30s 降到 <100ms）。
  // 4 缓冲：DMA 欠载时多了排队余量（2 缓冲实测出现过 FB-OVF）
  c.fb_count = 4;
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

// ============================================================
//  RGB565 帧 → 标准 PNG（zlib 用 stored 块，不需要压缩库）
//  为什么不用 BMP：视觉 API 普遍只收 png/jpeg/webp/gif，BMP 会被拒
//  （实测 DeepSeek 回 "You have uploaded an unsupported image"）。GC0308 没有片上
//  JPEG 编码器，所以端侧自己拼 PNG —— stored 块虽然不压缩，但格式合法、任何后端都认。
//  代价：320x240 真彩色 ≈231KB（比 RGB565 的 205KB 大一点），全部放 PSRAM。
// ============================================================
static uint32_t crcTable[256];
static bool crcReady = false;
static void crcEnsure() {
  if (crcReady) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crcTable[n] = c;
  }
  crcReady = true;
}
static uint32_t crc32of(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  crcEnsure();
  for (size_t i = 0; i < n; i++) crc = crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}
static void putBE32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

// 生成 PNG；成功返回 PSRAM 缓冲（调用方 free），outLen 为总字节数
static uint8_t* rgb565ToPng(const uint8_t* src, int w, int h, size_t& outLen) {
  const size_t rowBytes = (size_t)w * 3 + 1;          // 每行 = 1 字节 filter(0) + RGB888
  const size_t rawLen = rowBytes * (size_t)h;
  const size_t nBlocks = rawLen / 65535 + 1;
  const size_t idatLen = 2 + rawLen + nBlocks * 5 + 4;   // zlib 头 + stored 块 + adler32
  const size_t total = 8 + 25 + (8 + idatLen + 4) + 12;
  uint8_t* buf = (uint8_t*)ps_malloc(total);
  if (!buf) return nullptr;

  uint8_t* p = buf;
  static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
  memcpy(p, sig, 8); p += 8;

  // IHDR
  putBE32(p, 13); memcpy(p + 4, "IHDR", 4);
  putBE32(p + 8, (uint32_t)w); putBE32(p + 12, (uint32_t)h);
  p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;      // 8bit 真彩色 无压缩 无隔行
  putBE32(p + 21, crc32of(p + 4, 17) ^ 0xFFFFFFFFu);
  p += 25;

  // IDAT
  putBE32(p, (uint32_t)idatLen); memcpy(p + 4, "IDAT", 4);
  uint8_t* idat = p + 8;
  idat[0] = 0x78; idat[1] = 0x01;                             // zlib: deflate, 无预设字典
  uint8_t* d = idat + 2;
  size_t rawLeft = rawLen, blockLeft = 0;
  uint32_t a = 1, b = 0;                                      // adler32（对未压缩字节流）
  // 流式写入：stored 块的边界与"行"解耦（攒满 65535 字节才开新块），于是块头数量与
  // idatLen 的估算一致。原来按行开块 = 240 个块头，而只预留了 4 个 → 越界写崩（实测重启）。
  auto emit = [&](uint8_t byte) {
    if (blockLeft == 0) {
      size_t chunk = rawLeft < 65535 ? rawLeft : 65535;
      d[0] = (chunk == rawLeft) ? 1 : 0;                      // BFINAL 仅最后一块
      d[1] = (uint8_t)chunk; d[2] = (uint8_t)(chunk >> 8);
      d[3] = (uint8_t)~d[1]; d[4] = (uint8_t)~d[2];
      d += 5; blockLeft = chunk;
    }
    *d++ = byte; rawLeft--; blockLeft--;
    a += byte; if (a >= 65521) a -= 65521;
    // b += a 最多可能到 131040（两个周期）→ 必须循环减，只减一次会让 adler32 偏大、
    // zlib 端报 "incorrect data check"（实测就是这里错了，PNG 结构本身没问题）
    b += a;    while (b >= 65521) b -= 65521;
  };
  for (int y = 0; y < h; y++) {
    emit(0);                                                  // 每行 filter=None
    const uint8_t* s = src + (size_t)y * w * 2;
    for (int x = 0; x < w; x++) {
      uint16_t v = (uint16_t)(s[1] | (s[0] << 8)); s += 2;   // GC0308 的 RGB565 是高字节在前（字节序反了会变彩虹噪点）
      uint8_t r = (uint8_t)((v >> 11) & 0x1F), g = (uint8_t)((v >> 5) & 0x3F), bl = (uint8_t)(v & 0x1F);
      emit((uint8_t)((r << 3) | (r >> 2)));
      emit((uint8_t)((g << 2) | (g >> 4)));
      emit((uint8_t)((bl << 3) | (bl >> 2)));
    }
  }
  // adler32 = (b << 16) | a，zlib 大端存放。只写 a 会让解压端报 "incorrect data check"
  { uint32_t adler = (b << 16) | a;
    d[0] = (uint8_t)(adler >> 24); d[1] = (uint8_t)(adler >> 16);
    d[2] = (uint8_t)(adler >> 8);  d[3] = (uint8_t)adler; }
  d += 4;
  putBE32(d, crc32of(idat - 4, idatLen + 4) ^ 0xFFFFFFFFu);
  p = d + 4;

  // IEND
  putBE32(p, 0); memcpy(p + 4, "IEND", 4);
  putBE32(p + 8, crc32of(p + 4, 4) ^ 0xFFFFFFFFu);
  p += 12;

  outLen = (size_t)(p - buf);
  return buf;
}

// 帧龄：这一帧是"多久以前"抓的。摄像头诊断的关键数字——大 = 拿到的是旧帧
// （fb_count=1 时曾恒等于"距上次取帧的时间"，双缓冲后应 <100ms）
static long frameAgeMs(const camera_fb_t* fb) {
  int64_t cap = (int64_t)fb->timestamp.tv_sec * 1000000 + (int64_t)fb->timestamp.tv_usec;
  int64_t age = (esp_timer_get_time() - cap) / 1000;
  return (long)(age < 0 ? 0 : age);
}

// 抓一帧"完整"的：DMA 欠载时会交付截断帧（len < 宽×高×2，cam_hal 同时刷 FB-SIZE 报错），
// 拿去编码 PNG 就是下半截垃圾 → 退回去重抓（GRAB_LATEST 总给最新帧）。重试耗尽返回 null
static camera_fb_t* grabWholeFrame(int tries) {
  for (int i = 0; i < tries; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return nullptr;
    if (fb->len >= (size_t)fb->width * fb->height * 2) return fb;
    esp_camera_fb_return(fb);
    delay(60);                          // 等下一帧进队列
  }
  return nullptr;
}

// 诊断用：抓帧 → PNG → base64（与 look 同一条链路），返回 base64 文本
String LaapVision::debugPngB64(size_t& outLen) {
  outLen = 0;
  // 与 look() 同持网络锁（v3.51 审计：本函数曾绕过模块自述的跨任务互斥——写 lastError、
  // 抓帧、分配 308KB PSRAM，与后台独白的 look 并发 = String 撕裂 + PSRAM 峰值叠加）
  if (!laapNetLock(1500)) { lastError = "视觉正忙（后台占用）"; return ""; }
  auto fail = [&](const char* m) -> String { lastError = m; laapNetUnlock(); return ""; };
  if (!_ok) return fail("摄像头未就绪");
  camera_fb_t* fb = grabWholeFrame(4);
  if (!fb) return fail("抓帧失败/连续截断");
  long ageMs = frameAgeMs(fb);
  size_t pngLen = 0;
  uint8_t* png = rgb565ToPng(fb->buf, (int)fb->width, (int)fb->height, pngLen);
  esp_camera_fb_return(fb);
  Serial.printf("[LOOKDUMP] 帧时间戳：距现在 %ld ms（越小越新）\n", ageMs);
  if (!png) return fail("PNG 生成失败");
  size_t need = 4 * ((pngLen + 2) / 3) + 8;
  char* b64 = (char*)ps_malloc(need);
  if (!b64) { free(png); return fail("PSRAM 不足"); }
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char*)b64, need, &olen, png, pngLen) != 0) {
    free(png); free(b64); return fail("base64 失败");
  }
  free(png);
  String out; out.reserve(olen + 4);
  for (size_t i = 0; i < olen; i += 512) {           // 分块 append，避免一次性大分配
    size_t n = (olen - i) < 512 ? (olen - i) : 512;
    out.concat(b64 + i, n);
  }
  free(b64);
  outLen = olen;
  laapNetUnlock();
  return out;
}

// 跨任务互斥：后台独白"起意前看一眼"与主线程的聊天/CLI 会用同一个 vision 对象
// （lastError 是 String，并发写=撕裂）。拿不到锁就当这次没看成。
String LaapVision::look(const String& question) {
  if (!laapNetLock()) { lastError = "视觉正忙（后台独白占用）"; return ""; }
  String r = lookLocked(question);
  laapNetUnlock();
  return r;
}

String LaapVision::lookLocked(const String& question) {
  lastError = "";
  if (!_ok) { lastError = "摄像头未就绪"; return ""; }
  if (WiFi.status() != WL_CONNECTED) { lastError = "WiFi 未连接"; return ""; }
  // 双模式：visionBase 填了走手机桥；留空则直连 OpenAI 兼容多模态接口
  // 直连必须显式配置 visionLlmBase（避免 LLM Key 非空就把 205KB base64 打进内部堆）
  bool bridge = cfg.s.visionBase[0] != 0;
  bool direct = cfg.s.visionLlmBase[0] != 0;
  if (!bridge && !direct) { lastError = "视觉未配置（填手机桥 URL 或直连视觉 base）"; return ""; }
  // base64 约 205KB、PNG 230KB 同时存活（峰值 ~540KB）+ 驱动 4 帧：门限按其定（v3.51 审计：
  // 原 400KB 门限过线后仍会在 b64 分配处失败，白抓一帧；旧代码只看内部堆是更早的坑）
  if (ESP.getFreePsram() < 700000) { lastError = "PSRAM 不足，跳过视觉"; return ""; }

  camera_fb_t* fb = grabWholeFrame(6);
  if (!fb) { lastError = "抓帧失败/连续截断"; return ""; }
  if (fb->format != PIXFORMAT_RGB565) { esp_camera_fb_return(fb); lastError = "像素格式非 RGB565"; return ""; }
  long ageMs = frameAgeMs(fb);          // 帧龄：这张"照片"是多久以前抓的

  // ---- 图像封装：RGB565 → 标准 PNG（BMP 会被 API 拒，实测 DeepSeek 直接 400）
  size_t pngLen = 0;
  uint8_t* raw = rgb565ToPng(fb->buf, (int)fb->width, (int)fb->height, pngLen);
  uint32_t px = fb->len;
  esp_camera_fb_return(fb);
  if (!raw) { lastError = "PSRAM 不足(PNG)"; return ""; }

  size_t need = 4 * ((pngLen + 2) / 3) + 8;
  char* b64 = (char*)ps_malloc(need);
  if (!b64) { free(raw); lastError = "PSRAM 不足(b64)"; return ""; }
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char*)b64, need, &olen, raw, pngLen) != 0) {
    free(raw); free(b64); lastError = "base64 编码失败"; return "";
  }
  free(raw);
  b64[olen] = 0;
  Serial.printf("[VISION] 帧 %ux%u %uB 帧龄 %ldms → PNG %uB → b64 %uB（heap %uKB / psram %uKB）\n",
                (unsigned)320, (unsigned)240, (unsigned)px, ageMs, (unsigned)pngLen, (unsigned)olen,
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));

  // ---- 请求体拆成 前缀 + b64 + 后缀 三段流式发出：205KB base64 绝不进 String（会挤爆内部堆）
  String url, prefix, suffix, auth;
  if (bridge) {
    // 模式一：手机桥 {image, question} → {desc}
    url = cfg.s.visionBase;
    prefix = "{\"image\":\"";
    suffix = "\",\"question\":\"" + LlmClient::jsonEscape(question) + "\"}";
  } else {
    // 模式二：直连 OpenAI 兼容多模态（Gemini Flash / GLM-4V 等）
    url = cfg.s.visionLlmBase[0] ? cfg.s.visionLlmBase : "https://openrouter.ai/api/v1/chat/completions";
    const char* model = cfg.s.visionModel[0] ? cfg.s.visionModel : "google/gemini-flash-1.5";
    auth = String("Authorization: Bearer ") + (cfg.s.visionKey[0] ? cfg.s.visionKey : cfg.s.llmKey);
    String prompt = question.length()
      ? ("你是随身数字生命的一只眼睛，用一两句中文看图回答：" + LlmClient::jsonEscape(question))
      : String("你是随身数字生命的一只眼睛，用一两句中文简述照片里的场景。");
    prefix = String("{\"model\":\"") + model +
      "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"" + prompt +
      "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,";
    suffix = "\"}}]}],\"max_tokens\":300}";
  }

  if (!url.startsWith("http")) { free(b64); lastError = "URL 无效"; return ""; }   // 补 free：曾泄漏 ~308KB/次（v3.51 审计）
  bool tls = url.startsWith("https");
  int dp = url.indexOf("://"), hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) { port = host.substring(host.indexOf(':') + 1).toInt(); host = host.substring(0, host.indexOf(':')); }

  WiFiClient* cli = tls ? (WiFiClient*)(new WiFiClientSecure) : new WiFiClient;
  if (tls) ((WiFiClientSecure*)cli)->setInsecure();
  cli->setTimeout(20000);
  if (!cli->connect(host.c_str(), port)) { lastError = "连接失败:" + host; delete cli; free(b64); return ""; }
  size_t clen = prefix.length() + olen + suffix.length();
  cli->print(String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
             (auth.length() ? ("\r\n" + auth) : "") +
             "\r\nContent-Type: application/json\r\nContent-Length: " + String((unsigned)clen) +
             "\r\nConnection: close\r\n\r\n");
  size_t sent = cli->print(prefix) + cli->print(b64) + cli->print(suffix);   // PSRAM 里 205KB 按整段发出
  free(b64);
  if (sent != clen) { lastError = "请求发送不完整（连接中断）"; cli->stop(); delete cli; return ""; }
  String hdrs, payload;
  laapHttpRead(cli, 45000, hdrs, payload, 40000);
  cli->stop(); delete cli;

  int st = 0, sp2 = hdrs.indexOf(' ');
  if (sp2 > 0) st = hdrs.substring(sp2 + 1, sp2 + 4).toInt();
  if (st != 200) {
    String emsg;
    if (LlmClient::extractStringField(payload, "message", emsg) && emsg.length())
      lastError = "HTTP " + String(st) + ": " + emsg.substring(0, 300);
    else lastError = "HTTP " + String(st);
    Serial.printf("[VISION] 失败原文: %.400s\n", payload.c_str());   // 完整错误码便于定位
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
  return out;
}

void LaapVision::logSight(const String& desc) {
  // utf8Cut：按字节 substring 会切半汉字，污染记忆文件（/api/memory 的 JSON 就是这么坏的）
  if (!desc.length()) return;
  // 独白每轮都看一眼世界，但视角基本不变（天花板/桌面）——逐条写"看见"会把后台记忆刷成
  // 相机流水账（用户实测"很频繁出现"）。只记"新景象"：语义新颖度达标才落账；
  // novelty 评估不了（熔断/无向量）时按 1 小时一条兜底。
  // 注意：只挡"记忆落账"，g_monoSight 照常注入独白上下文——看照看，只是不重复记。
  static uint32_t s_lastLog = 0;
  float nov = memory.noveltyOf(desc);
  bool fresh = (nov < 0) ? (millis() - s_lastLog > 3600000UL) : (nov > 0.25f);
  if (!fresh) return;
  s_lastLog = millis();
  memory.logEvent("world", "看见: " + utf8Cut(desc, 100));
}

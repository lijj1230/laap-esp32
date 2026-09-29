#include "laap_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static esp_lcd_panel_handle_t s_panel = nullptr;

// 颜色传输完成信号（v3.51 审计）：fillRect 复用 static 缓冲喂异步 DMA，
// 无完成同步时上一次传输仍在读缓冲、下一次已改写 → 条状错色/残影
static SemaphoreHandle_t s_lcdDone = nullptr;
static bool IRAM_ATTR lcdTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t hp = pdFALSE;
  if (s_lcdDone) xSemaphoreGiveFromISR(s_lcdDone, &hp);
  return hp == pdTRUE;
}

LaapDisplay display;

// ================= PCA9557 IO 扩展 =================
void LaapDisplay::pca9557Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(SZP_PCA9557_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// ================= ST7789 底层 =================
esp_lcd_panel_io_handle_t g_laapPanelIO = nullptr;   // begin() 里赋值（lcd 绘图命令走同一 panel_io）

void LaapDisplay::lcdCmd(uint8_t c) {
  uint8_t v = c;
  if (g_laapPanelIO) esp_lcd_panel_io_tx_param(g_laapPanelIO, 0x00, &v, 1);   // 0x00=命令
}

void LaapDisplay::lcdData(const uint8_t* d, int n) {
  if (g_laapPanelIO) esp_lcd_panel_io_tx_param(g_laapPanelIO, 0x40, d, n);    // 0x40=数据（DC 置 1 的伪命令位）
}

void LaapDisplay::setWindow(int x0, int y0, int x1, int y1) {
  uint8_t buf[4];
  lcdCmd(0x2A); // CASET
  buf[0] = x0 >> 8; buf[1] = x0 & 0xFF; buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
  lcdData(buf, 4);
  lcdCmd(0x2B); // RASET
  buf[0] = y0 >> 8; buf[1] = y0 & 0xFF; buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
  lcdData(buf, 4);
  lcdCmd(0x2C); // RAMWR
}

void LaapDisplay::fillRect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0 || x >= SZP_LCD_W || y >= SZP_LCD_H) return;
  if (x + w > SZP_LCD_W) w = SZP_LCD_W - x;
  if (y + h > SZP_LCD_H) h = SZP_LCD_H - y;
  if (!s_panel) return;
  // 按真实窗口尺寸填充缓冲（越界读会让 DMA 送出堆垃圾）
  static uint16_t buf[320 * 8];
  int xx0 = x, xx1 = x + w - 1;
  int winW = w;
  int rowsPerPass = 320 * 8 / (winW > 0 ? winW : 1);
  if (rowsPerPass < 1) rowsPerPass = 1;
  if (rowsPerPass > h) rowsPerPass = h;
  for (int i = 0; i < winW * rowsPerPass; i++) buf[i] = c;
  int yy = y;
  while (yy < y + h) {
    int rows = (y + h - yy < rowsPerPass) ? (y + h - yy) : rowsPerPass;
    // 传输前清空可能残留的信号、传输后等到完成——static 缓冲才能安全复用（v3.51 审计）
    if (s_lcdDone) while (xSemaphoreTake(s_lcdDone, 0) == pdTRUE) {}
    esp_lcd_panel_draw_bitmap(s_panel, xx0, yy, xx1 + 1, yy + rows, buf);
    if (s_lcdDone) xSemaphoreTake(s_lcdDone, pdMS_TO_TICKS(100));
    yy += rows;
  }
}

void LaapDisplay::fillCircle(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

// ST7789 完整厂商上电序列（小智/BSP 同款）：精简序列喂不醒冷态屏幕时用。
// 注意：**当前无调用者**（v3.51 审计核实）——IDF st7789 组件已含基础初始化，
// 实测屏幕工作正常；此表保留供冷启动/换屏排查时手动调用
void LaapDisplay::vendorInit() {
  lcdCmd(0xEF); { uint8_t d[] = {0x03, 0x80, 0x02}; lcdData(d, 3); }
  lcdCmd(0xCF); { uint8_t d[] = {0x00, 0xC1, 0x30}; lcdData(d, 3); }
  lcdCmd(0xED); { uint8_t d[] = {0x64, 0x03, 0x12, 0x81}; lcdData(d, 4); }
  lcdCmd(0xE8); { uint8_t d[] = {0x85, 0x00, 0x78}; lcdData(d, 3); }
  lcdCmd(0xCB); { uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; lcdData(d, 5); }
  lcdCmd(0xF7); { uint8_t d[] = {0x20}; lcdData(d, 1); }
  lcdCmd(0xEA); { uint8_t d[] = {0x00, 0x00}; lcdData(d, 2); }
  lcdCmd(0xC0); { uint8_t d[] = {0x11}; lcdData(d, 1); }   // VRH
  lcdCmd(0xC1); { uint8_t d[] = {0x20}; lcdData(d, 1); }   // Power ctrl 2
  lcdCmd(0xC5); { uint8_t d[] = {0xA0, 0x3C}; lcdData(d, 2); } // VCOM
  lcdCmd(0xC7); { uint8_t d[] = {0xB1}; lcdData(d, 1); }   // VCOMH
  lcdCmd(0xB1); { uint8_t d[] = {0x00, 0x1A}; lcdData(d, 2); } // 帧率
  lcdCmd(0xB6); { uint8_t d[] = {0x0A, 0xA2}; lcdData(d, 2); } // Display function
  lcdCmd(0x26); { uint8_t d[] = {0x01}; lcdData(d, 1); }   // Gamma set
  lcdCmd(0xE0); { uint8_t d[] = {0x0F,0x1A,0x0F,0x18,0x2F,0x28,0x20,0x22,0x1F,0x1B,0x23,0x37,0x00,0x07,0x02,0x10}; lcdData(d, 16); }
  lcdCmd(0xE1); { uint8_t d[] = {0x0F,0x1B,0x0F,0x17,0x33,0x2C,0x29,0x2E,0x30,0x30,0x39,0x3F,0x00,0x07,0x03,0x10}; lcdData(d, 16); }
}

void LaapDisplay::begin() {
  // I2C 先行：小智 Pca9557 构造同款（先写输出寄存器值，再开低3位输出）
  Wire.begin(SZP_I2C_SDA, SZP_I2C_SCL, 400000);
  pca9557Write(0x01, 0x03);   // 输出值：bit0(LCD_CS)=高=未选中, bit1(PA_EN)=1, bit2(摄像头PWDN)=0
  pca9557Write(0x03, 0xF8);   // 低3位转输出

  pinMode(SZP_LCD_DC, OUTPUT);
  pinMode(SZP_LCD_BL, OUTPUT);
  digitalWrite(SZP_LCD_BL, LOW);   // 反相：低=亮

  // esp_lcd 面板栈（xiaozhi szpi-esp32s3 完整同款：SPI3_HOST/MODE2/80MHz/ST7789 组件驱动）
  gpio_config_t io_dc = {};
  io_dc.pin_bit_mask = 1ULL << SZP_LCD_DC;
  io_dc.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io_dc);

  esp_lcd_panel_io_handle_t panel_io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;
  // SPI3 总线显式初始化（xiaozhi InitializeSpi 同款；漏了这步 panel_io 会挂不上）
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = SZP_LCD_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = SZP_LCD_CLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = SZP_LCD_W * SZP_LCD_H * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = -1;          // NC：CS 由 PCA9557 常选
  io_config.dc_gpio_num = SZP_LCD_DC;
  io_config.spi_mode = 2;
  io_config.pclk_hz = SZP_LCD_SPI_HZ;   // 常量唯一定义（v3.51：原来头文件写 40MHz、这里硬编码 80MHz）
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.on_color_trans_done = lcdTransDone;   // v3.51：传输完成回调，配合 fillRect 的等待
  io_config.user_ctx = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)SPI3_HOST, &io_config, &panel_io));
  g_laapPanelIO = panel_io;
  if (!s_lcdDone) s_lcdDone = xSemaphoreCreateBinary();

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 16;
  panel_config.flags.reset_active_high = 0;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
  s_panel = panel;

  esp_lcd_panel_reset(panel);
  // 关键一步（xiaozhi SetOutputState(0,0) 同款）：bit0(LCD_CS) 拉低选中面板，bit1(PA_EN) 保持 1。
  // 缺了这步 CS 一直为高，面板被片选隔离，所有 SPI 命令无效（无内容黑屏的根因）
  pca9557Write(0x01, 0x02);
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  esp_lcd_panel_invert_color(panel, true);   // INVON（xiaozhi 同款）
  esp_lcd_panel_swap_xy(panel, true);        // DISPLAY_SWAP_XY true
  esp_lcd_panel_mirror(panel, true, false);  // DISPLAY_MIRROR_X/Y
  esp_lcd_panel_disp_on_off(panel, true);

  { // 自检：回读 PCA9557 输出寄存器，确认 bit0(CS)=0 已选中、bit1(PA_EN)=1
    Wire.beginTransmission(SZP_PCA9557_ADDR); Wire.write(0x01); Wire.endTransmission();
    uint8_t rb = (Wire.requestFrom((int)SZP_PCA9557_ADDR, 1) == 1) ? Wire.read() : 0xFF;
    Serial.printf("[display] PCA9557 out=0x%02X (CS=%s PA_EN=%d)\n",
                  rb, (rb & 0x01) ? "HIGH未选中!" : "low已选中", (rb >> 1) & 1);
  }

  // 背光：analogWrite（3.x 内置 LEDC 后端），setBrightness 内部已做反相占空
  setBrightness(90);
  fillRect(0, 0, SZP_LCD_W, SZP_LCD_H, CLR_BG);
}
// 显示子系统自诊断：报告栈状态
String LaapDisplay::lcdDiag() {
  return "panel=" + String(s_panel ? "ok" : "null") +
         ", panelIO=" + String(g_laapPanelIO ? "ok" : "null") +
         ", brightness=" + String(brightness) +
         ", backlightPin=LOW(active)";
}

void LaapDisplay::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;      // 内部 API 自钳位（v3.51）：>100 会让 duty 变负 → uint32 巨值
  brightness = pct;
  // 实战派背光低电平点亮（反相）：亮度 pct 越大占空越小；息屏期间保持全灭
  int duty = 255 - (int)(255 * pct / 100);
  analogWrite(SZP_LCD_BL, _screenOn ? duty : 255);
}

// ================= 表情：眼睛 =================
void LaapDisplay::drawEye(int cx, int cy, int rx, int ry, int pupDx, int pupDy, int browY) {
  if (ry < 2) ry = 2;
  // 眼白（椭圆）
  for (int dy = -ry; dy <= ry; dy++) {
    float t = 1.0f - (float)dy * dy / (float)(ry * ry);
    if (t < 0) continue;
    int dx = (int)(rx * sqrtf(t));
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, CLR_EYE);
  }
  int pr = ry > 10 ? ry / 2 : 2;
  fillCircle(cx + pupDx, cy + pupDy, pr, CLR_PUPIL);
  if (browY != 0) { // 眉毛（焦虑/思考）
    fillRect(cx - rx, cy + browY - 2, rx * 2, 4, CLR_EYE);
  }
}

// ================= 布局常量（顶栏信息密集 + 大表情居中 + 底部 IP） =================
// 顶栏 0-35: 行1=时间+需求数字(彩点+%)  行2=设备信息(温度/RSSI/内存/运行时长)
// 表情区 38-198: 眼睛垂直居中 | 底栏 204-239: IP
#define UI_TOP_H     36
#define UI_FACE_TOP  40
#define UI_FACE_BOT  198
#define UI_BAR_Y     204
#define UI_BAR_H     36

void LaapDisplay::drawFace(const char* expr, bool thinking) {
  strlcpy(curExpr, expr, sizeof(curExpr));   // 息屏也要记住表情，唤醒时补画
  if (!_screenOn) return;
  int cx1 = 96, cx2 = SZP_LCD_W - 96, cy = (UI_FACE_TOP + UI_FACE_BOT) / 2;
  int rx = 44, ry = 30, pdx = 0, pdy = 0, brow = 0;
  String e(expr); e.toLowerCase();
  if (e == "happy")   { ry = 20; pdy = 6; }
  else if (e == "curious")  { rx = 36; ry = 36; pdx = 6; pdy = -6; }
  else if (e == "excited")  { rx = 48; ry = 40; }
  else if (e == "lonely")   { ry = 24; pdy = 8; brow = 0; }
  else if (e == "anxious")  { ry = 27; brow = -(ry + 10); }
  else if (e == "tired")    { ry = 11; pdy = 2; }
  else                     { rx = 42; ry = 30; } // calm
  fillRect(0, UI_FACE_TOP, SZP_LCD_W, UI_FACE_BOT - UI_FACE_TOP, CLR_BG);
  if (thinking) brow = -(ry + 12);
  drawEye(cx1, cy, rx, ry, pdx, pdy, brow);
  drawEye(cx2, cy, rx, ry, pdx, pdy, brow);
  drawMoodDot();     // 底栏情绪点跟随表情（不依赖"下次画 IP 栏"才更新）
}

void LaapDisplay::blinkTick() {
  if (!_screenOn) return;
  uint32_t now = millis();
  if (blinking) {
    if (now - lastBlink > 60) {
      lastBlink = now;
      blinkPhase++;
      int ry = 34 * (blinkPhase < 4 ? blinkPhase : 7 - blinkPhase + 1) / 4;
      String e(curExpr);
      int rx = 44, ryBase = 32;
      if (e == "happy") ryBase = 22; else if (e == "tired") ryBase = 12;
      else if (e == "excited") { rx = 52; ryBase = 46; }
      else if (e == "curious") { rx = 38; ryBase = 40; }
      int ry2 = (int)(ryBase * ry / 34); if (ry2 < 2) ry2 = 2;
      fillRect(0, UI_FACE_TOP, SZP_LCD_W, UI_FACE_BOT - UI_FACE_TOP, CLR_BG);
      drawEye(96, (UI_FACE_TOP + UI_FACE_BOT) / 2, rx, ry2, 0, 0, 0);
      drawEye(SZP_LCD_W - 96, (UI_FACE_TOP + UI_FACE_BOT) / 2, rx, ry2, 0, 0, 0);
      if (blinkPhase >= 8) { blinking = false; drawFace(curExpr, false); }
    }
  } else if (now - lastBlink > 4000 + (esp_random() % 4000)) {
    blinking = true; blinkPhase = 0; lastBlink = now;
  }
}

// 屏幕开关：关=背光灭（面板内容保留，不必整屏重画）
void LaapDisplay::setScreenOn(bool on) {
  if (_screenOn == on) return;
  _screenOn = on;
  if (on) {
    setBrightness(brightness);     // 恢复用户设定亮度（setBrightness 内部按 _screenOn 决定占空）
    repaint();                     // 息屏期间表情/顶栏变化补画一帧
  } else {
    analogWrite(SZP_LCD_BL, 255);  // 反相背光：255 占空=最暗
  }
}

void LaapDisplay::repaint() {
  if (!_screenOn) return;
  drawFace(curExpr, false);
  drawTopStrip();
  // 底栏必须一起补：扣翻装死是整屏 clear，翻回来只画脸+顶栏会让 IP 栏一直是黑的
  if (ipDrawn) drawIpLine(ipCache, ipWifiOk);
}

void LaapDisplay::drawListenState(bool listening) {
  int8_t want = listening ? 1 : -1;   // 不在听=不显示（原来恒画灰点，看着像一直在收声）
  if (_recOn) { listenDot = want; return; }   // 录音标记占用右上角：只记账不画，退出时恢复
  if (want == listenDot) return;
  listenDot = want;
  fillRect(SZP_LCD_W - 18, 1, 18, 17, CLR_BG);       // 只清右上角这一小块
  if (listenDot > 0) fillCircle(SZP_LCD_W - 8, 9, 4, CLR_EXP);
}

// 录音中标记（红点）：录音收音期间右上角亮红点，说完自动熄。息屏时画了也看不见，
// 唤醒的 repaint 会盖掉它——可接受（听到人声即刻 laapActivity 点亮，红点只在亮屏时有意义）
void LaapDisplay::drawRecState(bool on) {
  if (on == _recOn) return;
  _recOn = on;
  fillRect(SZP_LCD_W - 18, 1, 18, 17, CLR_BG);
  if (_recOn) fillCircle(SZP_LCD_W - 8, 9, 4, CLR_REC);
  else if (listenDot > 0) fillCircle(SZP_LCD_W - 8, 9, 4, CLR_EXP);   // 恢复聆听点（VAD 模式录音时它本来亮着）
}

void LaapDisplay::thinkingPulse() {
  if (!_screenOn) return;
  thinkStep = (thinkStep + 1) % 6;
  int x = 134 + thinkStep * 9;
  int y = UI_FACE_BOT - 24;           // 表情区底部（眼下）
  fillRect(134, y, 54, 4, CLR_BG);
  fillRect(x, y, 6, 4, CLR_DIM);
}

// ================= 3x5 微点阵（数字 + 设备信息所需字符） =================
static const uint8_t FONT3x5[10][5] = {
  {0x07,0x05,0x05,0x05,0x07}, // 0
  {0x02,0x06,0x02,0x02,0x07}, // 1
  {0x07,0x01,0x07,0x04,0x07}, // 2
  {0x07,0x01,0x07,0x01,0x07}, // 3
  {0x05,0x05,0x07,0x01,0x01}, // 4
  {0x07,0x04,0x07,0x01,0x07}, // 5
  {0x07,0x04,0x07,0x05,0x07}, // 6
  {0x07,0x01,0x02,0x02,0x02}, // 7
  {0x07,0x05,0x07,0x05,0x07}, // 8
  {0x07,0x05,0x07,0x01,0x07}, // 9
};
// 字母/符号字形（键: 字符 → 5 行 3bit）
static const struct { char ch; uint8_t g[5]; } GLYPH3x5[] = {
  {':', {0x00,0x02,0x00,0x02,0x00}},
  {'.', {0x00,0x00,0x00,0x00,0x02}},
  {'-', {0x00,0x00,0x07,0x00,0x00}},
  {'C', {0x07,0x04,0x04,0x04,0x07}},
  {'K', {0x05,0x05,0x06,0x05,0x05}},
  {'B', {0x06,0x05,0x06,0x05,0x06}},
  {'d', {0x01,0x01,0x07,0x05,0x07}},
  {'m', {0x00,0x07,0x05,0x05,0x05}},
  {'h', {0x04,0x04,0x07,0x05,0x05}},
  {'g', {0x07,0x05,0x07,0x01,0x06}},   // IMU 读数单位
};

// scale=1 时 3x5 像素；scale=2/3 时每点放大成方块（顶栏用 2~3 倍，原 3x5 太小看不清）
void LaapDisplay::drawGlyph3x5(int x, int y, char ch, uint16_t c, uint8_t scale) {
  const uint8_t* g = nullptr;
  if (ch >= '0' && ch <= '9') g = FONT3x5[ch - '0'];
  else {
    for (auto& e : GLYPH3x5) if (e.ch == ch) { g = e.g; break; }
  }
  if (!g) return;
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 5; row++)
      if (g[row] & (0x04 >> col))
        fillRect(x + col * scale, y + row * scale, scale, scale, c);
}

int LaapDisplay::drawText3x5(int x, int y, const char* s, uint16_t c, uint8_t scale) {
  for (; *s; s++) {
    drawGlyph3x5(x, y, *s, c, scale);
    x += (int)(((*s == '.') ? 3 : 4) * scale);
  }
  return x;
}

// ================= 顶栏（时间 + 需求数字 + 设备信息 + 聆听点） =================
// 版面：行1 时间(3x 放大) + 右侧五个需求数字(2x)；行2 设备信息(2x)。
// 需求数字按固定 38px 槽位排布，数字位数变化时不会左右抖动。
void LaapDisplay::drawTopStrip() {
  if (!_screenOn) return;
  fillRect(0, 0, SZP_LCD_W, UI_TOP_H, CLR_BG);
  fillRect(0, UI_TOP_H - 1, SZP_LCD_W, 1, CLR_DIM);   // 分隔线

  const uint16_t tc = CLR_TIME;   // 暖米灰（原近白蓝，深底上偏刺眼）
  const uint16_t dc = CLR_META;   // 冷灰蓝

  // ---- 行1 (y=1): 时间 3x 放大（9x15 像素，远处也看得清） ----
  time_t nowT = time(nullptr);
  if (nowT > 1700000000) {
    struct tm t;
    localtime_r(&nowT, &t);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.tm_hour, t.tm_min);
    drawText3x5(6, 1, tbuf, tc, 3);
  } else {
    drawText3x5(6, 1, "--:--", dc, 3);
  }
  // 需求数字：彩点 + 百分数（能量/好奇/社交/安全/表达）
  const uint16_t colors[5] = {CLR_ENE, CLR_CUR, CLR_SOC, CLR_SEC, CLR_EXP};
  int nx = 74;
  for (int i = 0; i < 5; i++) {
    fillCircle(nx + 3, 9, 3, colors[i]);
    char nbuf[6];
    snprintf(nbuf, sizeof(nbuf), "%d", needPct[i]);
    drawText3x5(nx + 9, 4, nbuf, colors[i], 2);
    nx += 38;
  }
  // 聆听点（右上角，仅真正在听时出现——原来常亮会让人以为一直在收声）
  if (listenDot > 0) fillCircle(SZP_LCD_W - 8, 9, 4, CLR_EXP);

  // ---- 行2 (y=20, 2x): 左 体温/信号/堆 ｜ 中 运行时长 ｜ 右 IMU 加速度模值 ----
  char buf[48];
  auto tw = [](const char* s, uint8_t sc) {          // 文本像素宽（'.' 窄一档）
    int w = 0; for (; *s; s++) w += ((*s == '.') ? 3 : 4) * sc; return w;
  };
  snprintf(buf, sizeof(buf), "%dC %ddB %uKB", (int)devTemp, devRssi, (unsigned)devHeapKb);
  int x2 = drawText3x5(6, 20, buf, dc, 2) + 10;   // 按实际宽度接排：左段位数会变（信号 -100、堆 5 位）
  snprintf(buf, sizeof(buf), "%uh%02um", (unsigned)(devUpMin / 60), (unsigned)(devUpMin % 60));
  drawText3x5(x2, 20, buf, dc, 2);
  if (devAccel > 0) {                                 // IMU 读数：静止 1.00g，晃动 1.5~3g
    snprintf(buf, sizeof(buf), "%.2fg", devAccel);
    drawText3x5(296 - tw(buf, 2), 20, buf, CLR_META, 2);   // 右对齐（数值跳动不左右抖）
  }
}

void LaapDisplay::drawStatusLine(float tempC, int rssi, uint32_t heapKb, uint32_t upMin, float accelG) {
  devTemp = tempC; devRssi = rssi; devHeapKb = heapKb; devUpMin = upMin; devAccel = accelG;
  drawTopStrip();
}

// ================= 需求 → 顶栏数字 =================
void LaapDisplay::drawNeeds(float energy, float curiosity, float social, float security, float expression) {
  float v[5] = {energy, curiosity, social, security, expression};
  for (int i = 0; i < 5; i++) {
    if (!isfinite(v[i])) v[i] = 0;   // NaN 直通比较会得到未定义 int（v3.51）
    int p = (int)((v[i] < 0 ? 0 : (v[i] > 1 ? 1 : v[i])) * 100);
    if (p != needPct[i]) { needPct[i] = p; }
  }
  drawTopStrip();
}

// ================= 7 段数码 IP 行 =================
// 段布局:  a
//        f   b
//          g
//        e   c
//          d
static const uint8_t SEG7[16] = { // 0-9
  0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F
};

void LaapDisplay::draw7seg(int x, int y, char ch, uint16_t c) {
  int w = 10, h = 18, t = 3;
  if (ch >= '0' && ch <= '9') {
    uint8_t s = SEG7[ch - '0'];
    if (s & 0x01) fillRect(x + t, y, w - 2*t, t, c);              // a
    if (s & 0x02) fillRect(x + w - t, y, t, h/2, c);              // b
    if (s & 0x04) fillRect(x + w - t, y + h/2, t, h/2, c);        // c
    if (s & 0x08) fillRect(x + t, y + h - t, w - 2*t, t, c);      // d
    if (s & 0x10) fillRect(x, y + h/2, t, h/2, c);                // e
    if (s & 0x20) fillRect(x, y, t, h/2, c);                      // f
    if (s & 0x40) fillRect(x + t, y + h/2 - t/2, w - 2*t, t, c);  // g
  } else if (ch == '.') {
    fillRect(x + t, y + h - t, w - 2*t, t, c);
  } else if (ch == '-') {
    fillRect(x + t, y + h/2 - t/2, w - 2*t, t, c);
  }
}

void LaapDisplay::drawIp7seg(int x, int y, const String& s) {
  int cx = x;
  for (unsigned int i = 0; i < s.length(); i++) {
    draw7seg(cx, y, s[i], CLR_TXT);
    cx += (s[i] == '.') ? 6 : 14;
  }
}

void LaapDisplay::drawIpLine(const String& ip, bool wifiOk) {
  ipCache = ip; ipWifiOk = wifiOk; ipDrawn = true;   // 先缓存：息屏/装死期间也能在恢复时补画
  if (!_screenOn) return;
  // 底栏：分隔线 + 天线/感叹号 + IP（7段数码），UI_BAR_H=36 内布局
  fillRect(0, UI_BAR_Y - 2, SZP_LCD_W, UI_BAR_H + 2, CLR_BG);
  fillRect(0, UI_BAR_Y - 2, SZP_LCD_W, 1, CLR_DIM);   // 分隔线
  if (wifiOk) {
    // 小天线图标
    fillRect(8, UI_BAR_Y + 30, 2, 4, CLR_EXP);
    fillRect(11, UI_BAR_Y + 27, 2, 7, CLR_EXP);
    fillRect(14, UI_BAR_Y + 24, 2, 10, CLR_EXP);
    drawIp7seg(22, UI_BAR_Y + 14, ip);
  } else {
    // 感叹号
    fillRect(10, UI_BAR_Y + 16, 4, 8, RGB565(255, 80, 80));
    fillRect(10, UI_BAR_Y + 26, 4, 2, RGB565(255, 80, 80));
    drawIp7seg(22, UI_BAR_Y + 14, String("--.--.-.-"));
  }
  drawMoodDot();   // 右侧情绪色点（当前表情对应色，随表情实时变化）
}

// 底栏右侧的情绪色点：颜色 = 当前表情。drawFace 每次换表情都会调它，
// 所以它不再是"开机画一次、之后一直不变"的老色点（用户会当成神秘红点）。
void LaapDisplay::drawMoodDot() {
  if (!_screenOn) return;
  String e(curExpr); e.toLowerCase();
  uint16_t mc = CLR_TXT;                                   // 平静/未知 = 近白
  if (e == "happy" || e == "excited") mc = CLR_EXP;        // 豆绿
  else if (e == "curious") mc = CLR_CUR;                   // 雾蓝
  else if (e == "anxious") mc = CLR_SEC;                   // 灰蓝
  else if (e == "tired") mc = CLR_DIM;                     // 暗灰
  else if (e == "lonely") mc = CLR_SOC;                    // 藕粉（偏红）
  fillRect(SZP_LCD_W - 22, UI_BAR_Y + 12, 12, 12, CLR_BG); // 先擦，换色不留残影
  fillCircle(SZP_LCD_W - 16, UI_BAR_Y + 18, 4, mc);
}

String LaapDisplay::ipBarDiag() {
  String e(curExpr); e.toLowerCase();
  const char* moodCn = "平静(近白)";
  if (e == "happy") moodCn = "开心(豆绿)"; else if (e == "excited") moodCn = "兴奋(豆绿)";
  else if (e == "curious") moodCn = "好奇(雾蓝)"; else if (e == "anxious") moodCn = "焦虑(灰蓝)";
  else if (e == "tired") moodCn = "疲惫(暗灰)"; else if (e == "lonely") moodCn = "孤单(藕粉/偏红)";
  return String("IP=\"") + ipCache + "\" 联网=" + (ipWifiOk ? "是(左侧绿天线)" : "否(左侧红感叹号)") +
         " 已画=" + (ipDrawn ? "是" : "否") + " | 当前表情=" + curExpr +
         " | 右侧情绪点=" + moodCn + (devAccel > 0 ? "" : " ");
}

void LaapDisplay::drawBootScreen() {
  fillRect(0, 0, SZP_LCD_W, SZP_LCD_H, CLR_BG);
  drawFace("calm", true);
  // 表情区下部三个色块（LAAP 色标）
  for (int i = 0; i < 3; i++)
    fillRect(126 + i * 26, UI_FACE_BOT - 26, 18, 18, i == 0 ? CLR_CUR : (i == 1 ? CLR_SOC : CLR_EXP));
}

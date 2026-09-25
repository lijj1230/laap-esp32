#include "laap_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"

static esp_lcd_panel_handle_t s_panel = nullptr;

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
    esp_lcd_panel_draw_bitmap(s_panel, xx0, yy, xx1 + 1, yy + rows, buf);
    yy += rows;
  }
}

void LaapDisplay::fillCircle(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

// ST7789 完整厂商上电序列（小智/BSP 同款）：精简序列喂不醒冷态屏幕时，
// 电源(PWRCTRL/VCOM)/伽马/门驱动这些寄存器是必需的——实测冷启动黑屏的根治项
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
  io_config.pclk_hz = 80 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.on_color_trans_done = nullptr;
  io_config.user_ctx = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)SPI3_HOST, &io_config, &panel_io));
  g_laapPanelIO = panel_io;

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
  brightness = pct;
  // 实战派背光低电平点亮（反相）：亮度 pct 越大占空越小
  int duty = 255 - (int)(255 * pct / 100);
  analogWrite(SZP_LCD_BL, duty);
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

void LaapDisplay::drawFace(const char* expr, bool thinking) {
  strlcpy(curExpr, expr, sizeof(curExpr));
  int cx1 = 96, cx2 = SZP_LCD_W - 96, cy = 66;
  int rx = 46, ry = 34, pdx = 0, pdy = 0, brow = 0;
  String e(expr); e.toLowerCase();
  if (e == "happy")   { ry = 22; pdy = 6; }
  else if (e == "curious")  { rx = 38; ry = 40; pdx = 6; pdy = -6; }
  else if (e == "excited")  { rx = 52; ry = 46; }
  else if (e == "lonely")   { ry = 26; pdy = 8; brow = 0; }
  else if (e == "anxious")  { ry = 30; brow = -(ry + 10); }
  else if (e == "tired")    { ry = 12; pdy = 2; }
  else                     { rx = 44; ry = 32; } // calm
  fillRect(0, 0, SZP_LCD_W, 148, CLR_BG);
  if (thinking) brow = -(ry + 12);
  drawEye(cx1, cy, rx, ry, pdx, pdy, brow);
  drawEye(cx2, cy, rx, ry, pdx, pdy, brow);
}

void LaapDisplay::blinkTick() {
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
      fillRect(0, 0, SZP_LCD_W, 148, CLR_BG);
      drawEye(96, 66, rx, ry2, 0, 0, 0);
      drawEye(SZP_LCD_W - 96, 66, rx, ry2, 0, 0, 0);
      if (blinkPhase >= 8) { blinking = false; drawFace(curExpr, false); }
    }
  } else if (now - lastBlink > 4000 + (esp_random() % 4000)) {
    blinking = true; blinkPhase = 0; lastBlink = now;
  }
}

void LaapDisplay::drawListenState(bool listening) {
  // 需求条区域右上角一个小圆点：绿=在听 灰=暂停
  fillRect(304, 154, 12, 12, CLR_BG);
  fillCircle(310, 160, 4, listening ? CLR_EXP : CLR_DIM);
}

void LaapDisplay::thinkingPulse() {
  thinkStep = (thinkStep + 1) % 6;
  int x = 148 + thinkStep * 8;
  fillRect(148, 132, 50, 3, CLR_BG);
  fillRect(x, 132, 6, 3, CLR_DIM);
}

// ================= 需求条 =================
void LaapDisplay::drawNeeds(float energy, float curiosity, float social, float security, float expression) {
  const int bx = 26, bw = SZP_LCD_W / 2 - bx - 20, bh = 14, gap = 22;
  const int rows[5] = {0, 1, 0, 1, 0}; // 左右两列? 简化为 5 行单列过窄 → 两列
  // 两列布局: 左列3条 右列2条
  struct { float v; uint16_t c; int col; int row; } bars[5] = {
    {energy,    CLR_ENE, 0, 0},
    {curiosity, CLR_CUR, 0, 1},
    {social,    CLR_SOC, 0, 2},
    {security,  CLR_SEC, 1, 0},
    {expression,CLR_EXP, 1, 1},
  };
  fillRect(0, 150, SZP_LCD_W, 78, CLR_BG);
  for (auto& b : bars) {
    int x = bx + b.col * (SZP_LCD_W / 2);
    int y = 154 + b.row * (bh + 8);
    fillRect(x, y, bw, bh, CLR_DIM);
    int fw = (int)(bw * (b.v > 1 ? 1 : b.v));
    if (fw > 0) fillRect(x, y, fw, bh, b.c);
  }
  (void)rows;
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
  fillRect(0, 214, SZP_LCD_W, 26, CLR_BG);
  if (wifiOk) {
    // 小天线图标
    fillRect(8, 228, 2, 4, CLR_EXP);
    fillRect(11, 225, 2, 7, CLR_EXP);
    fillRect(14, 222, 2, 10, CLR_EXP);
    drawIp7seg(22, 216, ip);
  } else {
    // 感叹号
    fillRect(10, 218, 4, 8, RGB565(255, 80, 80));
    fillRect(10, 228, 4, 2, RGB565(255, 80, 80));
    drawIp7seg(22, 216, String("--.--.-.-"));
  }
}

void LaapDisplay::drawBootScreen() {
  fillRect(0, 0, SZP_LCD_W, SZP_LCD_H, CLR_BG);
  drawFace("calm", true);
  // LAAP 字样用色块抽象表达：三个渐变方块
  for (int i = 0; i < 3; i++)
    fillRect(120 + i * 30, 190, 20, 20, i == 0 ? CLR_CUR : (i == 1 ? CLR_SOC : CLR_EXP));
}

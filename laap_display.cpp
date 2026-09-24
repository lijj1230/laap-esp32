#include "laap_display.h"

LaapDisplay display;

// ================= PCA9557 IO 扩展 =================
void LaapDisplay::pca9557Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(SZP_PCA9557_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// ================= ST7789 底层 =================
void LaapDisplay::lcdCmd(uint8_t c) {
  digitalWrite(SZP_LCD_DC, LOW);
  SPI.beginTransaction(SPISettings(SZP_LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  SPI.transfer(c);
  SPI.endTransaction();
}

void LaapDisplay::lcdData(const uint8_t* d, int n) {
  digitalWrite(SZP_LCD_DC, HIGH);
  SPI.beginTransaction(SPISettings(SZP_LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  SPI.transferBytes(d, nullptr, n);
  SPI.endTransaction();
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
  setWindow(x, y, x + w - 1, y + h - 1);
  digitalWrite(SZP_LCD_DC, HIGH);
  SPI.beginTransaction(SPISettings(SZP_LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
  size_t total = (size_t)w * h;
  const int CHUNK = 1024;
  static uint8_t line[CHUNK * 2];
  for (int i = 0; i < CHUNK; i++) { line[i*2] = c >> 8; line[i*2+1] = c & 0xFF; }
  while (total > 0) {
    int n = total > CHUNK ? CHUNK : total;
    SPI.transferBytes(line, nullptr, n * 2);
    total -= n;
  }
  SPI.endTransaction();
}

void LaapDisplay::fillCircle(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)sqrtf((float)(r * r - dy * dy));
    fillRect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
  }
}

void LaapDisplay::begin() {
  // I2C 先行：把 PCA9557 的 LCD_CS(bit0) 拉低永久选中
  Wire.begin(SZP_I2C_SDA, SZP_I2C_SCL, 400000);
  pca9557Write(0x03, 0xFC); // bit0(LCD_CS)/bit1(PA_EN) 输出，其余输入
  pca9557Write(0x01, 0x00); // LCD_CS=0 选中, PA_EN=0

  pinMode(SZP_LCD_DC, OUTPUT);
  pinMode(SZP_LCD_BL, OUTPUT);
  SPI.begin(SZP_LCD_CLK, -1, SZP_LCD_MOSI, -1);

  lcdCmd(0x01); // SWRESET
  delay(150);
  lcdCmd(0x11); // SLPOUT
  delay(120);
  lcdCmd(0x3A); uint8_t m = 0x55; lcdData(&m, 1); // 16bit
  lcdCmd(0x36); m = 0x60; lcdData(&m, 1);         // MADCTL: MV|MX 横屏(官方同款)
  lcdCmd(0x21); // INVON 反色(官方同款)
  lcdCmd(0x13); // NORON
  lcdCmd(0x29); // DISPON
  setBrightness(90);
  fillRect(0, 0, SZP_LCD_W, SZP_LCD_H, CLR_BG);
}

void LaapDisplay::setBrightness(uint8_t pct) {
  brightness = pct;
  analogWrite(SZP_LCD_BL, (int)(255 * pct / 100));
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

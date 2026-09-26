#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

// ===== 立创·实战派 ESP32-S3 板载 ST7789 320x240 屏 =====
// 官方 BSP: MOSI=40 CLK=41 DC=39 BL=42, CS 在 PCA9557(IO扩展,0x19) bit0,
//          I2C SDA=1 SCL=2; 屏幕方向: MADCTL=0x60(MV|MX) + 反色
#define SZP_I2C_SDA      1
#define SZP_I2C_SCL      2
#define SZP_PCA9557_ADDR 0x19
#define SZP_LCD_MOSI     40
#define SZP_LCD_CLK      41
#define SZP_LCD_DC       39
#define SZP_LCD_BL       42
#define SZP_LCD_W        320
#define SZP_LCD_H        240
#define SZP_LCD_SPI_HZ   40000000

// RGB565 常用色
#define RGB565(r,g,b) ((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3))
#define CLR_BLACK 0x0000
#define CLR_WHITE 0xFFFF
#define CLR_BG    RGB565(12, 14, 24)
#define CLR_EYE   RGB565(120, 220, 255)
#define CLR_PUPIL RGB565(8, 10, 18)
#define CLR_DIM   RGB565(60, 70, 90)
#define CLR_TXT   RGB565(200, 210, 225)
// 需求条莫兰迪柔和色（低饱和，深底不刺眼）
#define CLR_ENE   RGB565(224, 158, 106)   // 能量-暖杏
#define CLR_CUR   RGB565(118, 178, 205)   // 好奇-雾蓝
#define CLR_SOC   RGB565(198, 134, 170)   // 社交-藕粉
#define CLR_SEC   RGB565(128, 158, 200)   // 安全-灰蓝
#define CLR_EXP   RGB565(136, 186, 140)   // 表达-豆绿
// 顶栏文字（低饱和莫兰迪灰：柔和但必须看得清——第一版 132,138,152 实测"太淡"）
#define CLR_TIME  RGB565(214, 208, 192)   // 时间-暖米灰（主信息）
#define CLR_META  RGB565(164, 170, 184)   // 状态指标-冷灰蓝（次信息）
#define CLR_REC   RGB565(232, 96, 84)     // 录音中标记-珊瑚红（一眼可见）

class LaapDisplay {
public:
  void begin();
  void setBrightness(uint8_t pct);
  uint8_t getBrightness() const { return brightness; }
  String lcdDiag();            // 显示子系统自诊断（串口 /lcd 调用）
  String ipBarDiag();          // 底栏现状（IP/联网/情绪色点）自诊断（串口 /ipbar 调用）
  void clear(uint16_t c) { fillRect(0, 0, SZP_LCD_W, SZP_LCD_H, c); }
  // ---- UI ----
  void drawFace(const char* expr, bool thinking = false);
  void drawNeeds(float energy, float curiosity, float social, float security, float expression);
  void drawStatusLine(float tempC, int rssi, uint32_t heapKb, uint32_t upMin, float accelG);   // 顶栏刷新（时间/需求/设备/IMU）
  void drawIpLine(const String& ip, bool wifiOk);
  void drawBootScreen();
  void blinkTick();                       // loop 里调用，眨眼动画
  void drawListenState(bool listening);   // 屏角聆听状态点（只在真正聆听时显示）
  void drawRecState(bool on);             // 录音中标记（红点，与聆听点同用右上角，互斥）
  void thinkingPulse();                   // “思考中”动画步进
  void setScreenOn(bool on);              // 静默息屏（关背光，屏内容保留）
  bool screenOn() const { return _screenOn; }
  void repaint();                         // 重画整屏（唤醒/装死后恢复：表情 + 顶栏）
private:
  // ---- 底层 ST7789 ----
  void lcdCmd(uint8_t c);
  void lcdData(const uint8_t* d, int n);
  void setWindow(int x0, int y0, int x1, int y1);
  void fillRect(int x, int y, int w, int h, uint16_t c);
  void fillCircle(int cx, int cy, int r, uint16_t c);
  void pca9557Write(uint8_t reg, uint8_t val);
  void vendorInit();   // ST7789 完整厂商上电序列（冷态屏必需）
  // ---- UI helpers ----
  void drawEye(int cx, int cy, int rx, int ry, int pupDx, int pupDy, int browY);
  void drawGlyph3x5(int x, int y, char ch, uint16_t c, uint8_t scale);  // 3x5 点阵字符（scale 倍放大）
  int  drawText3x5(int x, int y, const char* s, uint16_t c, uint8_t scale);
  void drawTopStrip();                                    // 顶栏：时间+需求数字+设备信息
  void drawMoodDot();                                     // 底栏右侧情绪色点（跟随当前表情实时更新）
  void draw7seg(int x, int y, char ch, uint16_t c);
  void drawIp7seg(int x, int y, const String& s);
  uint8_t brightness = 220;
  bool _screenOn = true;              // 息屏期间跳过全部绘图（背光已关，画面不刷新）
  uint32_t lastBlink = 0; bool blinking = false; uint8_t blinkPhase = 0;
  uint8_t thinkStep = 0;
  char curExpr[16] = "calm";
  // 顶栏数据缓存
  uint8_t needPct[5] = {0,0,0,0,0};
  int8_t listenDot = -1;              // -1=不显示 0=灰 1=绿
  bool _recOn = false;                // 录音标记占用右上角时，聆听点只记账不画
  float devTemp = 0; int devRssi = 0; uint32_t devHeapKb = 0, devUpMin = 0;
  float devAccel = -1;                // IMU 加速度模值(g)，<0=不显示（IMU 缺席/无效）
  String ipCache; bool ipWifiOk = false; bool ipDrawn = false;   // 底栏缓存：息屏/装死后重画用
};

extern LaapDisplay display;

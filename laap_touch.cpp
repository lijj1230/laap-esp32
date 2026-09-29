#include "laap_touch.h"
#include <Wire.h>

static bool s_ok = false;

static bool rd(uint8_t reg, uint8_t* buf, int n) {
  Wire.beginTransmission(LTP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)LTP_ADDR, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();   // 必须读完，否则残留数据污染下次读取
  // 不再补 endTransmission(true)：requestFrom 内部已释放 Wire 互斥并收尾——
  // 多打这一发会对信号量二次 Give（互斥失效）+ 每 10ms 给 FT6236 一个空写事务（v3.51 审计）
  return true;
}

static bool wr(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(LTP_ADDR);
  Wire.write(reg); Wire.write(v);
  return Wire.endTransmission() == 0;
}

bool touchInit() {
  uint8_t b = 0;
  if (!rd(0x02, &b, 1)) { s_ok = false; return false; }   // 不应答=没有触摸芯片
  uint8_t mode = 0xFF;
  rd(0x00, &mode, 1);
  if (mode != 0) wr(0x00, 0);            // 置工作模式（monitor/待机模式不报触点）
  wr(0x80, 20);                          // 触摸阈值（默认偏钝，压低一点更灵敏）
  s_ok = true;
  return true;
}

bool touchPresent() { return s_ok; }

bool touchRead(int& x, int& y) {
  if (!s_ok) return false;
  uint8_t st[5];
  if (!rd(0x02, st, 5)) return false;
  if ((st[0] & 0x0F) == 0) return false;          // 无触点
  if (((st[1] >> 6) & 0x03) == 1) return false;   // 抬起事件：不算一次新触摸
  x = ((st[1] & 0x0F) << 8) | st[2];
  y = ((st[3] & 0x0F) << 8) | st[4];
  return true;
}

void touchReadRaw(uint8_t* buf5) {
  if (!s_ok || !rd(0x02, buf5, 5)) for (int i = 0; i < 5; i++) buf5[i] = 0xFF;
}

uint8_t touchReg(uint8_t reg) {
  uint8_t v = 0xFF;
  if (s_ok) rd(reg, &v, 1);
  return v;
}

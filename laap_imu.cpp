#include "laap_imu.h"
#include "laap_display.h" // 复用已初始化的 Wire（SDA=1 SCL=2）
#include <math.h>

#define QMI_ADDR 0x6A
#define REG_WHO_AM_I 0x00
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03
#define REG_CTRL3    0x04
#define REG_CTRL7    0x08
#define REG_RESET    0x60
#define REG_AX_L     0x35

static bool g_ok = false;
static float g_baseline = 1.0f; // 静止时 |acc| ≈ 1g

static void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

static int rd(uint8_t reg, uint8_t* buf, int n) {
  Wire.beginTransmission(QMI_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)QMI_ADDR, n) != n) return -2;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();   // 必须读出：否则残留数据污染下次读取
  Wire.endTransmission(true);
  return 0;
}

bool imuInit() {
  uint8_t id = 0;
  for (int i = 0; i < 3; i++) {
    if (rd(REG_WHO_AM_I, &id, 1) == 0 && id == 0x05) break;
    delay(30);
  }
  if (id != 0x05) return false;
  wr(REG_RESET, 0xB0); delay(20);
  wr(REG_CTRL1, 0x40);       // 地址自增
  wr(REG_CTRL7, 0x03);       // 使能加速度+陀螺仪
  wr(REG_CTRL2, 0x95);       // ACC 4g 250Hz
  wr(REG_CTRL3, 0xD5);       // GYR 512dps 250Hz
  delay(30);
  uint8_t chk = 0;
  if (rd(REG_CTRL7, &chk, 1) == 0 && chk != 0x03) {
    Serial.printf("[IMU] CTRL7 回读 0x%02X != 0x03，配置未生效\n", chk);
    return false;
  }
  g_ok = true;
  return true;
}

float imuMotionLevel() {
  if (!g_ok) return 0;
  uint8_t b[6];
  if (rd(REG_AX_L, b, 6) != 0) return 0;
  float x = (int16_t)(b[0] | (b[1] << 8)) / 8192.0f; // ±4g
  float y = (int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
  float z = (int16_t)(b[4] | (b[5] << 8)) / 8192.0f;
  float mag = sqrtf(x * x + y * y + z * z);
  // 慢自适应基线
  g_baseline = g_baseline * 0.98f + mag * 0.02f;
  float dev = fabsf(mag - g_baseline);
  float v = dev * 6.0f;
  return v > 1 ? 1 : v;
}

void imuReadAccel(float& x, float& y, float& z) {
  x = y = 0; z = -9;                       // 默认无效值
  if (!g_ok) return;
  uint8_t b[6];
  if (rd(REG_AX_L, b, 6) != 0) return;
  x = (int16_t)(b[0] | (b[1] << 8)) / 8192.0f;
  y = (int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
  z = (int16_t)(b[4] | (b[5] << 8)) / 8192.0f;
}

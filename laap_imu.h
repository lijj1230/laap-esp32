#pragma once
#include <Arduino.h>

// 立创实战派 板载 QMI8658 六轴 IMU（世界模型传感器）
// 初始化时序来自官方例程 02-attitude
bool imuInit();          // 成功返回 true（找不到芯片不影响主流程）
float imuMotionLevel();  // 0..1 加速度扰动强度（静止≈0）
// F5 触觉：原始三轴（单位 g），供摇晃/翻面手势识别；IMU 无效时 z=-9
void imuReadAccel(float& x, float& y, float& z);

#pragma once
#include <Arduino.h>

// ============================================================
//  R0 微型循环处理器（v3.48）：8 单元回声状态网络（ESN）。
//  本架构唯一"权重级"循环计算：隐状态 h(t) 逐拍依赖 h(t-1)，
//  在线学习预测下一拍的世界传感（房间动静、WiFi 信号质量）。
//  预测误差 = 对世界的惊讶度，回注认知（curiosity）。
//  储备池固定种子免训练；读出层在线 LMS 自适应，设备上自己学。
// ============================================================
struct LaapR0 {
  void begin();                                   // 固定种子生成储备池
  void tick(float motion01, float rssi01);        // 每拍：误差→学习→更新隐状态→出下一拍预测
  float lastErr() const { return _err; }          // 本拍归一化误差 0..1（惊讶度）
  float rollingErr() const { return _roll; }      // 滚动平均（长期惊讶水位）
  uint32_t steps() const { return _steps; }

private:
  static const int N = 8, NI = 2, OUT = N + NI + 1;
  float _wRes[N][N];                              // 固定随机储备池（谱半径<1，回声态）
  float _wIn[N][NI];                              // 固定输入权重
  float _wOut[NI][OUT];                           // 读出层（在线学）
  float _h[N] = { 0 };
  float _pred[NI] = { 0 };                        // 上拍算出的"对这一拍的预测"
  float _xPrev[NI] = { 0 };
  bool _run = false;                              // 是否已有上一拍（首拍只建状态）
  float _err = 0, _roll = 0;
  uint32_t _steps = 0;
};

extern LaapR0 r0;

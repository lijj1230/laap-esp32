#include "laap_r0.h"

LaapR0 r0;

void LaapR0::begin() {
  randomSeed(20260928UL);                         // 固定种子：储备池确定，行为可复现
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++)
      _wRes[i][j] = (i == j) ? 0.0f : (random(-350, 350) / 1000.0f);
    for (int k = 0; k < NI; k++) _wIn[i][k] = random(-500, 500) / 1000.0f;
  }
  for (int o = 0; o < NI; o++)
    for (int j = 0; j < OUT; j++) _wOut[o][j] = 0.0f;   // 读出层从零学起
  // 幂迭代估谱半径并归一化到 0.9——"回声态"从断言变成构造性保证（v3.49 审计补）
  {
    float v[N];
    for (int i = 0; i < N; i++) v[i] = 1.0f;
    float rho = 0;
    for (int it = 0; it < 12; it++) {
      float w[N], n2 = 0;
      for (int i = 0; i < N; i++) {
        float s = 0;
        for (int j = 0; j < N; j++) s += _wRes[i][j] * v[j];
        w[i] = s; n2 += s * s;
      }
      n2 = sqrtf(n2);
      if (n2 < 1e-6f) break;
      rho = n2;
      for (int i = 0; i < N; i++) v[i] = w[i] / n2;
    }
    if (rho > 1e-6f) {
      float sc = 0.9f / rho;
      for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) _wRes[i][j] *= sc;
    }
  }
  _run = false; _err = _roll = 0; _steps = 0;
}

void LaapR0::tick(float motion01, float rssi01) {
  float x[NI] = { constrain(motion01, 0.0f, 1.0f), constrain(rssi01, 0.0f, 1.0f) };
  if (!isfinite(x[0]) || !isfinite(x[1])) return;  // 异常输入不进状态（防 NaN 毒化权重）
  if (!_run) {                                    // 首拍：无预测可评，只建状态
    memcpy(_xPrev, x, sizeof(x));
    _run = true;
    for (int i = 0; i < N; i++) {
      float s = 0;
      for (int k = 0; k < NI; k++) s += _wIn[i][k] * x[k];
      _h[i] = tanhf(s);
    }
    return;
  }
  // 1) 误差：上一拍的预测 vs 这一拍实际（两维都归一到 0..1）
  float e[NI] = { x[0] - _pred[0], x[1] - _pred[1] };
  _err = sqrtf(e[0] * e[0] + e[1] * e[1]) * 0.7071f;
  _roll = 0.98f * _roll + 0.02f * _err;
  // 2) 读出层在线 LMS（线性自适应，无发散风险）
  float phi[OUT];
  for (int i = 0; i < N; i++) phi[i] = _h[i];
  phi[N] = _xPrev[0]; phi[N + 1] = _xPrev[1]; phi[N + 2] = 1.0f;
  for (int o = 0; o < NI; o++)
    for (int j = 0; j < OUT; j++) _wOut[o][j] += 0.04f * e[o] * phi[j];
  // 3) 更新储备池——唯一的循环计算：h(t) 依赖 h(t-1)
  for (int i = 0; i < N; i++) {
    float s = 0;
    for (int j = 0; j < N; j++) s += _wRes[i][j] * _h[j];
    for (int k = 0; k < NI; k++) s += _wIn[i][k] * x[k];
    _h[i] = 0.65f * _h[i] + 0.35f * tanhf(s);
  }
  // 4) 用新状态出下一拍预测
  for (int i = 0; i < N; i++) phi[i] = _h[i];
  phi[N] = x[0]; phi[N + 1] = x[1];               // phi[N+2] 恒为 1
  for (int o = 0; o < NI; o++) {
    float p = 0;
    for (int j = 0; j < OUT; j++) p += _wOut[o][j] * phi[j];
    _pred[o] = constrain(p, 0.0f, 1.0f);
  }
  memcpy(_xPrev, x, sizeof(x));
  _steps++;
}

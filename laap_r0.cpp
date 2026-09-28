#include "laap_r0.h"
#include <Preferences.h>

LaapR0 r0;

// ---- 学习进度持久化（v3.50）：magic+steps+roll+h+xPrev+pred+wOut，~148B ----
static const uint32_t kR0Magic = 0x52304231UL;    // "R0B1"

void LaapR0::saveNvs() {
  if (!_run) return;                              // 没跑过的全新状态无可存
  const size_t NF = N + NI + NI + NI * OUT;       // h(8)+xPrev(2)+pred(2)+wOut(22) = 34
  uint8_t buf[8 + NF * 4];
  size_t o = 0;
  memcpy(buf + o, &kR0Magic, 4); o += 4;
  uint32_t st = _steps; memcpy(buf + o, &st, 4); o += 4;
  float ro = _roll; memcpy(buf + o, &ro, 4); o += 4;
  for (int i = 0; i < N; i++) { float v = _h[i]; memcpy(buf + o, &v, 4); o += 4; }
  for (int i = 0; i < NI; i++) { float v = _xPrev[i]; memcpy(buf + o, &v, 4); o += 4; }
  for (int i = 0; i < NI; i++) { float v = _pred[i]; memcpy(buf + o, &v, 4); o += 4; }
  for (int i = 0; i < NI; i++) for (int j = 0; j < OUT; j++) { float v = _wOut[i][j]; memcpy(buf + o, &v, 4); o += 4; }
  Preferences p;
  if (!p.begin("laapmtr", false)) return;
  p.putBytes("r0", buf, o);
  p.end();
}

bool LaapR0::loadNvs() {
  Preferences p;
  if (!p.begin("laapmtr", true)) return false;
  size_t n = p.getBytesLength("r0");
  const size_t NF = N + NI + NI + NI * OUT;
  if (n != 8 + NF * 4) { p.end(); return false; }
  uint8_t buf[8 + NF * 4];
  if (p.getBytes("r0", buf, sizeof(buf)) != n) { p.end(); return false; }
  p.end();
  size_t o = 0;
  uint32_t magic; memcpy(&magic, buf + o, 4); o += 4;
  if (magic != kR0Magic) return false;
  uint32_t st; memcpy(&st, buf + o, 4); o += 4;
  float ro; memcpy(&ro, buf + o, 4); o += 4;
  auto fin = [](float v) { return isfinite(v) && v > -1e3f && v < 1e3f; };
  float h[N], xPrev[NI], pred[NI], wOut[NI][OUT];
  for (int i = 0; i < N; i++) { float v; memcpy(&v, buf + o, 4); o += 4; if (!fin(v)) return false; h[i] = v; }
  for (int i = 0; i < NI; i++) { float v; memcpy(&v, buf + o, 4); o += 4; if (!fin(v)) return false; xPrev[i] = v; }
  for (int i = 0; i < NI; i++) { float v; memcpy(&v, buf + o, 4); o += 4; if (!fin(v)) return false; pred[i] = v; }
  for (int i = 0; i < NI; i++) for (int j = 0; j < OUT; j++) { float v; memcpy(&v, buf + o, 4); o += 4; if (!fin(v)) return false; wOut[i][j] = v; }
  for (int i = 0; i < N; i++) _h[i] = h[i];       // 全部校验通过才提交（半毒 blob 不落地）
  for (int i = 0; i < NI; i++) { _xPrev[i] = xPrev[i]; _pred[i] = pred[i]; }
  for (int i = 0; i < NI; i++) for (int j = 0; j < OUT; j++) _wOut[i][j] = wOut[i][j];
  _steps = st; _roll = ro; _run = true;
  return true;
}

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

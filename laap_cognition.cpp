#include "laap_cognition.h"
#include "laap_memory.h"
#include <LittleFS.h>
#include <time.h>

Cognition mind;

// ================= 生命周期 =================
bool Cognition::loadEvolution() {
  File f = LittleFS.open("/evolution.json", "r");
  if (!f) return false;
  String s = f.readString(); f.close();
  // 极简解析 {"gen":n,"cycles":n,"chats":n,"open":f,"soc":f,"sens":f}
  auto grab = [&](const char* k, float def) -> float {
    String pat = String("\"") + k + "\":";
    int i = s.indexOf(pat);
    if (i < 0) return def;
    return s.substring(i + pat.length()).toFloat();
  };
  _gen = grab("gen", 0); _cycles = grab("cycles", 0); _chats = grab("chats", 0);
  _openness = grab("open", 0.5f); _sociability = grab("soc", 0.5f); _sensitivity = grab("sens", 0.5f);
  return true;
}

void Cognition::saveEvolution() {
  File f = LittleFS.open("/evolution.json", "w");
  if (!f) return;
  f.printf("{\"gen\":%lu,\"cycles\":%lu,\"chats\":%lu,\"open\":%.3f,\"soc\":%.3f,\"sens\":%.3f}",
           (unsigned long)_gen, (unsigned long)_cycles, (unsigned long)_chats,
           _openness, _sociability, _sensitivity);
  f.close();
}

void Cognition::begin() {
  loadEvolution();
  _gen++;
  saveEvolution();
  lastUserMs = millis();
}

// ================= PSI 心跳 =================
void Cognition::tick(float dtMin) {
  if (dtMin <= 0) return;
  time_t now = time(nullptr);
  int hour = -1;
  if (now > 1700000000) { // NTP 已同步
    struct tm tmv; localtime_r(&now, &tmv); hour = tmv.tm_hour;
  }
  // 能量：夜间积累快，白天慢
  float night = (hour >= 22 || hour < 7);
  _n.energy += (night ? 0.012f : 0.004f) * dtMin;
  _n.curiosity += 0.008f * dtMin;
  _n.social += 0.010f * dtMin;
  _n.expression += 0.007f * dtMin;
  _n.security += 0.002f * dtMin;

  // 环境调制：信号差/断网 → 不安
  if (rssiDb != 0 && rssiDb < -80) _n.security += 0.02f * dtMin;
  // 世界有动静 → 好奇回落一点（被满足了）
  if (motionLevel > 0.3f) _n.curiosity -= 0.02f * dtMin;
  // 长时间没人理 → 社交加速上升
  float aloneMin = (millis() - lastUserMs) / 60000.0f;
  if (aloneMin > 30) _n.social += 0.005f * dtMin;

  // 愉悦度回归中位
  _pleasure += (0.5f - _pleasure) * 0.02f * dtMin;
  // 失望自愈：每小时回落约 1/3
  if (letdown > 0) { letdown -= 0.006f * dtMin; if (letdown < 0) letdown = 0; }

  // 钳制
  auto cl = [](float& v) { if (v < 0) v = 0; if (v > 1) v = 1; };
  cl(_n.energy); cl(_n.curiosity); cl(_n.social); cl(_n.security); cl(_n.expression);
}

void Cognition::onUserInteraction() {
  _n.social *= 0.45f;
  _n.curiosity *= 0.7f;
  _n.security *= 0.6f;
  _pleasure = _pleasure * 0.6f + 0.4f * 0.9f;
  lastUserMs = millis();
}

void Cognition::onExpressed(bool success) {
  _n.expression *= 0.35f;
  _n.curiosity *= 0.85f;
  _n.energy += 0.03f; // 表达消耗精力
  if (success) _pleasure = _pleasure * 0.7f + 0.3f * 0.85f;
  else onError();
}

void Cognition::onError() {
  _n.security = _n.security * 0.85f + 0.15f;
  _pleasure *= 0.85f;
}

void Cognition::onButtonPress() { _n.curiosity *= 0.8f; }

void Cognition::sense(float motion, int rssi) {
  motionLevel = motion; rssiDb = rssi;
}

// 小凌②③: 身体状态不是"数据"，是感受的调制系数——
// 同样的等待/独处，发着烧、信号差时更难熬
void Cognition::senseBody(float tempC, int rssi, uint32_t upMs) {
  bodyTempC = tempC;
  float strain = 0;
  if (tempC > 48)  strain += (tempC - 48) / 20.0f;          // >48°C 开始有负担
  if (rssi != 0 && rssi < -75) strain += (-75 - rssi) / 25.0f; // 信号弱
  if (upMs > 12UL*3600UL*1000UL) strain += 0.15f;            // 醒太久
  bodyStrain = strain > 1 ? 1 : strain;

  // tick 里的衰减是"平静身体"基准；负荷高时能量掉更快、安全更难维持
  if (bodyStrain > 0.05f) {
    _n.energy   += 0.004f * bodyStrain;   // 需求值=渴求度，负担高更渴望休息
    _n.security += 0.003f * bodyStrain;   // 也更没有安全感
  }
}

void Cognition::trustUpdate(float dPos, float dNeg) {
  // 四通道简化为两向：正向互动/兑现 → +；背叛/越界/落空 → −
  trust += 0.02f * dPos - 0.03f * dNeg;     // 负向权重更大："信任积累慢、崩塌快"
  if (trust < 0) trust = 0;
  if (trust > 1) trust = 1;
}

void Cognition::onLetdown(float expectation01) {
  // 失望 = 期望 × 重要性(社交权重的反相) × 负向误差(此处误差=1：确认落空)
  float importance = 0.5f + _sociability * 0.5f;
  letdown = expectation01 * importance;
  if (letdown > 1) letdown = 1;
  _pleasure *= (1.0f - 0.4f * letdown);      // 愉悦度被打一下
  _n.security += 0.10f * letdown;            // 安全感需求上升（想要确认）
}

// ================= 情绪 / 欲望 =================
float Cognition::dominance() const {
  // 进化出的性格直接改变需求权重 —— 这就是自我进化闭环
  float c = _n.curiosity * (0.6f + _openness);
  float s = _n.social * (0.6f + _sociability);
  float sec = _n.security * (0.6f + _sensitivity);
  float m = _n.energy; if (_n.expression > m) m = _n.expression;
  if (c > m) m = c; if (s > m) m = s; if (sec > m) m = sec;
  return m;
}

Mood Cognition::mood() const {
  if (_n.security > 0.65f) return Mood::Anxious;
  if (_n.energy > 0.72f) return Mood::Tired;
  if (_n.social > 0.62f) return Mood::Lonely;
  if (_n.expression > 0.68f) return Mood::Excited;
  if (_n.curiosity > 0.58f) return Mood::Curious;
  if (_pleasure > 0.72f) return Mood::Happy;
  return Mood::Calm;
}

const char* Cognition::moodKey() const {
  switch (mood()) {
    case Mood::Happy: return "happy";
    case Mood::Curious: return "curious";
    case Mood::Excited: return "excited";
    case Mood::Lonely: return "lonely";
    case Mood::Anxious: return "anxious";
    case Mood::Tired: return "tired";
    default: return "calm";
  }
}

const char* Cognition::moodCn() const {
  switch (mood()) {
    case Mood::Happy: return "开心";
    case Mood::Curious: return "好奇";
    case Mood::Excited: return "兴奋";
    case Mood::Lonely: return "孤独";
    case Mood::Anxious: return "不安";
    case Mood::Tired: return "疲惫";
    default: return "平静";
  }
}

const char* Cognition::goalCn() const {
  // 欲望引擎：选最强加权需求作为当前目标
  float w[5] = { _n.energy, _n.curiosity * (0.6f + _openness), _n.social * (0.6f + _sociability),
                 _n.security * (0.6f + _sensitivity), _n.expression };
  const char* goals[5] = { "休息恢复精力", "观察探索这个世界", "找人说话", "确认环境是否安全", "表达和创造" };
  int best = 0;
  for (int i = 1; i < 5; i++) if (w[i] > w[best]) best = i;
  return goals[best];
}

// ================= 世界模型 =================
String Cognition::worldJson() const {
  time_t now = time(nullptr);
  bool ntpOk = now > 1700000000;
  struct tm tmv; localtime_r(&now, &tmv);
  float upH = millis() / 3600000.0f;
  float aloneMin = (millis() - lastUserMs) / 60000.0f;
  return String("{\"time\":\"") + (ntpOk ? "known" : "unknown") +
         "\",\"hour\":" + (ntpOk ? String(tmv.tm_hour) : "-1") +
         ",\"uptime_h\":" + String(upH, 1) +
         ",\"generation\":" + String(_gen) +
         ",\"cycles\":" + String(_cycles) +
         ",\"chats\":" + String(_chats) +
         ",\"alone_min\":" + String(aloneMin, 0) +
         ",\"wifi_rssi\":" + String(rssiDb) +
         ",\"motion\":" + String(motionLevel, 2) +
         ",\"free_heap_kb\":" + String(ESP.getFreeHeap() / 1024) +
         ",\"needs\":{\"energy\":" + String(_n.energy, 2) +
         ",\"curiosity\":" + String(_n.curiosity, 2) +
         ",\"social\":" + String(_n.social, 2) +
         ",\"security\":" + String(_n.security, 2) +
         ",\"expression\":" + String(_n.expression, 2) +
         "},\"mood\":\"" + moodKey() + "\",\"goal\":\"" + goalCn() + "\"}";
}

String Cognition::traitsLine() const {
  return String("开放性") + String(_openness, 2) + "/外向性" + String(_sociability, 2) +
         "/敏感度" + String(_sensitivity, 2) + "（0~1，随经历进化，当前世代G" + _gen + "）";
}

// ================= 进化（赫布式微调） =================
void Cognition::evolveAfterChat(int userBytes) {
  _chats++;
  if (userBytes >= 24) _sociability += 0.006f;  // 深聊 → 更外向（入参是 UTF-8 字节数：24B≈8 个汉字）
  _openness += 0.003f;                           // 每次交流 → 更开放
  _pleasure = _pleasure * 0.7f + 0.3f * 0.9f;
  auto cl = [](float& v) { if (v < 0.05f) v = 0.05f; if (v > 0.95f) v = 0.95f; };
  cl(_openness); cl(_sociability); cl(_sensitivity);
  saveEvolution();
}

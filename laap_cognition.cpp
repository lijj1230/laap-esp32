#include "laap_cognition.h"
#include "laap_memory.h"
#include "laap_snap.h"   // evolution.json 每次心跳都可能重写：快照走自动档节流
#include "laap_llm.h"    // utf8Cut（意图文本按字符边界截断）
#include <LittleFS.h>
#include <time.h>

Cognition mind;

// ================= 生命周期 =================
bool Cognition::loadEvolution() {
  File f = LittleFS.open("/evolution.json", "r");
  if (!f) return false;
  String s = f.readString(); f.close();
  // 极简解析 {"gen":n,"cycles":n,"chats":n,"open":f,"soc":f,"sens":f,"nE":f,"nC":f,"nSo":f,"nSe":f,"nEx":f,"pl":f}
  auto grab = [&](const char* k, float def) -> float {
    String pat = String("\"") + k + "\":";
    int i = s.indexOf(pat);
    if (i < 0) return def;
    return s.substring(i + pat.length()).toFloat();
  };
  _gen = grab("gen", 0); _cycles = grab("cycles", 0); _chats = grab("chats", 0);
  _openness = grab("open", 0.5f); _sociability = grab("soc", 0.5f); _sensitivity = grab("sens", 0.5f);
  // 需求五维+愉悦度跟性格同文件续跑（v3.42）：打盹/OTA/断电重启后情绪不"睡一觉归零"。
  // 老文件没有这些键 → grab 回退默认值，天然向后兼容
  _n.energy     = grab("nE",  0.30f);
  _n.curiosity  = grab("nC",  0.40f);
  _n.social     = grab("nSo", 0.45f);
  _n.security   = grab("nSe", 0.20f);
  _n.expression = grab("nEx", 0.35f);
  _pleasure     = grab("pl",  0.5f);
  _savedN = _n; _savedPl = _pleasure;
  return true;
}

void Cognition::saveEvolution(bool force) {
  // 节流：无变化且不满 96 周期（48min）就跳过——原来主循环每 30s 都走到这里全量写
  // （"周期数变化才写"恒真），~1MB/天纯白写
  if (!force && !_evoDirty && _cycles - _lastEvoSaveCycle < 96) return;
  _evoDirty = false;
  _lastEvoSaveCycle = _cycles;
  laapSnapMake("evolution", false);   // 心跳级写入×12h 节流 ≈ 每半天留一版性格
  // 原子重写：心跳级高频写 + 掉电窗口，直接 open("w") 半写会让性格/代数静默回退默认值
  File f = LittleFS.open("/evolution.tmp", "w");
  if (!f) return;
  f.printf("{\"gen\":%lu,\"cycles\":%lu,\"chats\":%lu,\"open\":%.3f,\"soc\":%.3f,\"sens\":%.3f,"
           "\"nE\":%.3f,\"nC\":%.3f,\"nSo\":%.3f,\"nSe\":%.3f,\"nEx\":%.3f,\"pl\":%.3f}",
           (unsigned long)_gen, (unsigned long)_cycles, (unsigned long)_chats,
           _openness, _sociability, _sensitivity,
           _n.energy, _n.curiosity, _n.social, _n.security, _n.expression, _pleasure);
  f.close();
  // v3.76e：单步 rename 直接覆盖（remove+rename 两步之间掉电 = 文件整个消失，比半写更糟）
  LittleFS.rename("/evolution.tmp", "/evolution.json");
  _savedN = _n; _savedPl = _pleasure;   // 快照对齐：下次从"写入时的值"起算漂移
}

void Cognition::begin() {
  loadEvolution();
  _gen++;
  saveEvolution(true);
  loadIntents();
  dropStaleIntents((uint32_t)time(nullptr));
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
  // ============================================================
  //  稳态化（active inference 的 homeostasis）：增长项乘饱和因子 (1-x)。
  //  每个"增长-衰减"对从此有稳定平衡点 x* = g/(g+d) < 1——需求永不可能
  //  被钉死在 1.0（结构性防跑飞），也不再依赖"回补速率必须大于累积速率"
  //  的调参巧合（那是当年"卡阈值死循环"补丁的根因）。各平衡点：
  //    夜间能量 0.375（安睡）/ 白天独处能量 0.31（打盹）/ 独处社交 0.42~0.59
  //    独处表达 0.44 / 稳定网络安全 0.33 —— 全部落在各自情绪阈值的合理侧。
  //  ============================================================
  float night = (hour >= 22 || hour < 7);
  _n.energy     += (night ? 0.012f : 0.004f) * dtMin * (1.0f - _n.energy);
  _n.curiosity  += 0.008f * dtMin * (1.0f - _n.curiosity);
  _n.social     += 0.010f * dtMin * (1.0f - _n.social);
  _n.expression += 0.007f * _negGain * dtMin * (1.0f - _n.expression);   // C3：负反馈压增长项 g→平衡点 x*=g/(g+d) 下移（v3.61 修正：原误乘衰减项=方向反转）
  _n.security   += 0.002f * dtMin * (1.0f - _n.security);

  // ============================================================
  //  自我回补（独处/夜间回落）：线性衰减在低值区自然趋缓（×x 的比例式也一样），
  //  与上面的饱和增长构成完整稳态环。
  // ============================================================
  // C3 快变量回中：~90 分钟半衰期向 1.0 恢复（无负反馈 2 小时后基本痊愈）
  if (_negGain < 1.0f) {
    _negGain += 0.004f * dtMin;                       // dtMin=心跳分钟数，30s 拍≈+0.002
    if (_negGain > 1.0f) _negGain = 1.0f;
  }
  float aloneMin = (millis() - lastUserMs) / 60000.0f;
  bool quiet = aloneMin > 5.0f;                       // 五分钟没人理 = 独处静息
  if (night)        _n.energy -= 0.020f * dtMin;      // 夜里睡下：平衡点 0.375
  else if (quiet)   _n.energy -= 0.009f * dtMin;      // 白天独处打盹：平衡点 0.31
  if (quiet) {                                        // 独处久了会自我调适（不调适就永远黏人）
    _n.social     -= 0.014f * dtMin;                  // 平衡点 0.42（<30min 时）
    _n.expression -= 0.009f * dtMin;                  // 平衡点 0.44（g 侧已被 C3 调制，衰减保持常量）
  }
  if (rssiDb == 0 || rssiDb > -70) _n.security -= 0.004f * dtMin;  // 环境稳定=安全感回落（平衡 0.33）
  if (rssiDb != 0 && rssiDb < -80) _n.security += 0.02f * dtMin;   // 信号差/断网 → 不安（原来的环境调制）
  if (motionLevel > 0.3f) _n.curiosity -= 0.02f * dtMin;           // 世界有动静 → 好奇被满足
  if (aloneMin > 30) _n.social += 0.010f * dtMin * (1.0f - _n.social); // 太久没人理仍会想念（0.010 保平衡点 ≈0.59，孤独阈值 0.62 附近）

  // 愉悦度回归中位
  _pleasure += (0.5f - _pleasure) * 0.02f * dtMin;
  // 失望自愈：每小时回落约 1/3
  if (letdown > 0) { letdown -= 0.006f * dtMin; if (letdown < 0) letdown = 0; }

  // 钳制：下限 0.05 而不是 0——真实需求系统不会"完全归零"，
  // 而且全为 0 时 dominance()=0、[状态提示] 仍挑一个"最高"需求乱给语气指引
  // （实测连聊几轮后 social/curiosity/expression 全掉到 0.02，整机进入无欲无求的瘫平态）
  auto cl = [](float& v) { if (v < 0.05f) v = 0.05f; if (v > 1) v = 1; };
  cl(_n.energy); cl(_n.curiosity); cl(_n.social); cl(_n.security); cl(_n.expression);

  // 需求漂移标脏（v3.42）：任一维/愉悦度偏离上次落盘快照 >0.05 就置脏，
  // 真实写盘节奏仍由 saveEvolution 的节流决定——任何重启前的值最多差 0.05
  if (fabsf(_n.energy - _savedN.energy) > 0.05f || fabsf(_n.curiosity - _savedN.curiosity) > 0.05f ||
      fabsf(_n.social - _savedN.social) > 0.05f || fabsf(_n.security - _savedN.security) > 0.05f ||
      fabsf(_n.expression - _savedN.expression) > 0.05f || fabsf(_pleasure - _savedPl) > 0.05f)
    _evoDirty = true;
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

// 自言自语：满足度低于"对主人说话"（自己聊不如有人听），但仍要计入
void Cognition::onMonologue() {
  _n.curiosity *= 0.65f;
  _n.expression *= 0.55f;
  _n.social *= 0.88f;
  _n.energy += 0.015f;                       // 动脑子也耗精力
  _pleasure = _pleasure * 0.85f + 0.15f * 0.6f;
}

// 发现新知：好奇的消解量随新颖度走——查到全新的东西（gain 高）比把已知的事再聊一遍
// （gain 低）满足得多（active inference：好奇 = 不确定性下降，Schmidhuber 的学习进度）
void Cognition::onDiscovery(float gain01) {
  if (gain01 < 0) return;
  if (gain01 > 1) gain01 = 1;
  _n.curiosity -= 0.02f + 0.06f * gain01;
  if (_n.curiosity < 0.05f) _n.curiosity = 0.05f;
  _pleasure = _pleasure * 0.9f + 0.1f * (0.6f + 0.3f * gain01);
}

// ================= 意图栈（PIANO goals 模块） =================
static const char* INTENTS_PATH = "/mem/intents.txt";

void Cognition::loadIntents() {
  intentN = 0;
  File f = LittleFS.open(INTENTS_PATH, "r");
  if (!f) return;
  while (f.available() && intentN < 3) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    int bar = ln.indexOf('|');
    if (bar <= 0) continue;
    intentBorn[intentN] = (uint32_t)ln.substring(0, bar).toInt();
    intents[intentN] = ln.substring(bar + 1);
    if (intents[intentN].length()) intentN++;
  }
  f.close();
}

void Cognition::saveIntents() {
  // 原子写：掉电落在 open("w") 截断之后 = 意图栈整个丢（与其他重写路径同规格）
  String tmpPath = String(INTENTS_PATH) + ".tmp";
  File f = LittleFS.open(tmpPath, "w");
  if (!f) return;
  for (int i = 0; i < intentN; i++)
    f.printf("%lu|%s\n", (unsigned long)intentBorn[i], intents[i].c_str());
  f.close();
  // v3.76e：单步 rename 直接覆盖（消除 remove→rename 间的掉电丢失窗口）
  LittleFS.rename(tmpPath, INTENTS_PATH);
}

// 清空记忆联动：性格/代数/计数归零并立即落盘（防止 psiTick 用 RAM 旧值写回）
void Cognition::resetEvolution() {
  _openness = _sociability = _sensitivity = 0.5f;
  _gen = 0; _cycles = 0; _chats = 0;
  _n = Needs();                        // "整个人重来"连需求/情绪一起归零（否则旧值会被写回盘）
  _pleasure = 0.5f;
  _expCat = EXP_NONE;                  // R3 预期残留一并清（否则上一世的 expect_last 进提示词）
  _expLastTxt = "";
  saveEvolution(true);
}

// 意图栈整栈放下（含磁盘）
void Cognition::clearAllIntents() {
  intentN = 0;
  LittleFS.remove(INTENTS_PATH);
}

bool Cognition::addIntent(const String& text, uint32_t ts) {
  String t = text;
  t.trim();
  t = utf8Cut(t, 60);                        // ≤20 字
  if (t.length() < 6) return false;
  for (int i = 0; i < intentN; i++)
    if (intents[i] == t) return false;       // 已有
  if (intentN >= 3) {                        // 满：放下最老的
    int oldest = 0;
    for (int i = 1; i < intentN; i++) if (intentBorn[i] < intentBorn[oldest]) oldest = i;
    for (int i = oldest; i < intentN - 1; i++) { intents[i] = intents[i + 1]; intentBorn[i] = intentBorn[i + 1]; }
    intentN--;
  }
  intents[intentN] = t; intentBorn[intentN] = ts; intentN++;
  saveIntents();
  return true;
}

void Cognition::dropIntent(int i) {
  if (i < 0 || i >= intentN) return;
  for (int k = i; k < intentN - 1; k++) { intents[k] = intents[k + 1]; intentBorn[k] = intentBorn[k + 1]; }
  intentN--;
  saveIntents();
}

void Cognition::dropStaleIntents(uint32_t nowTs) {
  bool changed = false;
  for (int i = intentN - 1; i >= 0; i--)
    if (intentBorn[i] && nowTs > intentBorn[i] + 7UL * 86400UL) { dropIntent(i); changed = true; }
  (void)changed;
}

String Cognition::intentsLine() const {
  if (intentN == 0) return "";
  String s = "心里惦记的事：";
  for (int i = 0; i < intentN; i++) { if (i) s += "、"; s += "「" + intents[i] + "」"; }
  return s;
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
// dtMin：按分钟计率（原来按"每次心跳"加，心跳间隔一改数值就全变，且量级大到能压过主平衡）
void Cognition::senseBody(float tempC, int rssi, uint32_t upMs, float dtMin) {
  bodyTempC = tempC;
  float strain = 0;
  if (tempC > 48)  strain += (tempC - 48) / 20.0f;          // >48°C 开始有负担
  if (rssi != 0 && rssi < -75) strain += (-75 - rssi) / 25.0f; // 信号弱
  if (upMs > 12UL*3600UL*1000UL) strain += 0.15f;            // 醒太久
  bodyStrain = strain > 1 ? 1 : strain;

  // tick 里的衰减是"平静身体"基准；负荷高时能量掉更快、安全更难维持
  // （与 tick 主环同款饱和因子：需求趋近 1 时增长自然收束，不再线性顶满）
  if (bodyStrain > 0.05f && dtMin > 0) {
    _n.energy   += 0.010f * bodyStrain * dtMin * (1.0f - _n.energy);
    _n.security += 0.008f * bodyStrain * dtMin * (1.0f - _n.security);
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
  // 性格权重封顶 1.2 倍（v3.58）：原 0.6+trait 上限 1.6 倍会把后台阈值实际拉低 1/3
  // （需求 60% 按 96% 触发=阈值形同虚设），且该参数只涨不跌=单向通胀。封顶后进化仍改
  // 变触发倾向（0.9→1.2 倍区间），只是不再无界放大；内容/品味层面照常进化不受限
  float c = _n.curiosity * (0.6f + min(_openness, 0.6f));
  float s = _n.social * (0.6f + min(_sociability, 0.6f));
  float sec = _n.security * (0.6f + min(_sensitivity, 0.6f));
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

// ---- R3 预测-误差回环（v3.48）----
static const char* kExpectCn[] = { "没什么可预期", "主人会来", "主人不会来", "世界将有动静", "世界会安静" };

void Cognition::setExpectation(uint8_t cat) {
  if (cat > EXP_WORLD_QUIET) cat = EXP_NONE;
  _expCat = cat;
  _expAtMs = millis();
}

// C3 快变量：负反馈事件压表达/社交的增长增益。×0.55 一步，向 1.0 以 ~90 分钟半衰期
// 回中（稳态环自己消化，无需显式恢复逻辑）；下限 0.3=再怎么踩也保留三成天性。
// 事件源：网页👎/播报被打断/夜间规则里"求安静"类命中
void Cognition::onNegativeFeedback(const char* src) {
  _negGain *= 0.55f;
  if (_negGain < 0.3f) _negGain = 0.3f;
  _n.expression = min(_n.expression, 0.5f);    // 当拍表达欲直接压回阈下（0.5<默认阈 0.55，v3.61 修正）
  Serial.printf("[C3] 负反馈(%s) 表达增益→%.2f\n", src, _negGain);
}

// C1：全局广播判决。salience 由调用方按事件源合成（需求/intent/惊讶），这里只做
// "冠军须过阈"的门控与节拍槽登记。阈=0.62，随 dominance 反比微调（0.5~0.72）：
// 欲望越高，越小的声音也能抢到舞台；瘫平时只有强念头才上得了台
float Cognition::broadcastSalience(const char* kind, const String& text, float salience, bool writeBeat) {
  float dom = dominance();
  float th = 0.62f - 0.12f * dom;                    // dom=1→0.50；dom=0→0.62
  _bcTh = th;
  if (salience < th) { Serial.printf("[BC] %s %.2f<%.2f 未赢得\n", kind, salience, th); return salience; }
  _bcCount++;
  if (writeBeat) {   // v3.62：need 通道等"非事件"胜利只判门不写槽（防常量文本连场复读）
    _bcText = String("[此刻占据注意的事] ") + text + "\n（它刚赢得你的注意，可以自然影响此刻的语气与念头，不必刻意提起）";
    _lastBcMs = millis();
  }
  Serial.printf("[BC] %s %.2f>=%.2f 赢得广播\n", kind, salience, th);
  return salience;
}

// A1 常驻元监控（v3.61）：把自己此刻的认知状态汇总成一行说给自己听——在线高阶
// 自模型的提示词化（HOT 族指标的工程落点）；注入点在 buildSystemPrompt
String Cognition::metaLine(bool calm) const {
  String parts;
  if (_lastBcMs) {
    long age = (long)((millis() - _lastBcMs) / 60000UL);
    // v3.62：事龄封顶（安静数日后"3000 分钟"式观感）+ 定性化
    if (age > 180) parts += "心里有件事放了挺久";
    else parts += (age == 0) ? String("刚在心里过了一件事") : String("心上事龄 ") + age + " 分钟";
  }
  float ema = 0; int n = 0;
  for (int i = 1; i <= EXP_WORLD_QUIET; i++) if (_expN[i]) { ema += _expEma[i]; n++; }
  if (n) {
    if (parts.length()) parts += "，";
    // v3.62：命中率定性分档——系统规则 2 禁止模型输出百分比，注入样例自带数字有被复读的风险
    float hit = ema / n * 100;
    parts += (hit < 40) ? String("预期最近常落空") : (hit < 70 ? String("预期时准时不准") : String("预期常能应验"));
  }
  if (_negGain < 0.9f && !calm) {   // calm=[自我校准]行已在场，免同帧双注入（v3.62）
    if (parts.length()) parts += "，";
    parts += "近期被踩过（语气收敛中）";
  }
  return parts;
}

float Cognition::expectPrecision(uint8_t cat) const {
  if (cat == EXP_NONE || cat > EXP_WORLD_QUIET) return 1.0f;
  // 判定次数越少越向 0.5 收（冷启动别让一两次样本定终身）；EMA 本身钳 0.05~1
  float prior = 0.5f;
  float w = _expN[cat] < 4 ? _expN[cat] / 4.0f : 1.0f;
  float p = prior * (1 - w) + _expEma[cat] * w;
  if (p < 0.05f) p = 0.05f;
  return p;
}

void Cognition::expectEmaLoad(const uint8_t* blob) {
  for (int i = 0; i < 5; i++) {
    _expEma[i] = blob[i] / 200.0f;                   // 定点 /200：0~1.275 覆盖 0~1
    _expN[i] = blob[5 + i * 2] | (blob[6 + i * 2] << 8);
    if (_expEma[i] <= 0.001f && _expN[i] == 0) _expEma[i] = 0.5f;   // 全零=无数据
    if (_expEma[i] < 0.05f) _expEma[i] = 0.05f;
    if (_expEma[i] > 1) _expEma[i] = 1;
  }
}

void Cognition::expectEmaBlob(uint8_t* out) {
  for (int i = 0; i < 5; i++) {
    float e = _expEma[i] * 200.0f;
    out[i] = (uint8_t)(e > 255 ? 255 : e);
    out[5 + i * 2] = _expN[i] & 0xFF;
    out[6 + i * 2] = (_expN[i] >> 8) & 0xFF;
  }
}

void Cognition::expectOutcome(bool fulfilled) {
  _expLastTxt = String(kExpectCn[_expCat]) + (fulfilled ? "——应验了" : "——落空了");
  // C5：精度先验在 EMA 更新前取（当拍结果不得污染本次的惊讶增益）
  float prec = expectPrecision(_expCat);
  // 命中 EMA 更新（α=0.3 平滑，钳 0.05~1 与落盘钳位一致）
  if (_expCat != EXP_NONE && _expCat <= EXP_WORLD_QUIET) {
    _expEma[_expCat] += 0.3f * ((fulfilled ? 1.0f : 0.0f) - _expEma[_expCat]);
    if (_expEma[_expCat] < 0.05f) _expEma[_expCat] = 0.05f;
    if (_expN[_expCat] < 65535) _expN[_expCat]++;
  }
  // 误差回注：预测错了才需要学——按类别把惊讶写进对应需求（小幅，稳态环自己消化）。
  // C5 精度加权（v3.61 修向）：一直很准的类别偶发落空=真意外（全量回注 1.0）；
  // 一直不准的类别落空是常态（打折 ~0.15），不再"常落空的预期反复推高需求=假情绪"
  float missGain = 0.1f + 0.9f * prec;             // 精度 1→1.0 全量；精度 0.05→0.145 打折
  if (_expCat == EXP_OWNER_COME) {
    _n.social += (fulfilled ? -0.08f : 0.05f * missGain);    // 来了=念想落地；落空=更想念
    if (_n.social < 0.05f) _n.social = 0.05f;     // 双侧钳（单侧 min 会瞬时出负需求）
    if (_n.social > 1) _n.social = 1;
    if (fulfilled) trustUpdate(1, 0);
  } else if (_expCat == EXP_OWNER_AWAY) {
    if (!fulfilled) _n.social = min(1.0f, _n.social + 0.06f * missGain);
  } else if (_expCat == EXP_WORLD_ACTIVE || _expCat == EXP_WORLD_QUIET) {
    if (!fulfilled) _n.curiosity = min(1.0f, _n.curiosity + 0.05f * missGain);
  }
  // A2 广播连续化（v3.61）：落空事件参与节拍竞争——越意外的落空越可能占据舞台
  if (!fulfilled && _expCat != EXP_NONE)
    broadcastSalience("exp-miss", String("预期落空了：") + kExpectCn[_expCat], 0.45f + 0.4f * prec);   // v3.62 修向：与 C5 同极性（越准的类别落空越意外）
  _expCat = EXP_NONE;                            // 评估完清空，等下次自发行为再立
}

void Cognition::noteSurprise(float s01) {
  if (s01 < 0) s01 = 0;
  if (s01 > 1) s01 = 1;
  _n.curiosity += 0.02f * s01 * (1.0f - _n.curiosity);   // 惊讶→好奇微抬（稳态环消化）
}

String Cognition::expectLine() const {
  String s = String("\"expect\":\"") + kExpectCn[_expCat] + "\"";
  if (_expLastTxt.length()) s += String(",\"expect_last\":\"") + _expLastTxt + "\"";
  return s;
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
         "},\"trust\":" + String(trust, 2) +
         "," + expectLine() + ",\"mood\":\"" + moodKey() + "\",\"goal\":\"" + goalCn() + "\"}";
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
  _evoDirty = true;                    // 性格真变化：下次落盘节拍放行
  saveEvolution();
}

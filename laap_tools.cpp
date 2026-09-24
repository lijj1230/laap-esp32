#include "laap_tools.h"
#include "laap_audio.h"
#include "laap_display.h"
#include "laap_config.h"

// ================= 匹配器 =================
static bool containsAny(const String& text, const char* const* words, int n) {
  for (int i = 0; i < n; i++) {
    if (!words[i][0]) continue;
    if (text.indexOf(words[i]) >= 0) return true;
  }
  return false;
}

bool toolMatchSentence(const VoiceTool& t, const String& text, ToolMatch& m) {
  m = ToolMatch();
  if (!containsAny(text, t.triggers, 8)) return false;
  // 动作语校验（防"你的声音真好听"误触）
  bool act = containsAny(text, t.actions, 8);
  // 数字抠取（连续数字串，如 "50"/"100"）
  String digits;
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c >= '0' && c <= '9') {
      digits += c;
      // 下一字符不是数字则收尾
      if (i + 1 >= text.length() || text[i + 1] < '0' || text[i + 1] > '9') break;
    }
  }
  m.hasValue = digits.length() > 0;
  if (m.hasValue) { m.value = digits.toInt(); act = true; }
  // 方向判定：抱怨句式"太X了"（太大/太亮/太高/太响…）意图是调小/调暗，
  // 但触发词本身含"大/亮/高"会被下面的增量词误判成向上——抱怨词优先反转为向下。
  bool complain = text.indexOf("太") >= 0 || text.indexOf("好吵") >= 0 || text.indexOf("刺眼") >= 0;
  bool upWord = text.indexOf("大") >= 0 || text.indexOf("亮") >= 0 || text.indexOf("高") >= 0;
  m.up = complain ? false : upWord;   // "大点声/调大"原语义不变；"太大了"改为向下
  m.fine = text.indexOf("一点") >= 0 || text.indexOf("一些") >= 0 || text.indexOf("稍微") >= 0;
  m.extreme = containsAny(text, t.actions, 8) &&
              (text.indexOf("最大") >= 0 || text.indexOf("最亮") >= 0);
  bool minside = text.indexOf("静音") >= 0 || text.indexOf("最暗") >= 0;
  if (m.extreme) { m.value = t.maxV; act = true; }
  else if (minside) { m.value = t.minV; act = true; }
  if (!act) return false;
  m.hit = true;
  return true;
}

// ================= 音量工具 =================
static String volApply(int value, int prev, const ToolMatch& m) {
  audio.setVolume((uint8_t)value);
  cfg.s.volume = (uint8_t)value;
  String say;
  if (value == 0)        say = "好，我安静待着。";
  else if (value == 100) say = "好，我用最大的声音说。";
  else if (value > prev) say = "音量调大到" + String(value) + "了。";
  else if (value < prev) say = "音量调小到" + String(value) + "了。";
  else                   say = "音量本来就是" + String(value) + "呢。";
  return say;
}

// ================= 亮度工具 =================
static String briApply(int value, int prev, const ToolMatch& m) {
  display.setBrightness((uint8_t)value);
  cfg.s.brightness = (uint8_t)value;
  String say;
  if (value >= 100)     say = "好，屏幕最亮。";
  else if (value <= 5)  say = "屏幕调到最暗了，还能看清我吗？";
  else if (value > prev) say = "屏幕调亮到" + String(value) + "了。";
  else if (value < prev) say = "屏幕调暗到" + String(value) + "了。";
  else                   say = "亮度本来就是" + String(value) + "呢。";
  return say;
}

// ================= 注册表（加新意图：一行 + 一个 apply） =================
static VoiceTool kTools[] = {
  { "volume",
    { "音量", "声音", "嗓门", "大声", "小声", "点声", "静音", "" },
    { "调", "太", "点", "些", "最大", "静音", "", "" },
    0, 100, 20, volApply },
  { "brightness",
    { "亮度", "屏幕", "太亮", "太暗", "刺眼", "最亮", "", "" },
    { "调", "太", "点", "些", "最亮", "刺眼", "", "" },
    5, 100, 20, briApply },
};
static const int kToolN = sizeof(kTools) / sizeof(kTools[0]);

void laapToolsInit() {
  // 应用持久化值（setup 里 audio/display 已各自应用，这里只兜日志）
  Serial.printf("[TOOLS] %d 个语音工具就绪（音量/亮度）\n", kToolN);
}

String laapToolsDispatch(const String& text) {
  for (int i = 0; i < kToolN; i++) {
    ToolMatch m;
    if (!toolMatchSentence(kTools[i], text, m)) continue;
    const VoiceTool& t = kTools[i];
    int cur = (t.name[0] == 'v') ? audio.volume() : display.getBrightness();
    int target = m.value;
    if (!m.hasValue && !m.extreme) {
      // 方向词步进（"一点/稍微"减半）；无方向词时 target=cur → 确认语走"本来就是"
      int step = m.fine ? t.step / 2 : t.step;
      target = m.up ? cur + step : cur - step;
    }
    if (target < t.minV) target = t.minV;
    if (target > t.maxV) target = t.maxV;
    String say = t.apply(target, cur, m);
    Serial.printf("[TOOLS] %s: %d%% -> %d%%\n", t.name, cur, target);
    cfg.save();
    return say;
  }
  return "";
}

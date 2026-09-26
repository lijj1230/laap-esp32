#include "laap_tools.h"
#include "laap_audio.h"
#include "laap_display.h"
#include "laap_config.h"
#include "laap_llm.h"      // utf8Cut
#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

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

// ================= 天气快问（联网，不经过大模型） =================
// 根因备注：让大模型从搜索结果里"自己得出天气"时，它常因自身人设（无联网能力的生命体）
// 回答"我查不到/我没有联网能力"。端侧直接取一行纯文本天气更稳更快（2026-09-26）。
static String httpGetText(const String& host, const String& path, int timeoutMs) {
  WiFiClientSecure cli;
  cli.setInsecure();                    // 端侧自签策略，见 README
  cli.setTimeout(timeoutMs);
  if (!cli.connect(host.c_str(), 443)) return "";
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: curl/8.0\r\nAccept: */*\r\nConnection: close\r\n\r\n");
  String resp; resp.reserve(2048);
  uint32_t deadline = millis() + timeoutMs;
  while (cli.connected() && millis() < deadline) {
    while (cli.available()) { resp += (char)cli.read(); if (resp.length() > 6000) break; }
    if (resp.length() > 6000) break;
    delay(2);
  }
  cli.stop();
  int sp = resp.indexOf(' ');
  int status = (sp > 0) ? resp.substring(sp + 1, sp + 4).toInt() : 0;
  if (status != 200) { Serial.printf("[WEA] HTTP %d\n", status); return ""; }
  int bs = resp.indexOf("\r\n\r\n");
  return (bs > 0) ? resp.substring(bs + 4) : String("");
}

// wttr.in 可能用 chunked；把分块壳剥掉（首行是纯十六进制长度才算）
static String dechunk(const String& in) {
  int nl = in.indexOf("\r\n");
  if (nl <= 0) return in;
  String first = in.substring(0, nl);
  for (unsigned int i = 0; i < first.length(); i++)
    if (!isHexadecimalDigit(first[i])) return in;
  String out; int pos = 0;
  while (pos < (int)in.length()) {
    int e = in.indexOf("\r\n", pos);
    if (e < 0) break;
    long n = strtol(in.substring(pos, e).c_str(), nullptr, 16);
    if (n <= 0) break;
    int s = e + 2;
    out += in.substring(s, s + n);
    pos = s + n + 2;
  }
  return out.length() ? out : in;
}

// 从"北京今天天气怎么样"里抠城市名（剥掉问句壳与天气词），空=按出口 IP 定位
static String weatherCityOf(const String& text) {
  static const char* junk[] = {"今天","明天","后天","现在","目前","最近","怎么样","怎样","如何","咋样",
                               "查一下","查查","帮我","帮忙","看看","一下","天气","气温","预报","下雨",
                               "下雪","冷不冷","热不热","的","呢","吗","呀","啊","？","?"," "};
  String s = text;
  for (auto j : junk) s.replace(j, "");
  s.trim();
  if (s.length() > 12) s = utf8Cut(s, 12);
  return s;
}

// wttr.in 的天气描述是英文（lang=zh 对 %C 不生效）→ 端侧转中文。
// 用"强度前缀 + 天气类型"组合而非穷举表：wttr 的措辞有上百种组合，穷举必漏
// （实测漏过 "Smoky haze"、"Moderate rain at times"）。
static String cnWeather(const String& en) {
  String s = en; s.toLowerCase();
  if (s.indexOf("thunder") >= 0) return "雷阵雨";
  if (s.indexOf("blizzard") >= 0) return "暴雪";
  String inten;                                   // 强度前缀
  if (s.indexOf("torrential") >= 0)      inten = "暴";
  else if (s.indexOf("heavy") >= 0)      inten = "大";
  else if (s.indexOf("moderate") >= 0)   inten = "中";
  else if (s.indexOf("light") >= 0 || s.indexOf("patchy") >= 0) inten = "小";
  static const struct { const char* k; const char* v; } kTypes[] = {
    {"smoky haze", "烟霾"}, {"haze", "霾"}, {"mist", "薄雾"}, {"fog", "雾"},
    {"freezing drizzle", "冻毛毛雨"}, {"freezing rain", "冻雨"},
    {"sleet", "雨夹雪"}, {"drizzle", "毛毛雨"}, {"rain", "雨"}, {"snow", "雪"},
    {"dust", "浮尘"}, {"sand", "沙尘"}, {"smoke", "烟"},
    {"overcast", "阴"}, {"cloudy", "多云"}, {"sunny", "晴"}, {"clear", "晴"},
    {"windy", "风大"}, {"breezy", "微风"},
  };
  for (auto& t : kTypes)
    if (s.indexOf(t.k) >= 0) return inten + t.v;
  return en;   // 没命中保留英文，总比没有强
}

static String weatherReport(const String& city) {
  // 5 分钟缓存：连着问不重复打网络
  static String cCity; static String cText; static uint32_t cMs = 0;
  if (cText.length() && cCity == city && millis() - cMs < 300000UL) return cText;
  String path = "/" + city + "?format=%C+%t&lang=zh";
  String enc; char buf[8];
  for (unsigned int i = 0; i < path.length(); i++) {
    char c = path[i];
    if (isalnum((unsigned char)c) || strchr("-_.~/?=&+", c)) enc += c;   // "+" 是 wttr 格式串分隔符，不能转义
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); enc += buf; }
  }
  String body = dechunk(httpGetText("wttr.in", enc, 6000));
  body.trim();
  int nl = body.indexOf('\n');
  if (nl > 0) body = body.substring(0, nl);
  body.trim();
  if (!body.length()) return "";
  // 形如 "Moderate rain at times\t+24°C"（注意 wttr 用制表符分隔，不能只按空格切）
  String temp, cond = body;
  int deg = body.indexOf("°");   // 必须用字符串：'°' 是多字节字面量，会被截成 0xB0 而误匹配续字节
  if (deg > 0) {
    // 温度 token 起点：往前退到"分隔符"为止。wttr 的分隔符可能是空格/制表符/不间断空格，
    // 所以除列出的空白外，遇到任何非 ASCII 字节也立刻停（绝不吃进中文/emoji）
    int st = deg;
    while (st > 0) {
      unsigned char c = (unsigned char)body[st - 1];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0 || c >= 0x80) break;
      st--;
    }
    int en2 = deg;                                 // 终点：往后到空白或结尾
    while (en2 < (int)body.length()) {
      unsigned char c = (unsigned char)body[en2];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0) break;
      en2++;
    }
    temp = body.substring(st, en2);
    cond = body.substring(0, st);
  }
  cond.trim(); temp.trim();
  Serial.printf("[WEA] raw=%s | cond=%s | temp=%s\n", body.c_str(), cond.c_str(), temp.c_str());
  if (temp.length()) { temp.replace("+", ""); temp.replace("°C", "度"); }   // TTS 友好："24度"
  String out = cnWeather(cond);
  if (temp.length()) out += " " + temp;
  cCity = city; cText = out; cMs = millis();
  return out;
}

String laapToolsDispatch(const String& text) {
  // 时间/日期：直接读本机 NTP 时钟。旧链路只把"几点"写进提示词、没有分钟，
  // 模型只能自己编 → 用户实测"问时间不准"（2026-09-26）
  static const char* tkeys[] = {"几点", "现在时间", "什么时间", "几号", "星期几", "周几", "今天的日期", "几月几号"};
  if (containsAny(text, tkeys, 8)) {
    time_t now = time(nullptr);
    if (now < 1700000000) return "我的时钟还没跟网络对上，等我联网校准一下再问我吧。";
    struct tm t; localtime_r(&now, &t);
    static const char* wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[80];
    snprintf(buf, sizeof(buf), "现在是 %d 月 %d 日 星期%s，%02d:%02d。",
             t.tm_mon + 1, t.tm_mday, wd[t.tm_wday], t.tm_hour, t.tm_min);
    Serial.printf("[TOOLS] time → %s\n", buf);
    return String(buf);
  }
  // 自身状态：直接读传感器（只认第一人称问法，避免"今天多少度"被抢答成体温）
  static const char* skeys[] = {"运行多久", "开机多久", "你多少度", "你的温度", "你热不热",
                                "你的信号", "内存还剩", "你醒多久"};
  if (containsAny(text, skeys, 8)) {
    uint32_t up = laapUptimeMin();
    char buf[170];
    snprintf(buf, sizeof(buf), "我这次醒来 %u 分钟（累计运行 %u 小时 %u 分），芯片体温 %.1f 度，"
                              "WiFi 信号 %d dBm，脑子里还剩 %uKB 空地方。",
             (unsigned)(millis() / 60000UL), (unsigned)(up / 60), (unsigned)(up % 60),
             temperatureRead(), WiFi.RSSI(), (unsigned)(ESP.getFreeHeap() / 1024));
    Serial.printf("[TOOLS] self → %s\n", buf);
    return String(buf);
  }
  // 天气优先（联网快问）：设备上"查天气"最稳的一条路
  // 但主人明说"搜索"时不要劫走——那是要联网搜网页，不是问天气
  static const char* wkeys[] = {"天气", "气温", "下雨", "下雪", "冷不冷", "热不热", "weather"};
  if (containsAny(text, wkeys, 7) && text.indexOf("搜索") < 0 && !text.startsWith("搜")) {
    String city = weatherCityOf(text);
    Serial.printf("[TOOLS] weather: 城市「%s」\n", city.length() ? city.c_str() : "(按出口IP定位)");
    String cond = weatherReport(city);
    if (!cond.length()) return "网络这会儿不太顺，天气没查着，过会儿再问我一次吧。";
    String say = city.length() ? (city + "现在" + cond) : ("你那边现在" + cond);
    Serial.printf("[TOOLS] weather → %s\n", say.c_str());
    return say;
  }
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

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

// ================= 天气快问（联网，不经过大模型） =================
// 根因备注：让大模型从搜索结果里"自己得出天气"时，它常因自身人设（无联网能力的生命体）
// 回答"我查不到/我没有联网能力"。端侧直接取一行纯文本天气更稳更快（2026-09-26）。
static String httpGetText(const String& host, const String& path, int timeoutMs) {
  // 与搜索/视觉同持一把网络锁（v3.51 审计：天气是全仓唯一漏网的联网路径，
  // 与后台独白 TLS 抢 ~40KB 连续块 = 任一方"连接失败"）
  if (!laapNetLock(1500)) { Serial.println("[WEA] 网络正忙（后台任务占用），本次跳过"); return ""; }
  WiFiClientSecure cli;
  cli.setInsecure();                    // 端侧自签策略，见 README
  cli.setTimeout(timeoutMs);
  if (!cli.connect(host.c_str(), 443)) { laapNetUnlock(); return ""; }
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: curl/8.0\r\nAccept: */*\r\nConnection: close\r\n\r\n");
  String hdrs, payload;
  laapHttpRead(&cli, timeoutMs, hdrs, payload, 6000);
  cli.stop();
  int sp = hdrs.indexOf(' ');
  int status = (sp > 0) ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (status != 200) { Serial.printf("[WEA] HTTP %d\n", status); laapNetUnlock(); return ""; }
  laapNetUnlock();
  return payload;
}

// wttr.in 可能用 chunked；把分块壳剥掉（首行是纯十六进制长度才算）

// 从"北京今天天气怎么样"里抠城市名（剥掉问句壳与天气词），空=按出口 IP 定位。
// "市"不能进无条件剥离表：它会把"你们城市"啃成"你们城"这种假城市名——只在结尾时剥一次
static String weatherCityOf(const String& text) {
  static const char* junk[] = {"今天","明天","后天","现在","目前","最近","怎么样","怎样","如何","咋样",
                               "查一下","查查","帮我","帮忙","看看","一下","天气","气温","预报","下雨",
                               "下雪","冷不冷","热不热","的","呢","吗","呀","啊","？","?"," ",
                               "你那边","外面","这里","这边","本地","室内","室外","屋里","家里",
                               "你们","你的","咱们","那边","那个","这个"};
  String s = text;
  for (auto j : junk) s.replace(j, "");
  s.trim();
  if (s.endsWith("市")) s = s.substring(0, s.length() - 3);   // "苏州市"→"苏州"
  s.trim();
  // 剥完还剩"形容词壳"（"今天天气真不错"→"真不错"）：这是陈述句不是点名城市，
  // 当成城市名去查必然查无此地——按无城市处理（走配置城市兜底）。
  // 黑名单只能挑无城市名冲突的词："太"不能进（太原/太仓是城市）
  if (s.length()) {
    static const char* notCity[] = {"不错","很","挺","真","热","冷","好","舒服","干燥","闷","凉快"};
    if (containsAny(s, notCity, sizeof(notCity) / sizeof(notCity[0]))) return String("");
  }
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

// ---------- 主源：Open-Meteo（免 Key，15 分钟更新；wttr.in 数据源陈旧是"天气不符"主因） ----------
// WMO 天气代码 → 中文
static String wmoCn(int code) {
  if (code == 0) return "晴";
  if (code == 1) return "基本晴";
  if (code == 2) return "局部多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code >= 51 && code <= 55) return "毛毛雨";
  if (code == 56 || code == 57) return "冻毛毛雨";
  if (code == 61) return "小雨";
  if (code == 63) return "中雨";
  if (code == 65) return "大雨";
  if (code == 66 || code == 67) return "冻雨";
  if (code == 71) return "小雪";
  if (code == 73) return "中雪";
  if (code == 75) return "大雪";
  if (code == 77) return "雪粒";
  if (code == 80) return "阵雨";
  if (code == 81) return "阵雨";
  if (code == 82) return "强阵雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code == 95) return "雷阵雨";
  if (code == 96 || code == 99) return "雷阵雨伴冰雹";
  return "天气代码" + String(code);
}

// JSON 里取数值字段：跳过同名的 units 键（值带引号），只认后面紧跟数字的
static bool jsonNum(const String& body, const char* key, float& out) {
  String pat = String("\"") + key + "\":";
  int pos = 0;
  while (true) {
    int p = body.indexOf(pat, pos);
    if (p < 0) return false;
    pos = p + pat.length();
    char c = body[pos];
    if (c == '-' || (c >= '0' && c <= '9')) { out = body.substring(pos).toFloat(); return true; }
  }
}

static String urlEncQuery(const String& s) {
  String enc; char buf[8];
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || strchr("-_.~", c)) enc += c;
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); enc += buf; }
  }
  return enc;
}

static String g_omCity; static String g_omText; static uint32_t g_omMs = 0;
static float g_geoLat = 999, g_geoLon = 999; static String g_geoCity;

static bool weatherOpenMeteo(const String& city, String& out) {
  // 5 分钟缓存
  if (g_omCity == city && g_omText.length() && millis() - g_omMs < 300000UL) { out = g_omText; return true; }
  // 60s 失败缓存（v3.51）：网络半通时原来 geo(10s)+forecast(10s)+wttr(6s) 每次问都全量重打
  static uint32_t s_omFailMs = 0;
  static String s_omFailCity;
  if (s_omFailCity == city && s_omFailMs && millis() - s_omFailMs < 60000UL) return false;
  float lat, lon;
  if (g_geoCity != city || g_geoLat > 900) {
    String body = httpGetText("geocoding-api.open-meteo.com",
                    "/v1/search?name=" + urlEncQuery(city) + "&count=1&language=zh&format=json", 6000);
    if (!jsonNum(body, "latitude", lat) || !jsonNum(body, "longitude", lon)) {
      s_omFailMs = millis(); s_omFailCity = city;   // 失败留底：60s 内不重打三发
      return false;
    }
    g_geoCity = city; g_geoLat = lat; g_geoLon = lon;
  }
  lat = g_geoLat; lon = g_geoLon;
  char p[176];
  snprintf(p, sizeof(p), "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&timezone=auto", lat, lon);
  String body = httpGetText("api.open-meteo.com", p, 6000);
  float tempC = 0, code = 0;
  if (!jsonNum(body, "temperature_2m", tempC) || !jsonNum(body, "weather_code", code)) {
    s_omFailMs = millis(); s_omFailCity = city;
    return false;
  }
  out = wmoCn((int)code) + " " + String((int)(tempC + (tempC >= 0 ? 0.5f : -0.5f))) + "度";
  g_omCity = city; g_omText = out; g_omMs = millis();
  Serial.printf("[WEA-OM] %s → %s（lat %.3f lon %.3f）\n", city.c_str(), out.c_str(), lat, lon);
  return true;
}

static String weatherReport(const String& city, String& resolvedLoc) {
  // 5 分钟缓存：连着问不重复打网络
  static String cCity; static String cText; static String cLoc; static uint32_t cMs = 0;
  if (cText.length() && cCity == city && millis() - cMs < 300000UL) { resolvedLoc = cLoc; return cText; }
  // %l 让 wttr 顺带报告它定位到的地名：IP 定位经常错城，把定位透出来错不错一眼可见
  String path = "/" + city + "?format=%l|%C|%t&lang=zh";
  String enc; char buf[8];
  for (unsigned int i = 0; i < path.length(); i++) {
    char c = path[i];
    if (isalnum((unsigned char)c) || strchr("-_.~/?=&+", c)) enc += c;   // "+" 是 wttr 格式串分隔符，不能转义
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); enc += buf; }
  }
  String body = httpGetText("wttr.in", enc, 6000);
  body.trim();
  int nl = body.indexOf('\n');
  if (nl > 0) body = body.substring(0, nl);
  body.trim();
  if (!body.length()) return "";
  // 形如 "Moderate rain at times\t+24°C"（注意 wttr 用制表符分隔，不能只按空格切）
  resolvedLoc = "";
  String temp, cond;
  int b1 = body.indexOf('|');
  if (b1 >= 0) {                                 // 新格式（%l 可能返回空）：[loc]|cond|temp
    int b2 = body.indexOf('|', b1 + 1);
    if (b2 > b1) {
      resolvedLoc = body.substring(0, b1);
      int cm = resolvedLoc.indexOf(',');         // "Kunshan, Jiangsu, China" 取首段
      if (cm > 0) resolvedLoc = resolvedLoc.substring(0, cm);
      resolvedLoc.trim();
      cond = body.substring(b1 + 1, b2);
      temp = body.substring(b2 + 1);
      temp.trim(); cond.trim();
      if (temp.length()) { temp.replace("+", ""); temp.replace("°C", "度"); }
      String out = cnWeather(cond);
      if (temp.length()) out += " " + temp;
      cCity = city; cText = out; cLoc = resolvedLoc; cMs = millis();
      return out;
    }
  }
  // 兜底：无 '|' 的旧式响应（°C 解析）
  cond = body;
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
  cCity = city; cText = out; cLoc = resolvedLoc; cMs = millis();
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
    // 城市被问句壳吃光（"今天天气怎么样"/录音截断丢了城市）→ 用配置的城市兜底。
    // 空城市原先直接跳过 Open-Meteo 去走 wttr 的出口 IP 定位——wttr 又不稳，
    // 这就是"配置了城市还是老查不到"的主路之一
    if (!city.length() && cfg.s.city[0]) city = cfg.s.city;
    Serial.printf("[TOOLS] weather: 城市「%s」\n", city.length() ? city.c_str() : "(按出口IP定位)");
    String say;
    String cond;
    float tempC = 0;
    // 主源 Open-Meteo（数据新鲜准确）；wttr.in 数据源陈旧，只作兜底
    if (city.length() && weatherOpenMeteo(city, cond)) {
      say = city + "现在" + cond;
    } else {
      String loc;
      cond = weatherReport(city, loc);
      if (!cond.length()) return "网络这会儿不太顺，天气没查着，过会儿再问我一次吧。";
      if (city.length()) say = city + "现在" + cond;                          // 主人点的城市
      else if (cfg.s.city[0]) say = String(cfg.s.city) + "现在" + cond;       // 配置的城市
      else {
        say = String("你那边现在") + cond;
        // IP 定位模式下 %l 常是坐标（数字开头），透出来没意义；地名才值得展示
        if (loc.length() && (loc[0] < '0' || loc[0] > '9'))
          say += String("（wttr 定位到：") + loc + "，不准就在后台设置里填城市名）";
      }
    }
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

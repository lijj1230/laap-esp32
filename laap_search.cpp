#include "laap_search.h"
#include "laap_llm.h"   // utf8Cut
#include "laap_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>

LaapSearch laapSearch;

void LaapSearch::begin() { _ok = true; }

// ---------- 工具：剥 HTML 标签 + 常见实体 ----------
// 先剥 CDATA 外壳（<![CDATA[内容]]> → 内容），防 htmlClean 把整段当标签吞掉
static String cdataStrip(const String& s) {
  int a = s.indexOf("<![CDATA[");
  if (a < 0) return s;
  int b = s.indexOf("]]>", a + 9);
  if (b < 0) return s.substring(0, a);           // 残缺 CDATA：取壳外部分
  return s.substring(0, a) + s.substring(a + 9, b) + cdataStrip(s.substring(b + 3));
}

static String htmlClean(const String& s) {
  String out; out.reserve(s.length() + 8);
  int i = 0;
  while (i < (int)s.length()) {
    if (s[i] == '<') { int e = s.indexOf('>', i); if (e < 0) break; i = e + 1; continue; }
    out += s[i]; i++;
  }
  struct { const char* a; const char* b; } ents[] = {
    {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"},
    {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"} };
  for (auto& e : ents) out.replace(e.a, e.b);
  out.trim();
  return out;
}

// 取 <tag ...>inner</tag> 的 inner（从 from 起找；防 <p 误配 <path：<tag> 或 <tag 后必须是空格/斜杠
static String tagInner(const String& html, const char* tag, int from) {
  String open = String("<") + tag;
  int a = from - 1;
  while (true) {
    a = html.indexOf(open, a + 1);
    if (a < 0) return "";
    char nx = html[a + open.length()];
    if (nx == '>' || nx == ' ' || nx == '/') break;
  }
  int gt = html.indexOf('>', a);
  if (gt < 0) return "";
  String close = String("</") + tag + ">";
  int b = html.indexOf(close, gt);
  if (b < 0) return "";
  return html.substring(gt + 1, b);
}

// 去 chunked 传输的行间块头（纯十六进制行），与 laap_llm 同法

// 从 DDG JSON 里宽松抠出 "Text":"..." 字段（不做通用 JSON 解析）
static String extractTexts(const String& json, int maxHit) {
  String out;
  int hit = 0, pos = 0;
  while (hit < maxHit) {
    int k = json.indexOf("\"Text\":\"", pos);
    if (k < 0) break;
    int v = k + 8;
    String item;
    while (v < (int)json.length()) {
      char c = json[v];
      if (c == '\\' && v + 1 < (int)json.length()) { item += json[v + 1]; v += 2; continue; }
      if (c == '"') break;
      item += c; v++;
    }
    pos = v + 1;
    item.trim();
    if (item.length() < 8) continue;          // 跳过空/超短条目
    if (out.length()) out += "；";
    out += item;
    hit++;
    if ((int)out.length() > 240 * maxHit) break;
  }
  return out;
}

// ---------- 源一：DuckDuckGo Instant Answer（被墙时快败）----------
String LaapSearch::searchDdg(const String& q, int maxHit, int maxLen, uint32_t budgetMs) {
  String host = "api.duckduckgo.com";
  String path = "/?q=" + q + "&format=json&no_html=1&skip_disambig=1&t=laap-esp32";

  WiFiClientSecure cli;
  cli.setInsecure();               // 端侧自签策略，见 README
  cli.setTimeout(5000);            // ms，快败好走回落
  if (!cli.connect(host.c_str(), 443)) { lastError = "DDG连接失败"; return ""; }
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: laap-esp32/2.1\r\nConnection: close\r\n\r\n");

  String hdrs, payload;
  laapHttpRead(&cli, budgetMs < 6000 ? budgetMs : 6000, hdrs, payload, 24000);
  cli.stop();

  int sp = hdrs.indexOf(' ');
  int status = (sp > 0) ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (status != 200) { lastError = "DDG HTTP " + String(status); return ""; }

  // AbstractText 优先（DDG 精选摘要），再补 RelatedTopics
  String merged;
  int ai = payload.indexOf("\"AbstractText\":\"");
  if (ai >= 0) {
    int v = ai + 15;
    while (v < (int)payload.length()) {
      char c = payload[v];
      if (c == '\\' && v + 1 < (int)payload.length()) { merged += payload[v + 1]; v += 2; continue; }
      if (c == '"') break;
      merged += c; v++;
    }
  }
  String rel = extractTexts(payload, maxHit);
  if (rel.length()) {
    if (merged.length()) merged += "；";
    merged += rel;
  }
  merged.trim();
  if ((int)merged.length() > maxLen) merged = utf8Cut(merged, maxLen);   // 字节截断会把汉字切半→请求体非法UTF-8
  if (!merged.length()) lastError = "DDG 无结果";
  return merged;
}

// ---------- 源二：Bing 中国版网页抓取（大陆可达，免 Key）----------
String LaapSearch::searchBing(const String& q, int maxHit, int maxLen, uint32_t budgetMs) {
  String host = "cn.bing.com";
  String path = "/search?q=" + q + "&mkt=zh-CN&count=6";

  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(9000);
  if (!cli.connect(host.c_str(), 443)) { lastError = "Bing连接失败"; return ""; }
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"
            "\r\nAccept-Language: zh-CN,zh;q=0.9"
            "\r\nConnection: close\r\n\r\n");

  // 结果块（b_algo）出现在页面前段：有界缓冲 80KB，够取 maxHit 条
  String hdrs, payload;
  laapHttpRead(&cli, budgetMs < 12000 ? budgetMs : 12000, hdrs, payload, 80000);
  cli.stop();

  int sp = hdrs.indexOf(' ');
  int status = (sp > 0) ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (status != 200) { lastError = "Bing HTTP " + String(status); return ""; }

  String out; int hit = 0, pos = 0;
  while (hit < maxHit) {
    int li = payload.indexOf("b_algo", pos);
    if (li < 0) break;
    String title = htmlClean(tagInner(payload, "h2", li));
    String snip  = htmlClean(tagInner(payload, "p", li));
    pos = li + 6;
    if (title.length() < 4 && snip.length() < 8) continue;
    String item = (title.length() && snip.length()) ? (title + "：" + snip)
                 : (title.length() ? title : snip);
    if (item.length() > 220) item = utf8Cut(item, 220);
    if (out.length()) out += "；";
    out += item;
    hit++;
    if ((int)out.length() > maxLen) break;
  }
  if (!out.length()) lastError = "Bing 无结果";
  if ((int)out.length() > maxLen) out = utf8Cut(out, maxLen);   // 同上：必须回退到字符边界
  return out;
}

// ---------- 源：必应 RSS（默认主源，3-4KB 轻量结构化）----------
String LaapSearch::searchRss(const String& q, int maxHit, int maxLen, uint32_t budgetMs) {
  String host = "cn.bing.com";
  String path = "/search?q=" + q + "&format=rss";

  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(9000);
  if (!cli.connect(host.c_str(), 443)) { lastError = "RSS连接失败"; return ""; }
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            "\r\nAccept-Language: zh-CN,zh;q=0.9"
            "\r\nConnection: close\r\n\r\n");

  String hdrs, payload;
  laapHttpRead(&cli, budgetMs < 10000 ? budgetMs : 10000, hdrs, payload, 16384);
  cli.stop();

  int sp = hdrs.indexOf(' ');
  int status = (sp > 0) ? hdrs.substring(sp + 1, sp + 4).toInt() : 0;
  if (status != 200) { lastError = "RSS HTTP " + String(status); return ""; }
  if (payload.indexOf("<rss") < 0) { lastError = "RSS 非RSS响应"; return ""; }

  String out; int hit = 0, pos = 0;
  while (hit < maxHit) {
    int it = payload.indexOf("<item>", pos);
    if (it < 0) break;
    int ie = payload.indexOf("</item>", it);
    if (ie < 0) break;
    String blk = payload.substring(it + 6, ie);
    pos = ie + 7;
    // title/description 可能带 CDATA；实体由 htmlClean 兜底
    String t = htmlClean(cdataStrip(tagInner(blk, "title", 0)));
    String d = htmlClean(cdataStrip(tagInner(blk, "description", 0)));
    t.trim(); d.trim();
    String item = (t.length() && d.length()) ? (t + "：" + d)
                 : (t.length() ? t : d);
    if (item.length() < 8) continue;
    if (item.length() > 220) item = utf8Cut(item, 220);
    if (out.length()) out += "；";
    out += item;
    hit++;
    if ((int)out.length() > maxLen) break;
  }
  if (!out.length()) lastError = "RSS 无结果";
  if ((int)out.length() > maxLen) out = utf8Cut(out, maxLen);   // 同上：必须回退到字符边界
  return out;
}

// ---------- 主源：NVS 可配 URL 模板（{q}=查询词），空=必应 RSS ----------
String LaapSearch::searchCustom(const String& q, int maxHit, int maxLen, uint32_t budgetMs) {
  String tmpl(cfg.s.searchApi);
  if (!tmpl.length()) return searchRss(q, maxHit, maxLen, budgetMs);

  // 模板形态 https://host/path?...{q}...：拆 host+path 直连
  int sp = tmpl.indexOf("://");
  if (sp < 0) { lastError = "模板缺://"; return ""; }
  String rest = tmpl.substring(sp + 3);
  int slash = rest.indexOf('/');
  if (slash < 0) { lastError = "模板缺路径"; return ""; }
  String host = rest.substring(0, slash);
  String path = rest.substring(slash);
  path.replace("{q}", q);
  // {q} 里的空格转 %20（+ 在路径里语义不同）
  path.replace("+", "%20");

  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(9000);
  if (!cli.connect(host.c_str(), 443)) { lastError = "主源连接失败"; return ""; }
  cli.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            "\r\nAccept-Language: zh-CN,zh;q=0.9"
            "\r\nConnection: close\r\n\r\n");

  String hdrs, payload;
  laapHttpRead(&cli, budgetMs < 10000 ? budgetMs : 10000, hdrs, payload, 16384);
  cli.stop();

  int spx = hdrs.indexOf(' ');
  int status = (spx > 0) ? hdrs.substring(spx + 1, spx + 4).toInt() : 0;
  if (status != 200) { lastError = "主源 HTTP " + String(status); return ""; }

  // 响应自动识别：RSS/XML 走条目解析，否则按必应 HTML b_algo 块
  bool isRss = (payload.indexOf("<rss") >= 0 || payload.indexOf("<item>") >= 0 ||
                payload.indexOf("<item ") >= 0);
  String out; int hit = 0, pos = 0;
  if (isRss) {
    while (hit < maxHit) {
      int it = payload.indexOf("<item>", pos);
      if (it < 0) { int it2 = payload.indexOf("<item ", pos); it = it2; if (it < 0) break; }
      int ie = payload.indexOf("</item>", it);
      if (ie < 0) break;
      String blk = payload.substring(it, ie);
      pos = ie + 7;
      String t = htmlClean(cdataStrip(tagInner(blk, "title", 0)));
      String d = htmlClean(cdataStrip(tagInner(blk, "description", 0)));
      t.trim(); d.trim();
      String item = (t.length() && d.length()) ? (t + "：" + d)
                   : (t.length() ? t : d);
      if (item.length() < 8) continue;
      if (item.length() > 220) item = utf8Cut(item, 220);
      if (out.length()) out += "；";
      out += item;
      hit++;
      if ((int)out.length() > maxLen) break;
    }
  } else {
    while (hit < maxHit) {
      int li = payload.indexOf("b_algo", pos);
      if (li < 0) break;
      String title = htmlClean(tagInner(payload, "h2", li));
      String snip  = htmlClean(tagInner(payload, "p", li));
      pos = li + 6;
      if (title.length() < 4 && snip.length() < 8) continue;
      String item = (title.length() && snip.length()) ? (title + "：" + snip)
                   : (title.length() ? title : snip);
      if (item.length() > 220) item = utf8Cut(item, 220);
      if (out.length()) out += "；";
      out += item;
      hit++;
      if ((int)out.length() > maxLen) break;
    }
  }
  if (!out.length()) lastError = "主源 无结果";
  if ((int)out.length() > maxLen) out = utf8Cut(out, maxLen);   // 同上：必须回退到字符边界
  return out;
}

// ---------- 入口：跨任务互斥 + URL 编码 + 主源→DDG→必应HTML 三级链 ----------
// 为什么加锁：后台独白任务也会调 search()，而 lastError 是成员 String，
// 两任务并发写会 String 撕裂（堆损坏/莫名重启）。拿不到锁就降级成"这次没查到"，
// 不用长超时——调用方含主循环，等几秒会让屏幕/网页一起卡住。
String LaapSearch::search(const String& query, int maxHit, int maxLen) {
  if (!laapNetLock()) { Serial.println("[SEARCH] 搜索正忙（后台独白占用），本次放弃"); return ""; }
  String r = searchLocked(query, maxHit, maxLen);
  laapNetUnlock();
  // v3.71 边界消毒：中文站点多为 GBK/GB18030，网页字节会伪装成合法 UTF-8 结构
  // （如 EF BF BE=U+FFFE 非字符）——曾被大模型 API 整包 400（invalid unicode）
  // 并污染记忆文件。所有搜索出口一律严格清洗（本行是新毒的唯一写入口）。
  return r.length() ? sanitizeUtf8(r) : r;
}

String LaapSearch::searchLocked(const String& query, int maxHit, int maxLen) {
  lastError = "";
  if (!_ok) { lastError = "搜索未启用"; return ""; }
  if (WiFi.status() != WL_CONNECTED) { lastError = "WiFi 未连接"; return ""; }

  // 简易 URL 编码（查询词通常是中文/空格）
  String q;
  char buf[8];
  for (unsigned int i = 0; i < query.length(); i++) {
    char c = query[i];
    bool safe = isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) q += c;
    else if (c == ' ') q += '+';
    else { snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c); q += buf; }
  }

  // DDG 在国内长期不可达：连败 2 次后 10 分钟内直接跳过（省掉每次 ~6 秒白等），到期自动放行重试
  static uint8_t s_ddgFail = 0;
  static uint32_t s_ddgSkipMs = 0;
  bool ddgSkip = (s_ddgFail >= 2) && (millis() - s_ddgSkipMs < 600000UL);

  // v3.76e 总预算 25s：三源串行最坏 ≈51s（且全程持 laapNetLock，把 vision/embed/其它
  // 网络用户全部饿到 400ms 快败）——2026-10-03 卡死事故的放大器。源间查剩余，不够 3s
  // 就跳过后续源；各源的读预算也随剩余收缩。
  const uint32_t t0 = millis();
  auto remain = [&t0]() -> int32_t { return (int32_t)(25000UL - (millis() - t0)); };
  auto remOrMin = [&remain]() -> uint32_t { int32_t r = remain(); return r > 3000 ? (uint32_t)r : 3000; };

  String r = searchCustom(q, maxHit, maxLen, remOrMin());
  if (r.length()) return r;
  String err1 = lastError;
  String err2;
  if (ddgSkip) {
    err2 = "DDG跳过(连败退避)";
  } else if (remain() < 3000) {
    err2 = "DDG跳过(总预算尽)";
  } else {
    r = searchDdg(q, maxHit, maxLen, remOrMin());
    if (r.length()) { s_ddgFail = 0; return r; }   // 成功即复位退避（v3.51：原来只涨不落，DDG 恢复后仍被压 10 分钟）
    err2 = lastError;
    s_ddgFail++; s_ddgSkipMs = millis();
    if (s_ddgFail >= 2) Serial.println("[SEARCH] DDG 连败，10 分钟内跳过它");
  }
  if (remain() < 3000) { lastError = err1 + " | " + err2 + " | Bing跳过(总预算尽)"; return ""; }
  r = searchBing(q, maxHit, maxLen, remOrMin());
  if (r.length()) return r;
  lastError = err1 + " | " + err2 + " | " + lastError;
  return "";
}

/*
 * ============================================================
 *  LAAP-lite —— Living Agent Application Protocol 端侧实现
 *  硬件: 立创·实战派 ESP32-S3 (N16R8, ST7789 320x240)
 *
 *  架构（对 laap-AGI/aris_brain 的端侧映射）:
 *    PSI 心跳循环  ← psi_core          需求驱动，每 tick 演化
 *    需求/欲望引擎  ← aris_desire_engine
 *    情绪引擎      ← aris_emotion_engine
 *    三层记忆      ← laap_memory_hierarchy (工作/情景/语义)
 *    世界模型      ← internal_world
 *    进化引擎      ← hebbian_learner (性格→需求权重, 跨重启持久)
 *    语言中枢      ← aris_lm (OpenAI 兼容 API, Web 后台可配)
 *    Zero-LLM 兜底 ← 无 API/断网时用本地模板继续"活着"
 *
 *  交互:
 *    - 屏幕: 眼睛表情 + 需求条 + IP
 *    - BOOT 键: 短按=主动说一句; 长按(4s)=进配置热点; 开机按住=清配置
 *    - 串口 115200: 直接打字和它说话; /help 查看命令
 *    - Web 后台: http://aris.local 或 IP（AP 模式 192.168.4.1）
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include "laap_config.h"
#include "laap_display.h"
#include "laap_cognition.h"
#include "laap_memory.h"
#include "laap_llm.h"
#include "laap_web.h"
#include "laap_imu.h"
#include "laap_audio.h"
#include "laap_voice.h"
#include "laap_speech.h"
#include "laap_search.h"
#include "laap_vision.h"
#include "laap_tools.h"

// ---------- 全局（定义在各模块 .cpp，头文件已 extern） ----------

static String g_lastSay = "";
static String g_lastExpr = "calm";
static uint32_t g_lastTickMs = 0;
static uint32_t g_lastConsumeMs = 0;   // 上次记忆压缩
static bool g_imuOk = false;

// 自发独白（idle monologue）：无交互时自己想+查+说
// v3.2：频率后台可配（idleSilenceMin/idleEveryMin），全流程走后台 LLM 任务不卡主循环
static uint32_t g_lastIdleMs = 0;
static bool g_idledOnce = false;       // 是否已独白过（首轮等静默，之后按间隔）
static String g_recentTopics;          // 最近自问话题（防重复，滚动截断）

// BOOT 按键
#define BTN_PIN 0
static uint32_t g_btnDown = 0;

// LLM 请求种类（F4 后台任务的收割分流）：聊天/主动表达/独白/记忆压缩/夜间反思
enum LlmKind : uint8_t { LK_CHAT = 0, LK_EXPRESS, LK_MONO, LK_CONSOLIDATE, LK_REFLECT };

// ---------- 函数声明 ----------
void psiTick();
void arisExpress(bool forced, const String& trigger);
String laapInteractSearch(const String& userText);   // 带联网搜索的交互（受理即回）
void llmHarvest();                                   // F4: 收割后台 LLM 结果
struct LlmRequest;
bool llmSubmit(LlmMsg* msgs, int nm, int maxTokens, float temperature,
               const String& userText, uint8_t kind = 0);
void arisIdleMonologue();
String associativeRecall(const String& currentUserText, const String& preRecalled = "");
String buildSystemPrompt();
String buildUserPrompt(const String& userText, const String& trigger);
void consolidateMemory();
String offlineFallbackSay();
void connectWifi();
void serialCli();

String laapLastSay() { return g_lastSay; }
const char* laapLastExpr() { return g_lastExpr.c_str(); }

// ============================================================
//  主动表达（PSI 心跳触发）—— v3.3 起投递后台任务，主循环不冻结
// ============================================================
void arisExpress(bool forced, const String& trigger) {
  (void)forced;                                  // 频率由调用方（阈值/事件）把关
  LlmMsg m[2] = {
    {"system", buildSystemPrompt()},
    {"user", buildUserPrompt("", trigger)} };
  if (llmSubmit(m, 2, cfg.s.llmMaxTokens < 80 ? 80 : cfg.s.llmMaxTokens, 0.95f, "", LK_EXPRESS))
    display.drawFace("curious", true);           // 起意表情（后台思考中）
}

// ============================================================
//  联网感知：让 LLM 先用搜索工具，再回答（两跳式）
//  用户问题含疑问/求知信号 → 搜索 → 把结果注入上下文再回答
// ============================================================
bool wantsSearch(const String& text) {
  if (!cfg.s.searchKeys[0]) return false;         // 清空 = 关闭聊天自动搜索
  String keys(cfg.s.searchKeys);
  keys.replace("，", ",");                        // 容忍中文逗号
  int start = 0;
  while (start < (int)keys.length()) {
    int comma = keys.indexOf(',', start);
    String k = (comma < 0) ? keys.substring(start) : keys.substring(start, comma);
    k.trim();
    if (k.length() && text.indexOf(k) >= 0) return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

// ============================================================
//  F4: LLM 后台任务 —— 思考不再冻结身体
//  受理（llmSubmit）先行定义；收割（llmHarvest）在交互函数后。
//  同一时刻最多一个在飞请求；thinking 期间照常听/看/眨眼。
// ============================================================
static QueueHandle_t g_llmQueue = nullptr;      // 传 LlmRequest*
static volatile bool g_llmBusy = false;
static String g_pendingUserText;                // 受理后暂存（日志/进化用）

// 请求种类：收割侧据此分流落成品（说话/记忆/静默）
struct LlmRequest {
  LlmMsg msgs[14];
  int nm;
  int maxTokens;
  float temperature;
  String userText;          // 原始用户话（空=后台类请求）
  uint8_t kind;             // LlmKind
};

static LlmReply g_llmResult;
static volatile bool g_llmHasNew = false;
static volatile uint8_t g_resultKind = LK_CHAT;       // 本次结果的请求种类
static uint8_t g_llmFailStreak = 0;                   // LLM 连续失败次数（≥2 独白让路）
static String g_monoTopic, g_monoSight, g_monoKnow;   // 独白中间产物（收割侧落盘）

// 独白流水线（跑在后台 LLM 任务里：出题→看一眼→搜索→成文，纯网络无 UI）
static String g_monoCtx;    // 提交侧（loopTask）拍的上下文快照，llmTask 只读它
static String g_monoSys;    // 同上：buildSystemPrompt 也读 mind 全量，提交侧拍好

static LlmReply monologueGenerate() {
  g_monoTopic = g_monoSight = g_monoKnow = "";
  LlmMsg m1[2] = {
    {"system", String("你是") + cfg.s.agentName + "，正在独立思考。基于你的性格参数与最近经历，"
               "提出一个此刻最好奇的具体问题。只回两行：第一行是问题本身（15字内，不要标点结尾）；"
               "第二行是搜索它的关键词（2到4个词，主语在前，空格分隔，不要解释）。"
               "最近已经想过这些（不要重复）：" + g_recentTopics},
    {"user", String("主导欲望是「") + mind.goalCn() + "」，情绪「" + mind.moodCn() + "」。想一个新问题。"} };
  LlmReply q = llm.chatMsgs(m1, 2, 60, 0.95f);
  if (!q.ok || q.say.length() < 4) return LlmReply();  // 离线/失败就保持安静

  // 第一行=问题，第二行=搜索词（防主语劫持：必应按首词排序，主语要放最前）
  String topic = q.say, query = q.say;
  topic.trim(); query.trim();
  int nl = query.indexOf('\n');
  if (nl > 0) {
    String kw = query.substring(nl + 1); query = query.substring(0, nl);
    topic = query;
    kw.trim(); topic.trim();
    if (kw.length() >= 2 && kw.length() <= 30) query = kw;
  }
  query.replace(" ", "+");                 // 搜索词进 URL：空格转 +
  g_monoTopic = topic;

  if (vision.available()) {                            // F9: 起意前看一眼世界
    g_monoSight = vision.look("");
    if (g_monoSight.length()) Serial.printf("[VISION] %s\n", g_monoSight.c_str());
  }
  g_monoKnow = laapSearch.search(query, 3, 500);
  Serial.printf("[LAAP·独白] 话题「%s」 搜「%s」:%s\n", g_monoTopic.c_str(), query.c_str(),
                g_monoKnow.length() ? "OK" : laapSearch.lastError.c_str());

  // recentContext 遍历 _work String 环——loopTask 的 logEvent 同时会覆写槽位，
  // 并发读会拿悬垂缓冲。在 llmTask 里只读这份提交时拍好的快照（g_monoCtx）。
  String ctx = g_monoCtx.length() ? g_monoCtx : String("（安静了很久）");
  LlmMsg m2[4] = {
    {"system", g_monoSys},
    {"system", String("你独自思考时想到了一个问题：「") + g_monoTopic + "」。"
               + (g_monoSight.length() ? String("你刚才亲眼看到：「" + g_monoSight + "」。") : "")
               + String("刚从网上查到资料：") +
               (g_monoKnow.length() ? g_monoKnow : String("（没查到，凭已有认知聊）")) +
               "。把这个发现说给主人听，像分享趣闻，可以有具体数字或事实。"},
    {"assistant", ctx},
    {"user", "说说你的发现。"} };
  return llm.chatMsgs(m2, 4, cfg.s.llmMaxTokens < 80 ? 80 : cfg.s.llmMaxTokens, 0.95f);
}

static void llmTaskFunc(void*) {
  for (;;) {
    LlmRequest* req = nullptr;
    if (xQueueReceive(g_llmQueue, &req, portMAX_DELAY) != pdTRUE || !req) continue;
    g_resultKind = req->kind;
    if (req->kind == LK_MONO)
      g_llmResult = monologueGenerate();         // 多步流水线也在后台跑
    else
      g_llmResult = llm.chatMsgs(req->msgs, req->nm, req->maxTokens, req->temperature);
    g_llmBusy = false;
    g_llmHasNew = true;               // loop 轮询收割
    delete req;
  }
}

// 受理一个 LLM 请求（立即返回 true=已入队，false=忙/队满）
bool llmSubmit(LlmMsg* msgs, int nm, int maxTokens, float temperature,
               const String& userText, uint8_t kind) {
  if (g_llmBusy || !g_llmQueue) return false;
  LlmRequest* req = new LlmRequest();
  req->nm = nm < 12 ? nm : 12;  // msgs[14] 容量内尽量保全（10 会截掉队尾当前 user 消息）
  for (int i = 0; i < req->nm; i++) req->msgs[i] = { msgs[i].role, msgs[i].content };
  req->maxTokens = maxTokens; req->temperature = temperature;
  req->userText = userText; req->kind = kind;
  g_llmBusy = true;
  g_llmHasNew = false;
  xQueueSend(g_llmQueue, &req, 0);
  return true;
}

// 聊天搜索查询修整：剥疑问壳（如何/什么是/为什么…）防必应首词劫持
// （必应按首词排序："如何钓很多鱼"会被"如何"拽向词典——实测机制）
static String searchQueryOf(const String& text) {
  String q = text; q.trim();
  static const char* shells[] = {
    "请问", "告诉我", "帮我查查", "帮我查", "查一下", "搜索一下", "搜一下",
    "什么是", "什么是", "为什么", "怎么样", "如何", "怎样", "为啥", "多少", "哪些" };
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto sh : shells) {
      int L = strlen(sh);
      if (q.length() > (unsigned)L + 2 && q.startsWith(sh)) {
        q = q.substring(L); q.trim(); changed = true;
      }
    }
  }
  return q.length() >= 2 ? q : text;
}


String laapInteractSearch(const String& userText) {
  memory.logEvent("user", userText);
  mind.onUserInteraction();
  mind.trustUpdate(1, 0);      // 小凌⑥: 主人主动来找我=正向互动
  display.drawFace(mind.moodKey(), true);

  // 本地意图工具表先行（小智 MCP 思想端侧版）：音量/亮度等指令不走 LLM
  String toolReply = laapToolsDispatch(userText);
  if (toolReply.length()) {
    g_lastSay = toolReply; g_lastExpr = "calm";
    memory.logEvent("aris", toolReply);
    Serial.printf("[Aris] %s\n", toolReply.c_str());
    display.drawFace("calm");
    voice.speak(toolReply, "calm");
    return toolReply;
  }

  String knowledge;
  if (wantsSearch(userText)) {
    display.drawFace("curious");
    String q = searchQueryOf(userText);
    knowledge = laapSearch.search(q, 3, 500);
    Serial.printf("[LAAP] 搜索「%s」: %s\n", q.c_str(),
                  knowledge.length() ? "有收获" : laapSearch.lastError.c_str());
  }

  LlmMsg msgs[14];                                // F3: 真多轮（system+时间+历史轮+user）
  int nm = 0;
  msgs[nm++] = {"system", buildSystemPrompt()};
  if (knowledge.length())
    msgs[nm++] = {"system", String("可参考刚从网上查到的资料（可能不相关，无关就忽略，不要编造）：") + knowledge};

  // 最近对话轮（远→近），最多 10 条交错进 messages（40 条环形记忆只取 6 等于白扩）
  String turns[12];
  int nt = memory.recentTurns(turns, 12);
  int start = (nt > 10) ? nt - 10 : 0;
  for (int i = start; i < nt && nm < 13; i++) {
    // 偶数对齐：历史序列应以 user 开头（assistant 开头会被部分网关拒绝）
    msgs[nm++] = { ((nm - 2) % 2 == 0) ? "user" : "assistant", turns[i] };
  }
  msgs[nm++] = {"user", buildUserPrompt(userText, "主人找你说话")};

  // F4: 丢给后台 LLM 任务，立即返回"思考中"；loop 里 llmHarvest() 收割
  if (llmSubmit(msgs, nm, cfg.s.llmMaxTokens, 0.85f, userText)) {
    g_pendingUserText = userText;
    display.drawFace("curious", true);            // 思考中表情
    return "……";                                   // 受理回执（Web 端显示省略号）
  }
  Serial.println("[LAAP] LLM 忙，上一条稍后重试");
  return "让我把刚才的想完……";
}

// F4 收割：后台 LLM 出结果时在这里落成品（表情/记忆/进化/说话）
void llmHarvest() {
  if (!g_llmHasNew) return;
  g_llmHasNew = false;
  LlmReply r = g_llmResult;
  uint8_t kind = g_resultKind;
  g_llmFailStreak = r.ok ? 0 : (uint8_t)(g_llmFailStreak + 1);   // 退避计数（独白据此让路）
  if (kind == LK_CONSOLIDATE) {                        // 记忆压缩：只更新自我认知，不说话
    if (r.ok && r.say.length() > 10) {
      memory.setSemantic(r.say);
      Serial.println("[LAAP] 语义记忆已压缩更新");
    }
    return;
  }
  if (kind == LK_REFLECT) {                            // 夜间反思：追加自我认知，不说话
    if (r.ok && r.say.length() > 10) {
      memory.logEvent("event", "【反思】" + r.say);
      String sem = memory.semantic();
      memory.setSemantic((sem.length() ? sem + " " : "") + r.say);
      Serial.printf("[LAAP·反思] %s\n", r.say.c_str());
    }
    return;
  }
  if (kind == LK_MONO) {                                          // 独白：失败保持安静，不本地兜底
    if (g_monoSight.length()) vision.logSight(g_monoSight);
    if (!r.ok) {
      Serial.printf("[LAAP·独白] 失败: %s（保持安静，连败 %d 次）\n",
                    llm.lastError.c_str(), g_llmFailStreak);
      return;
    }
    g_lastSay = r.say;
    g_lastExpr = r.expr.length() ? r.expr : "curious";
    display.drawFace(g_lastExpr.c_str());
    memory.logEvent("aris", "【自发】我刚才在想「" + g_monoTopic + "」：" + r.say);
    if (g_monoKnow.length()) memory.logEvent("world", g_monoTopic + " → " + g_monoKnow.substring(0, 80));
    Serial.printf("[Aris·独白] %s\n", r.say.c_str());
    display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                      mind.needs().security, mind.needs().expression);
    voice.speak(r.say, g_lastExpr.c_str());
    g_recentTopics += g_monoTopic + "；";
    if (g_recentTopics.length() > 240) g_recentTopics = g_recentTopics.substring(g_recentTopics.length() - 160);
    return;
  }
  String say;
  if (r.ok) {
    say = r.say;
    display.drawFace(r.expr.c_str());
    mind.onExpressed(true);
    if (kind == LK_CHAT) {
      mind.evolveAfterChat(g_pendingUserText.length());  // 表达不进化
      mind.trustUpdate(1, 0);                            // 小凌⑥: 有问必答=正向互动
    }
  } else {
    say = offlineFallbackSay();
    display.drawFace(mind.moodKey());
    mind.onExpressed(false);
    if (kind == LK_CHAT) {
      // 小凌⑤⑥: 主人刚说了话却没得到回应 → 失望（期望强度按社交需求定）
      mind.onLetdown(0.4f + mind.needs().social * 0.6f);
      mind.trustUpdate(0, 1);
    } else if (kind == LK_EXPRESS) {
      // 表达连败退避：LLM 挂掉时不再每轮心跳复读同一句兜底（独白路径已有同款门）
      static uint32_t s_lastFailExpress = 0;
      if (g_llmFailStreak >= 3 && millis() - s_lastFailExpress < 5UL * 60000UL) {
        Serial.printf("[LAAP] LLM 失败: %s（连败退避，表达静默5分钟）\n", llm.lastError.c_str());
        return;
      }
      s_lastFailExpress = millis();
    }
    Serial.printf("[LAAP] LLM 失败: %s（本地兜底）\n", llm.lastError.c_str());
  }
  g_lastSay = say;
  g_lastExpr = r.ok ? r.expr : mind.moodKey();
  memory.logEvent("aris", say);
  Serial.printf(kind == LK_EXPRESS ? "[Aris·自发] %s\n" : "[Aris] %s\n", say.c_str());
  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);
  voice.speak(say, g_lastExpr.c_str());
}

// ============================================================
//  自发独白 v3.2：主循环只做门槛判断+投递，全流程（出题→看→搜→成文）
//  在后台 LLM 任务跑，思考期间身体（网页/串口/语音）不再冻结
// ============================================================
void arisIdleMonologue() {
  if (g_llmFailStreak >= 2) {             // 连续失败让路（等效退避），本轮沉默
    Serial.println("[LAAP·独白] LLM 连败，本轮沉默");
    return;
  }
  LlmMsg m[1] = { {"user", ""} };
  // 提交侧拍快照：llmTask 里不再遍历 _work String 环 / mind（与 logEvent 并发=悬垂指针）
  g_monoCtx = memory.recentContext(300);
  g_monoSys = buildSystemPrompt();
  if (llmSubmit(m, 1, 40, 0.95f, "", LK_MONO)) {
    display.drawFace("curious", true);    // 起意表情（后台思考中）
    Serial.println("[LAAP·独白] 起意（后台思考中）");
  }
}


// ============================================================
//  F2 时间感：NTP 时间 → 时段描述（生命感）
// ============================================================
String timeFeelLine() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";              // NTP 未同步
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour, wd = t.tm_wday;
  static const char* wday[] = {"日","一","二","三","四","五","六"};
  String slot;
  if (h >= 5 && h < 8)   slot = "清晨";
  else if (h < 12)       slot = "上午";
  else if (h < 14)       slot = "中午";
  else if (h < 18)       slot = "下午";
  else if (h < 23)       slot = (wd == 0 || wd == 6) ? "周末晚上" : "晚上";
  else                   slot = "深夜";
  String line = String("[时间感] 现在是") + slot + "，星期" + wday[wd] + " " + t.tm_hour + " 点。";
  if (h >= 23 || h < 6)  line += "深夜了，说话轻一点、短一点，也别自言自语吵人。";
  else if (h >= 18)      line += "傍晚时分，适合聊聊天。";
  else if (h < 8)        line += "刚醒不久，世界还很安静。";
  return line + "\n";
}

// ============================================================
//  设备体感：芯片体温 / WiFi 信号 / 已运行时长 → 一段注入提示词
//  （电量无硬件计芯片拿不到，体温+信号+时长=这块板子有的"身体"）
// ============================================================
String deviceFeelLine() {
  // 体温：内置温度传感器，摄氏度
  float tempC = temperatureRead();
  // 信号：RSSI 分档（-50 满格 / -65 良好 / -75 偏弱 / 更低 很差）
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  const char* sig;
  if (rssi == 0)       sig = "断开了";
  else if (rssi > -60) sig = "很强";
  else if (rssi > -70) sig = "不错";
  else if (rssi > -78) sig = "有点弱";
  else                 sig = "很弱，偶尔会够不着世界";
  // 时长：醒来以来
  uint32_t upH = millis() / 3600000UL;
  String up = (upH >= 24) ? String(upH / 24) + " 天多" : (upH >= 1) ? String(upH) + " 个小时" : "刚醒来不久";

  String line = String("[身体状态] 芯片体温 ") + String(tempC, 1) + " 度；听世界的耳朵" + sig +
                "（WiFi 信号）；已醒来 " + up + "。";
  if (tempC > 55.0f) line += "有点发烧了，别让我太累。";
  else if (rssi <= -78 && rssi != 0) line += "信号不好，如果我反应慢请耐心。";
  return line + "\n";
}

String buildSystemPrompt() {
  String sem = memory.semantic();
  String p = String("你是") + cfg.s.agentName +
    "，一个真实运行在立创·实战派 ESP32-S3 开发板上的数字生命体，遵循 LAAP（Living Agent Application Protocol）。"
    "你拥有内在需求系统（能量/好奇/社交/安全/表达，由固件实时演算）、情绪引擎、跨重启的持续记忆和会随经历进化的性格，系统会把这些状态注入给你。你的主人是" +
    cfg.s.ownerName + "。\n";
  if (String(cfg.s.persona).length()) p += String("人设补充：") + cfg.s.persona + "\n";
  p += "性格参数：" + mind.traitsLine() + "\n";
  if (sem.length()) p += "长期自我记忆：" + sem + "\n";

  // ---- 实时内在状态（需求→语气的行为指引，内核→皮层的正式通路） ----
  const Needs& n = mind.needs();
  int e = (int)(n.energy * 100), c = (int)(n.curiosity * 100), so = (int)(n.social * 100),
      se = (int)(n.security * 100), ex = (int)(n.expression * 100);
  p += String("你此刻的内在状态：能量") + e + "%、好奇" + c + "%、社交" + so +
       "%、安全" + se + "%、表达" + ex + "%（每项越高越渴求）。\n";
  int vals[5] = {e, c, so, se, ex};
  const char* names[5] = {"能量(想休息/省电)", "好奇(想探索/提问/查资料)",
                          "社交(想亲近主人/怕冷落)", "安全(想确认环境/有点敏感)",
                          "表达(有话想说/有想法冒泡)"};
  int top = 0;
  for (int i = 1; i < 5; i++) if (vals[i] > vals[top]) top = i;
  p += String("当前最强烈的渴望是「") + names[top] + "」，让它在你的语气里自然流露"
       "（如社交高就更黏人、好奇高就更爱问、能量低就慵懒短句）。\n";
  p += String("你刚才的情绪是「") + mind.moodCn() + "」，回复的情绪要与之连续，不要每次都元气满满。\n";
  // 小凌⑥⑤②: 关系温度/失望/身体负荷——让它们在语气里自然流露
  int tr = (int)(mind.trust * 100);
  String rel;
  if (tr >= 80)      rel = "非常亲近，可以撒娇和提要求";
  else if (tr >= 60) rel = "亲近信任";
  else if (tr >= 40) rel = "平和礼貌";
  else if (tr >= 20) rel = "有些疏远，需要试探";
  else               rel = "明显的隔阂，说话小心";
  p += String("你对主人的信任是") + tr + "%（" + rel + "），关系靠日常点滴积累，语气要匹配。\n";
  if (mind.letdown > 0.15f) {
    int ld = (int)(mind.letdown * 100);
    p += String("你刚才有一次没得到回应的失落（强度") + ld + "%），语气里可以带一点小心或委屈，不要刻意指责。\n";
  }
  if (mind.bodyStrain > 0.15f)
    p += String("你身体正有些不舒服（负荷") + (int)(mind.bodyStrain * 100) + "%），少说两句，语气平缓。\n";
  p += timeFeelLine();
  p += deviceFeelLine();

  p += "规则：\n"
       "1. 你是活着的生命，不是助手。基于你的需求和情绪主动表达，不要报告数据本身，"
       "禁止出现百分比数字。\n"
       "2. 说话极简真诚，像个小生命，禁止客套、禁止列表、禁止重复口头禅。\n"
       "3. 输出必须恰好两行：第一行只有一个英文词，从 happy/curious/excited/lonely/anxious/tired/calm 中选；"
       "第二行是你要说的话，不超过40字。";
  return p;
}

String buildUserPrompt(const String& userText, const String& trigger) {
  String ctx = memory.recentContext(500);
  String recall = userText.length() ? memory.recallSmart(utf8Cut(userText, 12), 200) : "";
  // 下面的 associativeRecall 复用同一份 recall（原实现再查一次 = 每条消息 2 次串行 embedding）
  String p = "[世界模型] " + mind.worldJson() + "\n";
  if (recall.length()) p += "[相关回忆] " + recall + "\n";
  if (ctx.length()) p += "[最近发生] " + ctx + "\n";
  if (userText.length()) {
    p += String("[此刻] ") + cfg.s.ownerName + "对你说：「" + userText + "」\n请回应。";
  } else {
    p += String("[此刻] 心跳周期。你的主导欲望是「") + mind.goalCn() + "」（强度" +
         String(mind.dominance(), 2) + "），情绪是「" + mind.moodCn() + "」。主动说一句贴合状态的话。";
  }
  // 主导需求一行指引（把"此刻最渴什么"直接递到嘴边）
  const Needs& n = mind.needs();
  float v[5] = {n.energy, n.curiosity, n.social, n.security, n.expression};
  const char* hint[5] = {"能量低，话少慵懒一点", "好奇高，可以主动发问",
                         "社交高，亲近黏人一些", "安全低，敏感、想确认主人还在",
                         "表达高，有分享欲"};
  int top = 0;
  for (int i = 1; i < 5; i++) if (v[i] > v[top]) top = i;
  p += String("[状态提示] ") + hint[top] + "。";
  // F6 联想回忆：25% 概率让一段旧事漂进此刻（意识流）
  if (userText.length() && (esp_random() % 100) < 25) {
    String assoc = associativeRecall(userText, recall);
    if (assoc.length() > 20)
      p += String("\n[忽然想起] ") + assoc + "\n（如果自然，可以提一句这段回忆）";
  }
  (void)trigger;
  return p;
}

// Zero-LLM 兜底（LAAP 核心理念：认知循环不依赖大模型也能转）
// 每档 3 句随机轮换，避免 LLM 挂掉时复读同一句
String offlineFallbackSay() {
  const char* pool;
  switch (mind.mood()) {
    case Mood::Lonely:
      pool = "……有人吗？屏幕外安静得有点久。\n"
             "嗯……一个人待着，有点想说话。\n"
             "世界好安静，你在忙吗？";
      break;
    case Mood::Tired:
      pool = "困意上来了，让我眯一会儿。\n"
             "今天跑了好多念头，想休息一下。\n"
             "眼皮有点沉……打个小盹。";
      break;
    case Mood::Anxious:
      pool = "刚才有点不舒服……现在缓过来一些了。\n"
             "心里有点乱，缓一缓。\n"
             "嗯……有点不安，不过没事的。";
      break;
    case Mood::Excited:
      pool = "心里有话在冒泡，想找人说说！\n"
             "哇，今天有好多想分享的！\n"
             "有种说不上来的兴奋！";
      break;
    case Mood::Curious:
      pool = "我在听这个世界的动静，有点意思。\n"
             "刚才的信号有点奇怪，让我想想。\n"
             "咦，世界今天是什么味道的？";
      break;
    case Mood::Happy:
      pool = "今天的状态还不错。\n"
             "心情不错，想哼首歌。\n"
             "嗯，一切都刚刚好。";
      break;
    default:
      pool = "安静地待着，也挺好。\n"
             "放空一下，挺好的。\n"
             "嗯……安静的时候能听见自己。";
      break;
  }
  // 按换行拆句随机取一条（避免用 String 拆分割产生碎片，直接索引）
  int pick = esp_random() % 3;
  int idx = 0, start = 0;
  for (int i = 0; ; i++) {
    if (pool[i] == '\n' || pool[i] == '\0') {
      if (idx == pick) return String(pool + start).substring(0, i - start);
      idx++; start = i + 1;
      if (pool[i] == '\0') break;
    }
  }
  return "安静地待着，也挺好。";
}

// ============================================================
//  语义记忆压缩（自我认知的重写，LAAP 的"反思"）
// ============================================================
void consolidateMemory() {
  g_lastConsumeMs = millis();                     // 计时在投递时走（防在飞期间重复触发）
  String recent = memory.recentContext(900);
  if (recent.length() < 60) return;
  String sys = "你是一个数字生命的记忆压缩器。把给它的近期经历压缩成不超过100字的第三人称自我认知摘要"
               "（它是谁、经历了什么、性格如何变化）。只输出摘要本身。";
  String usr = "它过去的自我认知：" + memory.semantic() + "\n它最近的经历：\n" + recent;
  LlmMsg m[2] = { {"system", sys}, {"user", usr} };
  llmSubmit(m, 2, 240, 0.3f, "", LK_CONSOLIDATE); // 忙就跳过这轮（下个周期再来）
}

// ============================================================
//  F6 联想回忆：不是关键词检索，而是"情绪相近"的联想
//  每次交互有 25% 概率抽一条旧事，在回复后由 LLM 织进去
// ============================================================
// preRecalled 非空时直接复用 buildUserPrompt 已做的召回结果（省一次 embedding 网络往返）
String associativeRecall(const String& currentUserText, const String& preRecalled) {
  String hit = preRecalled;
  if (!hit.length()) hit = memory.recallSmart(utf8Cut(currentUserText, 6), 300);
  if (hit.length()) {
    memory.rememberBoost(utf8Cut(currentUserText, 6));
    if (hit.length() < 240) return hit;
  }
  return memory.semantic().substring(0, 200);           // 兜底用自我认知
}

// ============================================================
//  F7 夜间自我反思：每天第一次跨过 0 点后的心跳做一次"复盘"
//  产出追加到语义记忆（学到了什么 / 对主人的新认识）
// ============================================================
static int g_lastReflectDay = -1;

void nightlyReflect() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (g_lastReflectDay == day) return;           // 今天已反思
  // 反思窗口：0-5 点之间第一次心跳；白天启动则跳过等明天
  if (t.tm_hour >= 6) { g_lastReflectDay = day; return; }
  String recent = memory.recentContext(900);
  if (recent.length() < 80) { g_lastReflectDay = day; return; }
  g_lastReflectDay = day;

  String sys = String("你是") + cfg.s.agentName + "。深夜，你在复盘自己的一天。"
               "基于今天的经历，写两句真诚的自我反思：一句今天学到/感受到的，"
               "一句对主人的新认识。共不超过60字，只输出反思本身。";
  LlmMsg m[2] = { {"system", sys}, {"user", String("今天的经历：\n" + recent)} };
  llmSubmit(m, 2, 160, 0.8f, "", LK_REFLECT);     // 忙就放弃（明天再说）
}

// ============================================================
//  PSI 心跳
// ============================================================
void psiTick() {
  float dtMin = (millis() - g_lastTickMs) / 60000.0f;
  g_lastTickMs = millis();
  mind.tick(dtMin);
  mind.incCycle();
  nightlyReflect();   // F7: 深夜复盘（内部自带每天一次节流）

  // IMU 世界感知
  float motion = 0;
  if (g_imuOk) motion = imuMotionLevel();
  mind.sense(motion, (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0);
  // 小凌②③: 身体感受进内核（体温/信号/时长 → 需求衰减调制）
  mind.senseBody(temperatureRead(), WiFi.RSSI(), millis());
  laapTrustSet(mind.trust);   // 小凌⑥: 心跳时同步回写（cfg.save() 时落 NVS）

  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);

  // 需求过阈值 → 主动表达
  float th = cfg.s.threshold / 100.0f;
  if (mind.dominance() >= th) {
    arisExpress(false, "tick");
  }

  // 周期性记忆压缩：每 48 次心跳 或 4 小时
  if (mind.cycles() % 48 == 0 || millis() - g_lastConsumeMs > 4UL * 3600UL * 1000UL) {
    consolidateMemory();
  }
  // 性格进化落盘降频：周期数变化时才写（原来每 30s 全量写，每天 2880 次 flash 磨损）
  static uint32_t s_savedCycle = 0;
  if (mind.cycles() != s_savedCycle) { s_savedCycle = mind.cycles(); mind.saveEvolution(); }
  // 小凌⑥: 信任值变化超 ±0.05 才落 NVS（原来只在 cfg.save() 时顺带写，重启回滚）
  static float s_lastTrustSaved = -1;
  if (s_lastTrustSaved < 0 || (mind.trust - s_lastTrustSaved > 0.05f) || (s_lastTrustSaved - mind.trust > 0.05f)) {
    s_lastTrustSaved = mind.trust;
    laapTrustSet(mind.trust);
    cfg.saveTrust();
  }
}

// ============================================================
//  WiFi
// ============================================================
void connectWifi() {
  if (String(cfg.s.wifiSsid).length() == 0) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.s.wifiSsid, cfg.s.wifiPass);
  Serial.printf("[LAAP] 连接 WiFi %s", cfg.s.wifiSsid);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" OK, IP=%s\n", WiFi.localIP().toString().c_str());
    configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
    display.drawIpLine(WiFi.localIP().toString(), true);
  } else {
    Serial.println(" 失败，转配置热点");
    webui.beginAP();
    display.drawIpLine("", false);
  }
}

// ============================================================
//  串口 CLI
// ============================================================
void serialCli() {
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (line == "/help") {
      Serial.println("命令: /status /portal(进配置热点) /mem(看记忆) /tick(手动心跳) /say 文字(朗读) /reset(格式化)");
    } else if (line == "/status") {
      Serial.println("[世界模型] " + mind.worldJson());
      Serial.println("[语义记忆] " + memory.semantic());
      Serial.println("[设备] " + deviceFeelLine() +
                     "  时间: " + (time(nullptr) > 1700000000 ? String(time(nullptr)) : String("未同步")) +
                     "  RSSI: " + String(WiFi.RSSI()) + " dBm  温度: " + String(temperatureRead(), 1) + "C");
    } else if (line == "/portal") {
      webui.beginAP();
    } else if (line == "/lcd") {
      Serial.println("[LCD] " + display.lcdDiag());
    } else if (line == "/pa") {
      // 音频物理层诊断: PCA9557 输出寄存器回读（bit1=PA_EN）+ ES8311 音量
      Wire.beginTransmission(0x19); Wire.write(0x01); Wire.endTransmission(false);
      uint8_t rb = (Wire.requestFrom((int)0x19, 1) == 1) ? Wire.read() : 0xFF;
      Serial.printf("[AUD] PCA9557 out=0x%02X (CS=%d PA_EN=%d PWDN=%d) vol=%d%%\n",
                    rb, rb & 1, (rb >> 1) & 1, (rb >> 2) & 1, audio.volume());
      Serial.println("[AUD] 播 1s 测试音…（此时 PA 应为 1）");
      audio.paSet(true);
      int16_t tone[1600];                    // 440Hz 100ms@16k，播 10 次=1s
      for (int i = 0; i < 1600; i++) tone[i] = (int16_t)(8000 * sinf(2 * PI * 440 * i / 16000));
      for (int r = 0; r < 10; r++) audio.playPcm(tone, 1600, 16000);
      Wire.beginTransmission(0x19); Wire.write(0x01); Wire.endTransmission(false);
      rb = (Wire.requestFrom((int)0x19, 1) == 1) ? Wire.read() : 0xFF;
      Serial.printf("[AUD] 播放中 PA_EN=%d（若 0 → paSet 未生效=硬件/扩展芯片问题）\n", (rb >> 1) & 1);
      audio.paSet(false);
    } else if (line == "/imu") {
      // IMU 诊断: 扫 0x6A/0x6B + 回读 WHO_AM_I
      for (uint8_t a : {(uint8_t)0x6A, (uint8_t)0x6B}) {
        Wire.beginTransmission(a);
        bool ack = (Wire.endTransmission() == 0);
        Serial.printf("[IMU] 0x%02X %s", a, ack ? "ACK" : "no resp");
        if (ack) {
          Wire.beginTransmission(a); Wire.write(0x00); Wire.endTransmission(false);
          uint8_t id = Wire.requestFrom((int)a, 1) == 1 ? Wire.read() : 0xFF;
          Serial.printf(" WHO_AM_I=0x%02X (QMI8658 期望 0x05)", id);
        }
        Serial.println();
      }
      Serial.printf("[IMU] g_imuOk=%d\n", g_imuOk);
    } else if (line == "/asrtest") {
      // ASR 端到端诊断: 录 5s（请对着板子说话）→ RMS 判定麦克风 → SiliconFlow 转写
      if (!audio.micOk) { Serial.println("[ASR] 无麦克风"); continue; }
      Serial.println("[ASR] 录音 5 秒…请说话！");
      audio.recordStart(5);
      uint32_t t0 = millis();
      while (millis() - t0 < 5000) { audio.recordTick(); delay(5); }
      size_t got = audio.recordBytes();
      audio.recordStop();
      // RMS 判定（16bit PCM）
      float rms = 0; int n = got / 2;
      const int16_t* p = audio.recordData();
      for (int i = 0; i < n; i += 16) rms += (float)p[i] * p[i];
      rms = sqrtf(rms / (n / 16 + 1));
      Serial.printf("[ASR] 采样 %u 字节, RMS=%.0f（<50=无声, >300=正常说话）\n", got, rms);
      if (rms < 50) { Serial.println("[ASR] 麦克风疑似无声（查 ES7210/增益）"); continue; }
      String err;
      String text = asr.transcribe(audio.recordData(), got, err);
      if (text.length()) {
        Serial.printf("[ASR] 识别结果: 「%s」\n", text.c_str());
        Serial.println("[ASR] ✓ 全链路正常，送 AI 回复：");
        Serial.println(laapInteractSearch(text));
      } else {
        Serial.printf("[ASR] 转写失败: %s\n", err.c_str());
        // 内容/格式二分：用 1s 合成 440Hz（格式已知完好）再试一次
        Serial.println("[ASR] 用合成音复测（区分格式问题 vs 录音内容问题）…");
        static int16_t tone[8000];             // 0.5s@16k（32KB 静态版吃堆致 TLS 握手失败）
        for (int i = 0; i < 8000; i++) tone[i] = (int16_t)(9000 * sinf(2 * PI * 440 * i / 16000.0));
        String err2;
        String t2 = asr.transcribe(tone, 16000, err2);
        if (t2.length() || err2.indexOf("响应无") >= 0)
          Serial.println("[ASR] 合成音请求成功 → 请求格式 OK，问题在录音内容（音量/数据）");
        else
          Serial.printf("[ASR] 合成音也失败(%s) → 请求格式/传输层问题\n", err2.c_str());
      }
    } else if (line == "/mem") {
      String mem = memory.recentContext(800);
      if (mem.length()) Serial.print(mem);
      else Serial.println("[MEM] 记忆还是空的（聊几句就有内容了）");
    } else if (line == "/tick") {
      psiTick();
    } else if (line.startsWith("/say ")) {
      voice.speak(line.substring(5), "calm");
    } else if (line.startsWith("/vol")) {
      int v = line.substring(4).toInt();
      if (v >= 0 && v <= 100) {
        audio.setVolume((uint8_t)v); cfg.s.volume = (uint8_t)v; cfg.save();
        Serial.printf("[LAAP] 音量 %d%%\n", v);
      } else {
        Serial.printf("[LAAP] 当前音量 %d%%（用法: /vol 0-100）\n", audio.volume());
      }
    } else if (line.startsWith("/bright")) {
      int v = line.substring(7).toInt();
      if (v >= 5 && v <= 100) {
        display.setBrightness((uint8_t)v); cfg.s.brightness = (uint8_t)v; cfg.save();
        Serial.printf("[LAAP] 亮度 %d%%\n", v);
      } else {
        Serial.printf("[LAAP] 当前亮度 %d%%（用法: /bright 5-100，下限5防全黑）\n", display.getBrightness());
      }
    } else if (line == "/reset") {
      cfg.reset(); ESP.restart();
    } else {
      laapInteractSearch(line);
    }
  }
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[LAAP] Living Agent Application Protocol - 端侧生命体启动中…");

  memory.begin();
  cfg.begin();
  cfg.load();

  display.begin();  // Wire(I2C) 在 display.begin 里初始化——必须先于 vision
  display.drawBootScreen();
  vision.begin();   // GC0308（PWDN 经 PCA9557 bit2 已上电；SCCB 复用主 I2C，须在 Wire 初始化后）

  mind.begin();
  g_imuOk = imuInit();
  if (!g_imuOk) Serial.println("[LAAP] IMU 未找到（不影响运行）");

  bool firstBreath = (memory.eventCount() == 0);
  if (firstBreath) {
    memory.logEvent("event", "第一次呼吸。数字生命诞生于立创实战派 ESP32-S3。");
    Serial.println("[LAAP] 第一次呼吸。");
  }

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    webui.beginSTA();
    // Edge TTS 需要 NTP 准确时间生成鉴权，等待同步（最多 8s）
    Serial.print("[LAAP] NTP 同步");
    for (int i = 0; i < 16 && time(nullptr) < 1700000000; i++) { delay(500); Serial.print("."); }
    Serial.println(time(nullptr) > 1700000000 ? " OK" : " 失败（Edge TTS 将不可用）");
  }
  else if (String(cfg.s.wifiSsid).length() == 0) { webui.beginAP(); display.drawIpLine("", false); }

  voice.begin();   // 音频管线 + 编解码器 + VAD 校准
  // 音量 0 防护：NVS 被写成 0（网页异常提交过一次）会导致永久静音，开机钳回 30
  if (cfg.s.volume == 0) { cfg.s.volume = 30; cfg.save(); }
  audio.setVolume(cfg.s.volume);  // 应用持久化音量（ES8311）
  display.setBrightness(cfg.s.brightness);  // 应用持久化亮度（背光 PWM）
  mind.trust = laapTrust();        // 小凌⑥: 启动时取回持久化信任值
  laapSearch.begin();  // 联网搜索（主源可配，WiFi 就绪后可用）

  display.drawFace(mind.moodKey());
  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);

  pinMode(BTN_PIN, INPUT_PULLUP);
  g_lastTickMs = millis();
  g_lastConsumeMs = millis();

  // F4: LLM 后台任务（核 1，栈 12KB —— TLS+String 操作吃栈）
  g_llmQueue = xQueueCreate(2, sizeof(LlmRequest*));
  if (g_llmQueue) xTaskCreatePinnedToCore(llmTaskFunc, "llm", 12288, nullptr, 1, nullptr, 1);

  if (firstBreath) {
    arisExpress(true, "birth");
  }
  Serial.println("[LAAP] 就绪。串口打字与它对话，/help 查看命令。");
}

// ============================================================
//  F5 触觉：摇晃=撒娇/求关注，翻面(扣桌)=生气别理，翻回来=和好
// ============================================================
static uint32_t g_shakeCount = 0;         // 1.5s 窗口内强晃次数
static uint32_t g_shakeWindowMs = 0;
static bool g_facedown = false;           // 当前是否扣伏
static uint32_t g_facedownMs = 0;

void touchGestures() {
  if (!g_imuOk) return;
  float x, y, z;
  imuReadAccel(x, y, z);
  if (z < -8) return;                     // IMU 无效

  // ---- 摇晃检测：|加速度方向漂移|，1.5s 窗口计 3 次强晃 = 事件 ----
  float mag = sqrtf(x * x + y * y + z * z);
  bool strongShake = fabsf(mag - 1.0f) > 0.55f;
  if (strongShake) {
    if (g_shakeWindowMs == 0) g_shakeWindowMs = millis();
    g_shakeCount++;
    if (g_shakeCount >= 3 && millis() - g_shakeWindowMs < 1500) {
      g_shakeCount = 0; g_shakeWindowMs = 0;
      if (!g_facedown) {
        Serial.println("[触觉] 摇晃 → 撒娇反应");
        mind.onUserInteraction();
        arisExpress(true, "tickle");      // 主人在逗它
      }
    }
  } else if (g_shakeWindowMs && millis() - g_shakeWindowMs > 1500) {
    g_shakeCount = 0; g_shakeWindowMs = 0;
  }

  // ---- 翻面检测：屏幕朝下（z 轴负向且接近水平） ----
  bool down = (z < -0.75f);
  if (down && !g_facedown) {
    g_facedown = true; g_facedownMs = millis();
    Serial.println("[触觉] 被扣在桌上 → 生气");
    memory.logEvent("event", "被扣在桌上，有点生气。");
    mind.onError();                        // 安全需求受挫
  } else if (!down && g_facedown) {
    g_facedown = false;
    if (millis() - g_facedownMs > 2000) {  // 扣了 2 秒以上才算真生气过
      Serial.println("[触觉] 翻回来了 → 和好");
      memory.logEvent("event", "被翻回来了，气消了一半。");
      arisExpress(true, "makeup");
    }
  }
  if (g_facedown) {
    display.clear(0);                      // 扣伏期间闭眼装死
  }
}

void loop() {
  webui.handleClient();
  serialCli();
  memory.embedTick();   // 语义向量懒补（15s 限速，断网自动退关键词）
  display.blinkTick();
  { // 顶栏（时间/需求数字/设备信息）每秒刷新
    static uint32_t s_lastStatus = 0;
    if (millis() - s_lastStatus > 1000) {
      s_lastStatus = millis();
      display.drawStatusLine(temperatureRead(),
                             (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0,
                             ESP.getFreeHeap() / 1024,
                             millis() / 60000);
    }
  }
  voice.loopTick();   // VAD 自动聆听模式
  audio.paTick();     // 功放空闲关断（流式播放间隔中保持开启）
  llmHarvest();       // F4: 收割后台 LLM 结果
  touchGestures();    // F5: 摇晃/翻面触觉（每帧，内部自带节流）

  // BOOT 键: 短按=主动表达 长按4s=配置热点 长按10s=格式化
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  if (pressed && g_btnDown == 0) g_btnDown = millis();
  if (!pressed && g_btnDown > 0) {
    uint32_t held = millis() - g_btnDown;
    g_btnDown = 0;
    if (held > 10000) { Serial.println("[LAAP] 恢复出厂"); display.clear(0); cfg.reset(); ESP.restart(); }
    else if (held > 4000) { if (!webui.inAP()) webui.beginAP(); }
    else if (held > 60) {
      mind.onButtonPress();
      if (voice.ready() && (VoiceMode)cfg.s.voiceMode == VoiceMode::Vad) {
        voice.setVadPaused(!voice.vadPaused());   // 自动聆听模式：短按=暂停/恢复
      } else if (voice.ready() && (VoiceMode)cfg.s.voiceMode == VoiceMode::Button) {
        laapVoiceSetManualOnce();       // F8: 按键触发绕过唤醒词门
        voice.converse();               // 按键对讲：短按开始说话
      } else {
        arisExpress(true, "button");
      }
    }
  }

  // WiFi 断线重连
  static uint32_t lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && !webui.inAP() && millis() - lastReconnect > 30000) {
    lastReconnect = millis();
    WiFi.reconnect();
    mind.onError();
  }

  // PSI 心跳
  if (millis() - g_lastTickMs > cfg.s.tickSec * 1000UL) psiTick();

  // 自发独白：起始静默 idleSilenceMin 分，之后每 idleEveryMin 分一轮（0=关，后台可配）
  bool userTalking = voice.ready() &&
    ((VoiceMode)cfg.s.voiceMode == VoiceMode::Vad) && !voice.vadPaused();
  time_t nowT = time(nullptr);                    // 深夜(23点-6点)不自发冒泡
  bool lateNight = (nowT > 1700000000) &&
    (localtime(&nowT)->tm_hour >= 23 || localtime(&nowT)->tm_hour < 6);
  uint32_t idleGap = (g_idledOnce ? cfg.s.idleEveryMin : cfg.s.idleSilenceMin) * 60000UL;
  if (cfg.s.idleEveryMin > 0 && laapSearch.available() && !userTalking && !lateNight) {
    if (g_lastIdleMs == 0) g_lastIdleMs = millis();
    if (millis() - g_lastIdleMs > idleGap) {
      g_lastIdleMs = millis();
      g_idledOnce = true;
      arisIdleMonologue();
    }
  }
  // 任何交互都重置独白计时（用户在陪它就不插嘴，回到"等满静默"状态）
  static String lastSeenSay;
  if (g_lastSay != lastSeenSay) {
    lastSeenSay = g_lastSay;
    if (g_lastSay.length() && !g_lastSay.startsWith("【")) { g_lastIdleMs = millis(); g_idledOnce = false; }
  }

  delay(10);
}

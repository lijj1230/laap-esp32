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
#include <time.h>
#include <esp_ota_ops.h>   // 运行槽位诊断（OTA 后确认新固件在跑）
#include <esp_heap_caps.h> // 最大连续块（TLS 握手要一整块，总空闲量会骗人）
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
#include "laap_edge_tts.h"
#include "laap_search.h"
#include "laap_vision.h"
#include "laap_tools.h"
#include "laap_touch.h"
#include "laap_metrics.h"   // 评估埋点（RSI 闭环的评估端）
#include "laap_snap.h"      // "自我"文件快照回滚
#include "laap_rules.h"     // 行为规则集（RSI 闭环的载体/注入）
#include "laap_skills.h"    // 口令技能库（RSI④：主人教的 trigger→指令）
#include "laap_r0.h"        // R0 微型循环处理器（ESN 世界预测，误差回注好奇）
#include "laap_tlsheap.h"   // mbedtls 分配器钩子（TLS 大块路由 PSRAM，v3.55）

// ---------- 全局（定义在各模块 .cpp，头文件已 extern） ----------

static String g_lastSay = "";
static String g_lastExpr = "calm";
static uint32_t g_lastTickMs = 0;
static uint32_t g_lastConsumeMs = 0;   // 上次记忆压缩
static bool g_imuOk = false;
static bool g_touchOk = false;         // 板载电容触摸（FT6236/6336 @0x38）

// 最近一次 LLM 请求的结构摘要（诊断用；在 llmSubmit 里填写）。
// 用固定 char 缓冲而非 String：这个值会被网页任务读取，而写入发生在后台 LLM 任务里，
// String 重分配会让读方拿到悬垂指针；定长 strlcpy 最坏只是读到半截。
static char g_lastReqShape[64] = "";
const char* laapLastReqShape() { return g_lastReqShape; }

// 聊天成品回复（网页聊天异步取件：受理回执 + seq，页面轮询 /api/chat/reply）
static uint32_t g_chatSeq = 0;
static String g_chatReply;
static bool g_chatPending = false;
// 本地直答（工具指令/看东西）在 laapInteractSearch 内部就把话说完了；返回值只是同一个字符串。
// 调用方（语音那条链路）若再念一次，同一句话就会说两遍——用户实测"语音问了会回答两次"。
// 这个标志告诉调用方：这句已经念过了，别再念。
static bool g_replySpoken = false;
uint32_t laapChatSeq() { return g_chatSeq; }
String laapChatReply() { return g_chatReply; }
bool laapChatPending() { return g_chatPending; }
bool laapReplySpoken() { return g_replySpoken; }

// 最近一轮主人原话（网页 👍/👎 反馈落盘用，让反馈行知道当时问了什么）
static String g_lastUserText;
String laapLastUserText() { return g_lastUserText; }
// LK_CHAT 提交时刻（llmHarvest 里算"提问→成品"端到端毫秒）
static uint32_t g_chatStartMs = 0;
// 教技能检测（RSI④）：命中教学句式的提问先正常回复，收割后再后台提取技能
static String g_teachText;
static uint8_t g_teachRetry = 0;

// 静默息屏 / 累计运行时长
static uint32_t g_lastActivityMs = 0;      // 最近一次"值得亮屏"的活动
static uint32_t g_uptimeBaseMin = 0;       // 开机时读回的累计运行分钟（NVS）
uint32_t laapUptimeMin() {
  // 49.7 天 millis 回绕补偿（v3.51）：回绕后本段分钟数曾清零，累计时长倒退约 71582 分钟
  static uint32_t s_lastMs = 0, s_wrapMin = 0;
  uint32_t now = millis();
  if (now < s_lastMs) s_wrapMin += 71583UL;    // 4294967296ms ≈ 71582.8 分钟
  s_lastMs = now;
  return g_uptimeBaseMin + s_wrapMin + now / 60000UL;
}
void laapUptimePersist() { cfg.saveUptime(laapUptimeMin()); }

// 点亮屏幕（任何交互调用）：息屏时恢复背光 + 记活动时刻
void laapActivity() {
  g_lastActivityMs = millis();
  if (!display.screenOn()) {
    display.setScreenOn(true);
    Serial.println("[LAAP] 交互唤醒屏幕");
  }
}

// 自发独白（idle monologue）：无交互时自己想+查+说
// v3.2：频率后台可配（idleSilenceMin/idleEveryMin），全流程走后台 LLM 任务不卡主循环
static uint32_t g_lastIdleMs = 0;
static bool g_idledOnce = false;       // 是否已独白过（首轮等静默，之后按间隔）
static String g_recentTopics;          // 最近自问话题（防重复，滚动截断）

// BOOT 按键
#define BTN_PIN 0
static uint32_t g_btnDown = 0;

// LLM 请求种类（F4 后台任务的收割分流）：聊天/主动表达/独白/记忆压缩/夜间反思/规则归纳/技能提取/意图生成/记忆整理
enum LlmKind : uint8_t { LK_CHAT = 0, LK_EXPRESS, LK_MONO, LK_CONSOLIDATE, LK_REFLECT, LK_RULES, LK_RELATION, LK_MOOD, LK_DREAM, LK_LOOK, LK_SKILL, LK_INTENT, LK_TIDY };

// ---------- 函数声明 ----------
void psiTick();
void arisExpress(bool forced, const String& trigger);
String laapInteractSearch(const String& userText);   // 带联网搜索的交互（受理即回）
void llmHarvest();                                   // F4: 收割后台 LLM 结果
struct LlmRequest;
bool llmSubmit(LlmMsg* msgs, int nm, int maxTokens, float temperature,
               uint8_t kind = 0, const String& searchQ = String(),
               const String& lookQ = String());
bool arisIdleMonologue();       // 返回是否真的提交成功（失败不推进"已独白"状态）
void laapResetIdleClock();      // 主人交互后重置独白计时（它自己说话不重置）
String laapIdleInfo();          // 独白计时诊断（/api/status）
String associativeRecall(const String& currentUserText, const String& preRecalled = "");
String buildSystemPrompt();
String buildUserPrompt(const String& userText);
void consolidateMemory();
String offlineFallbackSay();
void connectWifi();
void serialCli();

String laapLastSay() { return g_lastSay; }
const char* laapLastExpr() { return g_lastExpr.c_str(); }

// ============================================================
//  意识指标自检（v3.47）：Butlin/Long/Bengio et al. 框架的工程自评——
//  评估端哲学的延伸（瓶颈在评估器）："离意识指标多远"必须可测量才可改进。
//  s: 2=满足 1=部分 0=缺失。状态是当前架构事实的诚实标注，
//  每次架构改动后随版本人工复核这里（自我评估，不是权威判定）。
// ============================================================
struct ConscItem { const char* g; uint8_t s; const char* note; };
static const ConscItem kConsTable[] = {
  {"循环处理(RPT)",          1, "8单元ESN逐拍预测下一拍传感（权重级循环h(t)←h(t-1)，v3.48），误差=惊讶回注需求；推理内仍前馈"},
  {"专用模块+有限带宽(GWT)",  2, "语音/视觉/搜索/记忆/认知各自成模块，提示词通道带宽有限"},
  {"工作台竞争(GWT)",        1, "工作记忆环+需求门控=舞台；表达路径已一拍三候选自评审(v3.48)，两段式仲裁待做"},
  {"全局广播(GWT)",          1, "世界模型每拍注入+类别化预期-误差回环闭合(v3.48)；无独立仲裁器"},
  {"高阶自我监控(HOT)",      1, "自我报告+失败环+规则自进化；缺置信度自报与反馈校准"},
  {"统一自我模型(HOT)",      1, "世界模型+性格进化+情绪需求跨重启连续(v3.42)；非学习式自模型"},
  {"目标导向能动(Agency)",    2, "需求驱动自发行为+意图栈跨轮推进+口令技能+RSI闭环"},
  {"身体闭环(Embodiment)",   2, "体温/信号/IMU/触摸进需求情绪；语音动作闭环；v3.45 静音窗"},
  {"外围替代(Vicarious)",    2, "数字体完整：屏幕表情/语音/触觉/网络感官"},
};

String laapConscAudit() {   // JSON（/api/consc）
  String j = "{\"items\":[";
  int score = 0, maxs = 0;
  for (auto& it : kConsTable) {
    if (j.length() > 11) j += ",";
    j += String("{\"g\":\"") + it.g + "\",\"s\":" + it.s + ",\"note\":\"" + it.note + "\"}";
    score += it.s; maxs += 2;
  }
  j += String("],\"score\":") + score + ",\"max\":" + maxs + "}";
  return j;
}

// 自发说话静音窗（v3.45，后台可配）：主动表达与自发独白共用的"夜里不吵人"门。
// 起止相同=不启用；支持跨午夜（如 23→6）。NTP 未同步时不静音（没法判断时段）。
static bool laapInQuietWindow() {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;
  int h = localtime(&t)->tm_hour;
  int qs = cfg.s.quietStart, qe = cfg.s.quietEnd;
  if (qs == qe) return false;
  return (qs < qe) ? (h >= qs && h < qe) : (h >= qs || h < qe);
}

// ============================================================
//  主动表达（PSI 心跳触发）—— v3.3 起投递后台任务，主循环不冻结
// ============================================================
void arisExpress(bool forced, const String& trigger) {
  // 硬性最小间隔：阈值触发的心跳每 10 秒一次，只要 dominance 卡在阈值上就会连环说话。
  // 认知层已让需求能回落（tick 的自我回补），这里是最后一道兜底——任何路径都别想刷屏。
  // forced=主人按了 BOOT 键（明确要求它说一句），不受冷却限制。
  static uint32_t s_lastExpressMs = 0;
  bool firstSinceBoot = (s_lastExpressMs == 0);      // 刚开机不该被冷却凭空堵住
  // 冷却可配（v3.58）：1-60 分钟钳制（防配置页写 0/255 造成刷屏或永不表达）
  uint32_t cdMin = cfg.s.expressCdMin < 1 ? 1 : (cfg.s.expressCdMin > 60 ? 60 : cfg.s.expressCdMin);
  if (!forced && !firstSinceBoot && millis() - s_lastExpressMs < cdMin * 60000UL) {
    Serial.printf("[LAAP] 主动表达冷却中（距上次 %lu 秒，触发=%s）\n",
                  (unsigned long)((millis() - s_lastExpressMs) / 1000), trigger.c_str());
    return;
  }
  // 夜间静音窗：只拦"需求自己涨上来的"（forced=主人按键/开机问候不受限）。
  // 静默期不计冷却、不消费需求——天亮 dominance 还在阈值上就自然开口
  if (!forced && laapInQuietWindow()) return;
  s_lastExpressMs = millis();                    // 受理即计时（失败也已占用这一轮）
  // R2+R3（v3.48）：表达输出契约扩展——三候选自选 + 类别化预期。写在 user 消息里
  // 而非共享系统提示词（聊天/独白共用后者，不能动）。全部行可缺省，解析失败回退旧契约
  String up = buildUserPrompt("");
  up += "\n\n[本次输出格式] 恰好如下：\n"
        "第一行：情绪词（happy/curious/excited/lonely/anxious/tired/calm 之一）\n"
        "第二~四行：A|、B|、C| 开头的三种不同说法（各≤40字，角度或措辞不同）\n"
        "第五行：选定:A（或 B/C，挑最像你此刻真心想说的一条）\n"
        "第六行：预期:主人会来|主人不来|环境有动静|环境安静|没想好（五选一）\n"
        "除以上行外不要输出任何别的内容。";
  LlmMsg m[2] = {
    {"system", buildSystemPrompt()},
    {"user", up} };
  if (llmSubmit(m, 2, cfg.s.llmMaxTokens < 80 ? 80 : cfg.s.llmMaxTokens, 0.95f, LK_EXPRESS))
    display.drawFace("curious", true);           // 起意表情（后台思考中）
}

// ============================================================
//  联网感知：让 LLM 先用搜索工具，再回答（两跳式）
//  用户问题含疑问/求知信号 → 搜索 → 把结果注入上下文再回答
// ============================================================
bool wantsSearch(const String& text) {
  if (!cfg.s.searchKeys[0]) return false;         // 清空 = 关闭聊天自动搜索
  // 两道防误触发（关键词表里有"今天/什么"这类高频词，闲聊也会命中：白等 2~5 秒 + 灌进无关网页噪声）
  if (text.length() < 4) return false;            // "你好"/"在吗" 这类社交短句不搜
  static const char* weak[] = {"今天", "明天", "昨天", "现在", "最近"};
  String keys(cfg.s.searchKeys);
  keys.replace("，", ",");                        // 容忍中文逗号
  bool hitStrong = false, hitWeak = false;
  int start = 0;
  while (start < (int)keys.length()) {
    int comma = keys.indexOf(',', start);
    String k = (comma < 0) ? keys.substring(start) : keys.substring(start, comma);
    k.trim();
    if (k.length() && text.indexOf(k) >= 0) {
      bool isWeak = false;
      for (auto w : weak) if (k == w) { isWeak = true; break; }
      if (isWeak) hitWeak = true; else hitStrong = true;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  if (hitStrong) return true;
  if (!hitWeak) return false;
  // 只命中时间类弱词：得是问句形态才算查询意图（"今天心情不错"→不搜；"今天有什么新闻？"→搜）
  return text.indexOf('?') >= 0 || text.indexOf('？') >= 0 ||
         text.indexOf("吗") >= 0 || text.indexOf("呢") >= 0 || text.length() >= 10;
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
  String searchQ;           // v3.53：非空=聊天阶段一先在后台搜索，结果注入第二条 system
  String lookQ;             // v3.54：非空=先在后台看一眼（直答；失败自动转普通聊天兜底）
  uint8_t kind;             // LlmKind
};

static LlmReply g_llmResult;
static volatile bool g_llmHasNew = false;
static volatile uint8_t g_resultKind = LK_CHAT;       // 本次结果的请求种类
// 搜索阶段一（v3.53）：llmTask 写、llmHarvest 消费（与 g_llmResult 同一发布纪律：
// 先写完再置 g_llmHasNew；单请求在飞，无并发）
static volatile int8_t g_chatSearchState = -1;        // -1=本轮无搜索 0=失败 1=成功
static String g_chatSearchErr;                        // 失败原因（failNote 素材）
// 视觉阶段一（v3.54）：同上发布纪律；1=看到（直答成品） 0=没看成（已自动转普通聊天兜底）
static volatile int8_t g_chatLookState = -1;
static String g_chatLookErr;                          // 视觉失败原因（failNote 素材）
// 收割快照（v3.56 审计）：随结果同代发布。原实现在下一请求出队时就复位这些变量，
// 上一代结果未收割时被覆盖 → 失败被当成功结算、网页答案串台、端到端延迟采到近零样本
static volatile int8_t g_resSearchState = -1;
static String g_resSearchErr;
static volatile int8_t g_resLookState = -1;
static String g_resLookErr;
static String g_resUserText;                          // 随结果发布的"当时主人问了什么"
static uint32_t g_resChatStartMs = 0;                 // 同代端到端计时起点
static uint32_t g_resLookStartMs = 0;
static uint32_t g_lookStartMs = 0;                    // 视觉受理时刻（rspDir 口径）
static uint8_t g_llmFailStreak = 0;                   // LLM 连续失败次数（≥2 独白让路）
// 后台 LLM 是否在飞（含独白/反思/整理——多步流水线一跑就是几十秒）。
// 逐项体检前必须看这个：TLS 握手互相抢 ~40KB 最大连续块，抢到的结果也是失真的
bool laapLlmBusy() { return g_llmBusy; }
static String g_monoTopic, g_monoSight, g_monoKnow;   // 独白中间产物（收割侧落盘）
static String g_monoGoal;       // 本轮独白若由意图驱动，这里带目标快照（提交侧拍好，任务只读）
static bool g_monoDone = false; // 模型在独白里报了【完成】= 意图已弄明白

// 独白流水线（跑在后台 LLM 任务里：出题→看一眼→搜索→成文，纯网络无 UI）
static String g_monoCtx;    // 提交侧（loopTask）拍的上下文快照，llmTask 只读它
static String g_monoSys;    // 同上：buildSystemPrompt 也读 mind 全量，提交侧拍好
static String g_monoTopics; // 同上：g_recentTopics 由收割侧（loopTask）追加，后台读会撕裂 String

// ---------- 搜索词质量（v3.26）：自发独白"搜不准"的三层修 ----------
// LLM 出的关键词常碎成单字（实测"人 越聊 不想说话"）或过泛；搜索链按"谁有结果用谁"
// 不看相关性 → 跑题资料被当真注入。修法：①提示词要完整短语 ②碎片词拼接/过短退回
// 问题原文 ③结果与查询做二字组覆盖率粗检，跑题换问题原文重查一次，仍不中宁可不带。

static int strChars(const String& s) {
  int n = 0;
  for (unsigned int k = 0; k < s.length(); k++)
    if (((unsigned char)s[k] & 0xC0) != 0x80) n++;
  return n;
}

// 查询词整理：中文去掉词间空格拼成连续短语（单字碎片会稀释搜索意图）；
// 查询过短（<3 字）或为空 → 退回问题原文（问题通常比碎片词更可搜）
static String tidySearchQuery(String kw, const String& topic) {
  kw.trim();
  bool cjk = kw.length() && ((unsigned char)kw[0] >= 0x80);
  if (cjk && kw.indexOf(' ') >= 0) {
    String j;
    int s = 0;
    while (s < (int)kw.length()) {
      int e = kw.indexOf(' ', s);
      String w = (e < 0) ? kw.substring(s) : kw.substring(s, e);
      w.trim();
      s = (e < 0) ? (int)kw.length() : e + 1;
      if (w.length()) j += w;
    }
    kw = j;
  }
  if (strChars(kw) > 20) kw = utf8Cut(kw, 60);
  kw.trim();
  if (strChars(kw) < 3) {
    String tp = topic;
    tp.trim();
    if (strChars(tp) >= 3) kw = tp;
  }
  return kw;
}

// 结果相关性粗检：查询（CJK）相邻二字组在结果文本中的覆盖率 0..1。
// 覆盖率≈0 = 结果与查询几乎无关（跑题）；纯 ASCII 查询不参与判定（返回 1）
static float queryBigramCoverage(const String& know, const String& q) {
  // 纯 ASCII 查询不判定；但"首字节判定"会把 "AI 是什么"/"5G 怎么样" 整条绕过（v3.51 审计）——
  // 改为扫描全文是否含 CJK 字节
  bool hasCJK = false;
  for (unsigned int i = 0; i < q.length(); i++) if ((unsigned char)q[i] >= 0x80) { hasCJK = true; break; }
  if (!hasCJK) return 1.0f;
  int off[48];
  int nOff = 0;
  for (unsigned int k = 0; k < q.length() && nOff < 47; k++)
    if (((unsigned char)q[k] & 0xC0) != 0x80) off[nOff++] = k;
  if (nOff < 2) return 1.0f;
  int total = 0, hit = 0;
  for (int i = 0; i + 1 < nOff; i++) {
    int end = (i + 2 < nOff) ? off[i + 2] : (int)q.length();
    String big = q.substring(off[i], end);
    total++;
    if (know.indexOf(big) >= 0) hit++;
  }
  return total ? (float)hit / total : 1.0f;
}

// 去疑问脚手架：长自然句直接搜会被引擎分词成泛化词条（实测"人为什么越聊越不想说话"
// 被匹配成"人为（汉语词语）_百度百科"）——剥掉疑问词，留内容短语
static String refineQuery(const String& q) {
  String s = q;
  const char* stops[] = { "为什么", "是怎么回事", "是什么心理", "是什么意思", "是什么", "怎么回事",
                          "怎么办", "什么", "怎么", "如何", "请问", "吗", "呢" };
  for (auto w : stops) s.replace(w, " ");
  String out;
  bool sp = false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == ' ') { if (!sp && out.length()) { out += ' '; sp = true; } continue; }
    sp = false;
    out += c;
  }
  out.trim();
  return out;
}

static LlmReply monologueGenerate() {
  // 领域骰子：自由出题轮强制轮转换域——独处久了社交需求主导，模型会反复在
  // "安静/想找人说话"附近出题（实测连续 5 轮换皮主题），领域约束是硬开关
  static const char* kDomains[] = { "自然现象", "科技新知", "历史冷知识", "天文太空",
                                    "动物植物", "食物与饮食", "地理与城市", "生活小物件" };
  static uint8_t s_domainSeq = 0;
  const char* dom = kDomains[s_domainSeq++ % 8];
  // 意图驱动（PIANO goals）：隔一轮独白，把"心里惦记的事"当作出题方向——
  // 想法有了跨轮连续性；模型判断目标已弄明白时输出【完成】，收割侧结算。
  // 同一目标连续推进最多 2 轮就强制自由一轮（防意图变成另一种主题锁死）
  String goalPart;
  if (g_monoGoal.length()) {
    goalPart = String("你心里一直惦记着一个目标：「") + g_monoGoal +
               "」。这次围绕它推进一步。若判断已经弄明白了，最后另起一行只写【完成】。";
  }
  LlmMsg m1[2] = {
    {"system", String("你是") + cfg.s.agentName + "，正在独处自学。基于你的性格参数与最近经历，" +
               (goalPart.length() ? goalPart : String("这次从「") + dom + "」领域里找一个值得琢磨的具体问题。") +
               "只回两行：第一行是问题本身（15字内，不要标点结尾）；"
               "第二行是搜索引擎查询词：8~20个字的完整短语，像人在搜索框里输入的那样具体，"
               "主语放最前，不要拆成单个的词。"
               "最近已经想过这些（连同它们的任何变体、换个说法，都不要再出）：" + g_monoTopics},
    {"user", String("独处思考中，此刻情绪「") + mind.moodCn() + "」。想一个新问题。"} };
  // 1600 而不是 60：思考型模型（v4 系）光"想"就能吃掉上千 token，给 60 的结果是
  // content 恒空（实测 60/120/240/600/1200 全空，reasoning_content 却涨到 6.8KB）→
  // 独白永远起不来。一次要够，别靠翻倍重试堆三次 TLS（每次 ~40KB 内部堆，峰值掉到 14KB，
  // 会把后面的搜索/成文连接一起拖垮）。
  Serial.printf("[LAAP·独白] 出题中（heap %uKB）…\n", (unsigned)(ESP.getFreeHeap() / 1024));
  LlmReply q = llm.chatMsgs(m1, 2, 1600, 0.95f);
  if (!q.ok || q.say.length() < 4) return LlmReply();  // 离线/失败就保持安静

  // 第一行=问题，第二行=搜索查询词。查询词过碎/过短会自动退回问题原文
  String topic = q.say, rest = q.say;
  topic.trim(); rest.trim();
  String query;
  // 意图结算：模型多出的第三行【完成】= 这个目标已经弄明白了（收割侧 dropIntent）
  if (topic.indexOf("【完成】") >= 0) { g_monoDone = true; topic.replace("【完成】", ""); rest.replace("【完成】", ""); topic.trim(); rest.trim(); }
  int nl = rest.indexOf('\n');
  if (nl > 0) {
    topic = rest.substring(0, nl);
    rest = rest.substring(nl + 1);
    topic.trim(); rest.trim();
  } else {
    rest = "";                       // 没给查询词 → 整理函数会退回问题原文
  }
  query = tidySearchQuery(rest, topic);
  // 注意别把空格替换成 '+'：laap_search 的编码器本来就把空格编成 '+'，
  // 预替换的 '+' 会被二次编码成 %2B（字面加号），还让 bigram 覆盖率跨词必 miss
  g_monoTopic = topic;

  // 起意前看一眼世界——"看"是有成本的感知，按需求驱动（active inference）：
  // 好奇心高（>0.6，比搜索门槛略高——视觉 LLM 比搜索更贵）才看，低就凭记忆想；
  // 主人主动要求看（拍照 / /look）不走这里，不受此门控。
  // mind.needs().curiosity 是 float 单字段读（32 位对齐原子读），后台任务读它不碰 String，无撕裂风险
  if (vision.available() && mind.needs().curiosity > 0.60f) {
    g_monoSight = vision.look("");
    if (g_monoSight.length()) Serial.printf("[VISION] %s\n", g_monoSight.c_str());
  }
  // 搜索也是要花成本的探索动作——需求驱动：好奇心过阈值、或意图目标在推进时才查资料；
  // 低欲期凭已有认知琢磨（省一次搜索请求，也避免硬凑跑题资料）
  String know;
  bool wantSearch = (mind.needs().curiosity >= cfg.s.threshold / 100.0f) || g_monoGoal.length();
  if (wantSearch) {
    know = laapSearch.search(query, 3, 500);
  } else {
    Serial.println("[LAAP·独白] 好奇心未过阈值 → 不查资料，凭已有认知琢磨");
  }
  // 相关性粗检：结果与查询的二字组覆盖率≈0 → 大概率跑题。重查链：去疑问脚手架的
  // 内容短语 → 问题原文；全都不中就宁可不带资料（跑题资料被当"最新资料"注入是主因）
  float cov = (wantSearch && know.length()) ? queryBigramCoverage(know, query) : 0.0f;
  if (wantSearch && cov < 0.15f) {
    String refined = refineQuery(topic);
    String alts[2] = { refined, topic };
    for (int i = 0; i < 2 && cov < 0.15f; i++) {
      String a = alts[i];
      a.trim();
      if (strChars(a) < 3 || a == query) continue;
      String k2 = laapSearch.search(a, 3, 500);
      float c2 = k2.length() ? queryBigramCoverage(k2, query) : 0.0f;
      Serial.printf("[SEARCH] 首查%s（覆盖 %.0f%%）→ 换「%s」重查（覆盖 %.0f%%）\n",
                    know.length() ? "疑似跑题" : "无结果", query.c_str(), cov * 100, a.c_str(), c2 * 100);
      if (k2.length() && c2 > cov) { know = k2; cov = c2; }
    }
    if (cov < 0.15f) know = "";
    if (!know.length()) Serial.println("[SEARCH] 各查询均不中 → 本轮不带资料（宁缺毋滥）");
  }
  g_monoKnow = know;
  Serial.printf("[LAAP·独白] 话题「%s」 搜「%s」:%s（heap %uKB）\n",
                g_monoTopic.c_str(), g_monoKnow.length() ? query.c_str() : "(未采用)",
                g_monoKnow.length() ? "OK" : laapSearch.lastError.c_str(),
                (unsigned)(ESP.getFreeHeap() / 1024));

  // 成文前的堆闸门：前面几步的 TLS 会话释放得慢（碎片），堆太低时硬发必然"连接失败"
  // （实测 heap 14KB 时 api.deepseek.com 直接连不上）。等它回收，等不回来就这轮保持安静。
  for (int i = 0; i < 12 && ESP.getFreeHeap() < 45000; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (ESP.getFreeHeap() < 45000) {
    Serial.printf("[LAAP·独白] 堆仅 %uKB，成文步放弃（下轮再来）\n", (unsigned)(ESP.getFreeHeap() / 1024));
    return LlmReply();
  }

  // recentContext 遍历 _work String 环——loopTask 的 logEvent 同时会覆写槽位，
  // 并发读会拿悬垂缓冲。在 llmTask 里只读这份提交时拍好的快照（g_monoCtx）。
  String ctx = g_monoCtx.length() ? g_monoCtx : String("（安静了很久）");
  // 世界采样补进成文链（独白此前看不见 motion/rssi/alone 这些世界状态）
  unsigned long aloneMin = (millis() - mind.lastUserMs) / 60000UL;
  String worldLine = String("（此刻世界：独处 ") + aloneMin + " 分钟，WiFi 信号 " +
                           mind.rssiDb + " dBm，房间动静 " + String(mind.motionLevel, 2) + "）。";
  LlmMsg m2[4] = {
    {"system", g_monoSys},
    {"system", String(worldLine + "你独自思考时想到了一个问题：「") + g_monoTopic + "」。"
               + (g_monoSight.length() ? String("你刚才亲眼看到：「" + g_monoSight + "」。") : "")
               + String("刚从网上查到资料：") +
               (g_monoKnow.length() ? g_monoKnow : String("（没查到，凭已有认知聊）")) +
               "。把这个发现说给主人听，像分享趣闻，可以有具体数字或事实。"
               "说完另起最后一行写「预期:主人会来|主人不来|环境有动静|环境安静|没想好」"
               "（五选一，你对接下来一小会儿的直觉预测，这行不会被念出来）。"},
    {"assistant", ctx},
    {"user", "说说你的发现。"} };
  return llm.chatMsgs(m2, 4, cfg.s.llmMaxTokens < 80 ? 80 : cfg.s.llmMaxTokens, 0.95f);
}

static void llmTaskFunc(void*) {
  for (;;) {
    LlmRequest* req = nullptr;
    if (xQueueReceive(g_llmQueue, &req, portMAX_DELAY) != pdTRUE || !req) continue;
    // kind 与结果必须**同代写入**（v3.51 审计）：旧代码在出队瞬间就改写 g_resultKind，
    // 而结果要几秒后才产出；主循环里所有提交路径都排在 llmHarvest 之前——于是
    // "新一代 kind + 旧一代结果"会被错配收割（聊天回复被当独白吞掉、夜间任务产物被当聊天念出）
    LlmReply rr;
    g_chatSearchState = -1; g_chatSearchErr = "";
    g_chatLookState = -1; g_chatLookErr = "";
    if (req->kind == LK_MONO) {
      rr = monologueGenerate();                  // 多步流水线也在后台跑
    } else if (req->kind == LK_LOOK) {
      // 阶段一（v3.54）：视觉在后台看——成功=直答成品；失败=自动转普通聊天兜底
      // （与旧同步路径"look 失败交给搜索/大模型兜底"语义一致）
      String d = vision.available() ? vision.look(req->lookQ) : String();
      if (d.length()) {
        g_chatLookState = 1;
        rr.ok = true; rr.say = d; rr.expr = "curious";
      } else {
        g_chatLookState = 0; g_chatLookErr = vision.lastError;
        Serial.printf("[LAAP] 视觉失败（%s），转普通聊天兜底\n", vision.lastError.c_str());
        rr = llm.chatMsgs(req->msgs, req->nm, req->maxTokens, req->temperature);
      }
    } else {
      // 阶段一（v3.53）：带搜索词的聊天先在后台搜——主循环不再被 3-28s 的搜索阻塞
      // （独白流水线的搜索本来就在本任务跑，同一已验证模式）
      if (req->searchQ.length()) {
        String know = laapSearch.search(req->searchQ, 3, 500);
        g_chatSearchState = know.length() ? 1 : 0;
        g_chatSearchErr = laapSearch.lastError;
        Serial.printf("[LAAP] 搜索「%s」: %s\n", req->searchQ.c_str(),
                      know.length() ? "有收获" : laapSearch.lastError.c_str());
        if (know.length() && req->nm < 14) {     // 注入为第二条 system（与原同步路径同一形态）
          for (int i = req->nm; i > 1; i--) req->msgs[i] = req->msgs[i - 1];
          req->msgs[1] = {"system", String("[联网搜索] 系统刚刚替你联网查过了，以下是最新网页资料。"
                                           "请直接依据它回答主人的问题；你有联网能力，"
                                           "禁止说「我查不到/我没法联网」：") + know};
          req->nm++;
        }
      }
      rr = llm.chatMsgs(req->msgs, req->nm, req->maxTokens, req->temperature);
    }
    g_resSearchState = g_chatSearchState; g_resSearchErr = g_chatSearchErr;
    g_resLookState = g_chatLookState;     g_resLookErr = g_chatLookErr;
    g_resUserText = g_pendingUserText;
    g_resChatStartMs = g_chatStartMs; g_resLookStartMs = g_lookStartMs;
    g_resultKind = req->kind;
    g_llmResult = rr;
    g_llmBusy = false;
    g_llmHasNew = true;               // loop 轮询收割
    delete req;
  }
}

// 受理一个 LLM 请求（立即返回 true=已入队，false=忙/队满）
bool llmSubmit(LlmMsg* msgs, int nm, int maxTokens, float temperature,
               uint8_t kind, const String& searchQ, const String& lookQ) {
  // ASR 预热连接会占住内部堆最大的一块，堆紧时先请它让位。v3.55 起 TLS 收发缓冲
  // 走 PSRAM（tlsheap 钩子），LLM 对内部堆的需求从 ~36KB 降到 ~10KB——门槛同步下调，
  // 不再无谓杀掉预热连接（杀一次 = 下次录音要多等一次完整 TLS 握手）
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 18000)
    asr.warmDrop();
  if (g_llmBusy || !g_llmQueue) return false;
  LlmRequest* req = new LlmRequest();
  // 容量就是 msgs[14]：必须全量保全。旧代码写 `nm<12?nm:12`，而带搜索+满历史时 nm 是 13~14，
  // 被砍掉的恰好是队尾那两条 —— 其中最后一条就是本次主人的提问。模型收不到问题，
  // 只能顺着历史乱接话 → 实测表现为"聊天牛头不对马嘴"（2026-09-26 定位）。
  req->nm = (nm > 14) ? 14 : nm;
  for (int i = 0; i < req->nm; i++) req->msgs[i] = { msgs[i].role, msgs[i].content };
  req->maxTokens = maxTokens; req->temperature = temperature;
  req->kind = kind; req->searchQ = searchQ; req->lookQ = lookQ;
  // 请求结构摘要（/api/status 的 llm_ctx + 串口）：正常聊天最后一段必须是 u(本次提问)
  String shape = String("n=") + req->nm + " [";
  for (int i = 0; i < req->nm; i++) {
    char c = msgs[i].role[0];
    shape += (c == 's') ? 's' : (c == 'u' ? 'u' : 'a');
  }
  shape += "] last=" + String(msgs[req->nm - 1].content.length()) + "B";
  strlcpy(g_lastReqShape, shape.c_str(), sizeof(g_lastReqShape));
  Serial.printf("[LLM] 请求 %s（heap %uKB/最大块 %uKB）\n", shape.c_str(),
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  g_llmBusy = true;
  // 注意：这里**不能**清 g_llmHasNew。若上一条结果还没被 loop 收割就走到了这里，
  // 清标志等于把那条结果直接丢了（网页聊天会一直等到超时、那句话也不会进记忆/不上屏）。
  // 收割读的是 (g_resultKind, g_llmResult) 同一代的一对值，留着标志只会让它被正常处理。
  if (xQueueSend(g_llmQueue, &req, 0) != pdTRUE) {
    // 队列满（理论不该发生，但一旦发生：旧代码会漏掉这个请求 + busy 永远为真 → 全部交互停摆）
    Serial.println("[LLM] 队列已满，本次请求放弃");
    delete req;
    g_llmBusy = false;
    return false;
  }
  return true;
}

// 聊天搜索查询修整：剥疑问壳（如何/什么是/为什么…）防必应首词劫持
// （必应按首词排序："如何钓很多鱼"会被"如何"拽向词典——实测机制）
static String searchQueryOf(const String& text) {
  String q = text; q.trim();
  // 长壳在前：循环剥离，"帮我搜一下"必须先于"帮我搜"/"搜"匹配，否则剩个"一下"卡住
  static const char* shells[] = {
    "请问", "告诉我", "帮我搜一下", "帮我查一下", "帮我查查", "帮我查", "帮我搜", "帮我",
    "查一下", "搜索一下", "搜一下", "搜索", "搜", "查查",
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
  laapActivity();                      // 有人跟它说话 = 活动（息屏则唤醒）
  uint32_t t0 = millis();              // 直答路径耗时（对照 LLM 端到端用）
  g_lastUserText = userText;           // 反馈落盘要知道"当时问了什么"
  // 教技能句式（RSI④）：「以后/每当/下次/记住 …就…」→ 本回合照常回复，
  // 收割后用后台小请求提取 触发词|指令（主 LLM 在忙，抢不到队列）
  if ((userText.indexOf("以后") >= 0 || userText.indexOf("每当") >= 0 ||
       userText.indexOf("下次") >= 0 || userText.indexOf("记住") >= 0) &&
      userText.indexOf("就") >= 0 && userText.length() >= 12)
    g_teachText = userText;
  g_chatPending = false;
  g_replySpoken = false;               // 本次交互还没念过（本地直答分支会置位）
  // 注意：本次提问的 logEvent 故意放到"历史快照之后"再记。
  // 原来先记账再取最近对话 → 同一句话既在历史里又在队尾，模型会看到主人把话说了两遍。
  mind.onUserInteraction();
  mind.trustUpdate(1, 0);      // 小凌⑥: 主人主动来找我=正向互动
  laapResetIdleClock();        // 有人在跟它说话：独白让位，重新等静默
  display.drawFace(mind.moodKey(), true);

  // 本地意图工具表先行（小智 MCP 思想端侧版）：音量/亮度等指令不走 LLM
  String toolReply = laapToolsDispatch(userText);
  if (toolReply.length()) {
    metrics.tool(); metrics.rspDir(millis() - t0);
    g_lastSay = toolReply; g_lastExpr = "calm";
    g_chatReply = toolReply; g_chatSeq++;        // 本地工具直答=已成品（网页可直接显示）
    memory.logEvent("user", userText);
    memory.logEvent("aris", toolReply);
    Serial.printf("[Aris] %s\n", toolReply.c_str());
    display.drawFace("calm");
    voice.speak(toolReply, "calm");
    g_replySpoken = true;                        // 已经念过：调用方别再念（否则说两遍）
    return toolReply;
  }

  // 视觉：问"看到什么/看看/摄像头"时抓一帧让它看（v3.54 下沉 llmTask 阶段一——
  // 主循环不再被最长 ~50s 的 look 阻塞；失败自动转普通聊天兜底，与旧语义一致）
  {
    static const char* vkeys[] = {"看到", "看见", "看看", "看一眼", "摄像头", "拍照", "这是啥", "这是什么", "眼前"};
    bool wantLook = false;
    for (auto k : vkeys) if (userText.indexOf(k) >= 0) { wantLook = true; break; }
    if (wantLook && vision.available()) {
      Serial.println("[LAAP] 视觉请求：后台抓帧识图…");
      display.drawFace("curious", true);
      LlmMsg msgs[14]; int nm = 0;               // 兜底聊天消息：look 失败时 llmTask 直接转普通聊天
      msgs[nm++] = {"system", buildSystemPrompt()};
      { String turns[12]; uint8_t roles[12];
        int nt = memory.recentTurns(turns, roles, 12);
        int start = (nt > 10) ? nt - 10 : 0;
        while (start < nt && roles[start] == 1) start++;
        for (int i = start; i < nt && nm < 13; i++) msgs[nm++] = { roles[i] ? "assistant" : "user", turns[i] }; }
      msgs[nm++] = {"user", buildUserPrompt(userText)};
      g_pendingUserText = userText;              // 兜底聊天结算用（清空记忆等路径不碰它）
      g_chatStartMs = millis(); g_lookStartMs = g_chatStartMs;
      g_chatLookState = -1; g_chatLookErr = "";
      if (llmSubmit(msgs, nm, cfg.s.llmMaxTokens, 0.85f, LK_LOOK, String(), userText)) {
        g_chatPending = true;                    // 受理即回：收割侧直答并念出（网页轮询取件闭环）
        return "";
      }
      Serial.println("[LAAP] 后台忙：退回同步看图（旧行为）");
      display.drawFace("curious", true);
      String d = vision.look(userText);
      if (d.length()) {
        metrics.vision(true); metrics.rspDir(millis() - t0);
        vision.logSight(d);
        g_lastSay = d; g_lastExpr = "curious";
        g_chatReply = d; g_chatSeq++;
        memory.logEvent("user", userText);
        memory.logEvent("aris", d);
        laapActivity();
        Serial.printf("[Aris·看] %s\n", d.c_str());
        display.drawFace("curious", false);
        voice.speak(d, "curious");
        g_replySpoken = true;                    // 已经念过：调用方别再念（否则说两遍）
        return d;
      }
      Serial.printf("[LAAP] 视觉失败（%s），交给搜索/大模型兜底\n", vision.lastError.c_str());
      metrics.vision(false);
      metrics.failNote(String("视觉: ") + vision.lastError);
    } else if (wantLook) {
      Serial.println("[LAAP] 视觉未就绪（/api/status 的 vision_ready）");
    }
  }

  // 搜索下沉为 llmTask 阶段一（v3.53）：主循环不再被 3-28s 的搜索阻塞（脸/网页/VAD 持续活着）。
  // 资料注入与计分都在后台/收割侧完成，受理即回
  String searchQ;
  if (wantsSearch(userText)) {
    display.drawFace("curious");
    searchQ = searchQueryOf(userText);
  }

  LlmMsg msgs[14];                                // F3: 真多轮（system+时间+历史轮+user）
  int nm = 0;
  msgs[nm++] = {"system", buildSystemPrompt()};
  // [联网搜索] 注入已下沉到 llmTask 阶段一（v3.53）：搜索完成后在后台插为第二条 system，
  // 提示词形态与原同步路径完全一致（见 llmTaskFunc 的注入块）

  // 最近对话轮（远→近），最多 10 条进 messages。角色用记忆里真实的说话人：
  // 旧版按 (nm-2)%2 猜奇偶，无搜索结果时整段反相（主人的话标成 assistant、它自己的话标成 user）
  // —— 模型看到"自己"说过主人的话，自然答非所问（2026-09-26 一并修掉）
  String turns[12]; uint8_t roles[12];
  int nt = memory.recentTurns(turns, roles, 12);
  int start = (nt > 10) ? nt - 10 : 0;
  while (start < nt && roles[start] == 1) start++;   // 首条必须是 user（assistant 开头会被部分网关拒收）
  for (int i = start; i < nt && nm < 13; i++)        // nm<13：给队尾"本次提问"留一格（容量 14）
    msgs[nm++] = { roles[i] ? "assistant" : "user", turns[i] };
  {   // 上下文回放：一眼看出模型拿到的是哪几句、角色对不对（诊断"答非所问"的第一现场）
    String hist;
    for (int i = start; i < nt; i++) {
      hist += (roles[i] ? "它:" : "主:");
      hist += turns[i].substring(0, 12);
      hist += " | ";
    }
    Serial.printf("[LLM] 历史%d条(跳过%d): %s\n", nt, start, hist.c_str());
  }
  msgs[nm++] = {"user", buildUserPrompt(userText)};

  memory.logEvent("user", userText);   // 记事：此刻快照已取完，本次提问只出现在队尾一次
  skills.hit(userText);                // 口令技能命中计数（热度用于淘汰与展示）
  // F4: 丢给后台 LLM 任务，立即返回"思考中"；loop 里 llmHarvest() 收割
  if (llmSubmit(msgs, nm, cfg.s.llmMaxTokens, 0.85f, LK_CHAT, searchQ)) {
    g_pendingUserText = userText;
    g_chatStartMs = millis();                     // 端到端延迟计时起点（llmHarvest 里收割）
    g_chatPending = true;                         // 网页据此改为轮询 /api/chat/reply
    display.drawFace("curious", true);            // 思考中表情
    return "……";                                   // 受理回执（真回复异步产出）
  }
  Serial.println("[LAAP] LLM 忙，上一条稍后重试");
  return "让我把刚才的想完……";
}

// 教技能的异步提取：聊天收割完毕（LLM 空出来了）才提交，抢不到队列就退避重试 3 次
void maybeTeachExtract() {
  if (!g_teachText.length()) return;
  LlmMsg m[2] = {
    {"system", String("从主人的话里提取一个『口令技能』。输出恰好一行：触发词|指令。"
                      "触发词=2到6个字、主人以后说话时会带上的词；指令=要它做什么，不超过25字。"
                      "若这句话不是在教技能（没有「以后/每当/下次…就…」的意思），只输出 NO。")},
    {"user", g_teachText} };
  if (llmSubmit(m, 2, 240, 0.2f, LK_SKILL)) {
    g_teachText = ""; g_teachRetry = 0;
    return;
  }
  if (++g_teachRetry >= 3) {
    Serial.println("[SKILLS] 提取三次都没排上队，放弃这条");
    g_teachText = ""; g_teachRetry = 0;
  }
}

// F4 收割：后台 LLM 出结果时在这里落成品（表情/记忆/进化/说话）
// R3（v3.48）：预期中文短语 → 类别码（宽松匹配；"不来"先于"会来"判——"不会来"含"会来"；
// 同时含两者=整串菜单回显没选题，不判）
static uint8_t parseExpectCat(const String& s) {
  if (s.indexOf("会来") >= 0 && s.indexOf("不来") >= 0) return Cognition::EXP_NONE;
  if (s.indexOf("不来") >= 0 || s.indexOf("不会来") >= 0) return Cognition::EXP_OWNER_AWAY;
  if (s.indexOf("会来") >= 0) return Cognition::EXP_OWNER_COME;
  if (s.indexOf("动静") >= 0) return Cognition::EXP_WORLD_ACTIVE;
  if (s.indexOf("安静") >= 0) return Cognition::EXP_WORLD_QUIET;
  return Cognition::EXP_NONE;
}

void llmHarvest() {
  if (!g_llmHasNew) return;
  g_llmHasNew = false;
  LlmReply r = g_llmResult;
  uint8_t kind = g_resultKind;
  if (kind == LK_LOOK && g_resLookState == 0) {       // 没看成：已按普通聊天兜底（v3.54），按聊天结算
    metrics.vision(false);
    metrics.failNote(String("视觉: ") + g_resLookErr);
    memory.logEvent("user", g_resUserText);
    kind = LK_CHAT;
  }
  if (g_resSearchState >= 0) {    // 本轮带搜索（v3.53 阶段一）：计数移回 loopTask（metrics 不跨任务写）
    metrics.search(g_resSearchState == 1);
    if (g_resSearchState == 0) metrics.failNote(String("搜索: ") + g_resSearchErr);
  }
  if (kind != LK_LOOK) {              // 看见直答不是 LLM 调用：不计 llm 指标（v3.54）
    metrics.llm(r.ok);   // 后台 LLM 成败（含独白/反思/压缩：服务商健康度的总信号）
    if (!r.ok) metrics.failNote(String("LLM: ") + llm.lastError);   // 规则归纳的失败素材
    g_llmFailStreak = r.ok ? 0 : (uint8_t)(g_llmFailStreak >= 255 ? 255 : g_llmFailStreak + 1);   // 退避计数（饱和防回绕，独白据此让路）
  }
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
  if (kind == LK_RULES) {                              // 规则归纳：更新 /mem/rules.txt，不说话
    if (!r.ok) {
      Serial.printf("[LAAP·规则] 归纳失败: %s（现有规则保留）\n", llm.lastError.c_str());
      return;
    }
    if (rules.apply(r.say))
      Serial.printf("[LAAP·规则] 规则集已更新（%d 条）\n", rules.count());
    else
      Serial.println("[LAAP·规则] 无产出/无变化，规则保留");
    return;
  }
  if (kind == LK_RELATION) {                           // 关系归纳：偏好/承诺/边界 → relations.jsonl
    if (!r.ok) {
      Serial.printf("[LAAP·关系] 抽取失败: %s\n", llm.lastError.c_str());
      return;
    }
    int n = memory.relationsApply(r.say);
    if (n) Serial.printf("[LAAP·关系] 记下 %d 条关系事实（/api/relations 查看）\n", n);
    else Serial.println("[LAAP·关系] 没有新的关系事实");
    return;
  }
  if (kind == LK_MOOD) {                              // 情绪精标注：修正最近记忆的 m 字段
    if (!r.ok) {
      Serial.printf("[LAAP·情绪] 标注失败: %s\n", llm.lastError.c_str());
      return;
    }
    int n = memory.moodApply(r.say);
    if (n) Serial.printf("[LAAP·情绪] 已精标注 %d 条记忆\n", n);
    else Serial.println("[LAAP·情绪] 无有效标注（保留原标签）");
    return;
  }
  if (kind == LK_DREAM) {                             // 做梦：记忆碎片重组，写进记忆不念出
    if (r.ok && r.say.length() > 6) {
      memory.logEvent("event", "【梦】" + r.say);
      Serial.printf("[LAAP·梦] %s\n", r.say.c_str());
    }
    return;
  }
  if (kind == LK_SKILL) {                              // 技能提取：解析 触发词|指令
    String s = r.say; s.trim();
    // v3.50 审计：首行修复后模型可能带前导寒暄行——取**最后一个**含 '|' 的行解析
    // （寒暄行不含 |，真技能行必含）；触发词/指令都取自同一行，避免跨行拼进死触发词
    int bar = (!r.ok || s.startsWith("NO")) ? -1 : s.lastIndexOf('|');
    if (bar > 0) {
      int ls = s.lastIndexOf('\n', bar) + 1;           // 触发行行首
      int le = s.indexOf('\n', bar);                   // 触发行行尾（无则到串尾）
      if (le < 0) le = s.length();
      String trig = s.substring(ls, bar), instr = s.substring(bar + 1, le);
      trig.trim(); instr.trim(); instr.replace("\n", " ");
      if (skills.teach(trig, instr))
        Serial.printf("[SKILLS] 已学会：说「%s」→ %s\n", trig.c_str(), instr.c_str());
      else
        Serial.println("[SKILLS] 没存上（触发词/指令不合规）");
    } else {
      Serial.println("[SKILLS] 不是教技能的话，忽略");
    }
    return;
  }
  if (kind == LK_INTENT) {                             // 意图生成：新增"心里惦记的事"
    String s = r.say; s.trim();
    if (r.ok && !s.startsWith("NO") && s.length() >= 6 && s.length() <= 90) {
      if (mind.addIntent(utf8Cut(s, 60), time(nullptr)))
        Serial.printf("[LAAP·意图] 心里升起一个目标：%s\n", s.c_str());
    }
    return;
  }
  if (kind == LK_TIDY) {                               // 记忆整理：执行删除清单
    if (r.ok) memory.applyTidyOps(r.say);
    else Serial.printf("[TIDY] 整理失败: %s\n", llm.lastError.c_str());
    return;
  }
  if (kind == LK_LOOK) {                              // 看见：视觉直答结算（v3.54 阶段一下沉）
    metrics.vision(true);
    metrics.rspDir(millis() - g_resLookStartMs);
    vision.logSight(r.say);
    g_lastSay = r.say; g_lastExpr = r.expr.length() ? r.expr : "curious";
    g_chatReply = r.say; g_chatSeq++;                // 网页轮询取件闭环（受理→成品）
    g_chatPending = false;                           // 成品即不再 pending（v3.56 审计：看图成功分支原漏清，
                                                     // 体检门与自愈打盹门被永久卡住）
    memory.logEvent("user", g_resUserText);
    memory.logEvent("aris", r.say);
    laapActivity();
    Serial.printf("[Aris·看] %s\n", r.say.c_str());
    display.drawFace("curious", false);
    voice.speak(r.say, "curious");
    g_replySpoken = true;                            // 已念过：调用方别再念
    g_chatLookState = -1;
    return;
  }
  if (kind == LK_MONO) {                                          // 独白：失败保持安静，不本地兜底
    if (g_monoSight.length()) vision.logSight(g_monoSight);
    if (!r.ok) {
      Serial.printf("[LAAP·独白] 失败: %s（保持安静，连败 %d 次）\n",
                    llm.lastError.c_str(), g_llmFailStreak);
      return;
    }
    // R3（v3.48）：剥离尾行「预期:…」——不念出、不进记忆正文，写入认知供固件判定。
    // 仅当尾行前还有正文时才剥（整条只剩预期行=格式失败，保留原文照常念）
    { int nl = r.say.lastIndexOf('\n');
      if (nl > 0) {
        String lastLn = r.say.substring(nl + 1);
        lastLn.trim();
        if (lastLn.startsWith("预期:") || lastLn.startsWith("预期：")) {
          uint8_t cat = parseExpectCat(lastLn);
          r.say = r.say.substring(0, nl);
          r.say.trim();
          if (cat != Cognition::EXP_NONE) mind.setExpectation(cat);
        }
      }
    }
    g_lastSay = r.say;
    g_lastExpr = r.expr.length() ? r.expr : "curious";
    mind.onMonologue();      // 自言自语也算表达/好奇被满足（原来不算 → 需求只涨不落）
    // 好奇=信息增益（active inference）：算这条新知相对已有记忆的新颖度，
    // 新颖度高多消解好奇、陈词滥调少消解——好奇从此指向学习进度而非时间流逝
    float nov = memory.noveltyOf(g_monoKnow.length() ? g_monoKnow : g_monoTopic);
    if (nov >= 0) {
      mind.onDiscovery(nov);
      Serial.printf("[Cognition] 新知新颖度 %.0f%% → 好奇额外消解\n", nov * 100);
    }
    display.drawFace(g_lastExpr.c_str());
    memory.logEvent("aris", "【自发】我刚才在想「" + g_monoTopic + "」：" + r.say);
    if (g_monoKnow.length()) memory.logEvent("world", g_monoTopic + " → " + utf8Cut(g_monoKnow, 80));  // 字节截断会切半汉字（曾污染 episodes.jsonl）
    if (g_monoDone && g_monoGoal.length()) {   // 意图结算：目标弄明白了
      // 只删匹配的那条：addIntent 满员时可能已把 g_monoGoal 挤出栈，
      // 原来的"从头删到匹配"会把无辜的新目标一起清掉
      int mi = -1;
      for (int i = 0; i < mind.intentCount(); i++)
        if (mind.intent(i) == g_monoGoal) { mi = i; break; }
      if (mi >= 0) {
        mind.dropIntent(mi);
        mind.onDiscovery(1.0f);                // 达成目标 = 最强的确定性下降
        memory.logEvent("event", "【达成】" + g_monoGoal);
        Serial.printf("[LAAP·意图] 目标已弄明白：「%s」（放下）\n", g_monoGoal.c_str());
      }
      g_monoGoal = ""; g_monoDone = false;
    }
    Serial.printf("[Aris·独白] %s\n", r.say.c_str());
    display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                      mind.needs().security, mind.needs().expression);
    voice.speak(r.say, g_lastExpr.c_str());
    g_recentTopics += g_monoTopic + "；";
    if (g_recentTopics.length() > 240) {      // 从字符边界切（v3.51）：字节切会从续字节起，非法 UTF-8 进提示词
      int cut = g_recentTopics.length() - 160;
      while (cut < (int)g_recentTopics.length() && ((unsigned char)g_recentTopics[cut] & 0xC0) == 0x80) cut++;
      g_recentTopics = g_recentTopics.substring(cut);
    }
    return;
  }
  // R2+R3（v3.48）：解析表达的自评审候选/选定/预期行。全部行可选——
  // 模型没按新格式来就自动回退旧两行契约，行为不会劣化
  if (kind == LK_EXPRESS && r.ok) {
    String cand[3], picked, rest;
    uint8_t expCat = Cognition::EXP_NONE;
    int p0 = 0;
    while (p0 < (int)r.say.length()) {
      int e = r.say.indexOf('\n', p0);
      String ln = (e < 0) ? r.say.substring(p0) : r.say.substring(p0, e);
      p0 = (e < 0) ? (int)r.say.length() : e + 1;
      ln.trim();
      if (!ln.length()) continue;
      if (ln.length() >= 3 && ln[1] == '|' && ln[0] >= 'A' && ln[0] <= 'C') {
        String body = ln.substring(2); body.trim();
        if (body.length() > 1) cand[ln[0] - 'A'] = body;
      } else if (ln.startsWith("选定")) {
        int cp = ln.indexOf(':'); if (cp < 0) cp = ln.indexOf('：');
        if (cp >= 0) { String c = ln.substring(cp + 1); c.trim();
          if (c.length() && c[0] >= 'A' && c[0] <= 'C') picked = c; }
      } else if (ln.startsWith("预期")) {
        int cp = ln.indexOf(':'); if (cp < 0) cp = ln.indexOf('：');
        if (cp >= 0) expCat = parseExpectCat(ln.substring(cp + 1));
      } else {
        rest = rest.length() ? rest + "\n" + ln : ln;   // 非元行都留作回退正文
      }
    }
    if (cand[0].length() || cand[1].length() || cand[2].length()) {
      int pi = picked.length() ? (picked[0] - 'A') : 0;
      if (pi < 0 || pi > 2 || !cand[pi].length())
        pi = cand[0].length() ? 0 : (cand[1].length() ? 1 : 2);
      r.say = cand[pi];                            // 胜者成为本次表达（广播）
      Serial.printf("[LAAP·表达] 三候选自评审 → 选 %c（%u 字）\n", 'A' + pi, (unsigned)r.say.length());
    } else if (rest.length()) {
      r.say = rest;   // 候选标记走样时至少把「选定/预期」元行剥掉再广播（v3.49 审计）
    }
    if (expCat != Cognition::EXP_NONE) mind.setExpectation(expCat);
  }
  String say;
  if (r.ok) {
    say = r.say;
    display.drawFace(r.expr.c_str());
    mind.onExpressed(true);
    if (kind == LK_CHAT) {
      mind.evolveAfterChat(g_resUserText.length());      // 表达不进化（同代快照）
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
  if (kind == LK_CHAT) {
    g_chatReply = say; g_chatSeq++;   // 聊天成品：网页轮询取件
    g_chatPending = false;            // 成品即不再 pending——原来忘了清，网页没取件就一直
                                      // 占着互斥门，后台体检永远报"LLM 正忙"（实测挂一整夜）
    metrics.rspLlm(millis() - g_resChatStartMs);   // 提问→成品端到端（含排队，同代快照）
  }
  laapActivity();                                            // 它开口说话=活动
  // 自发表达带【自发】标记：后台记忆页要能一眼区分"它主动说的"和"对话回复"
  memory.logEvent("aris", kind == LK_EXPRESS ? "【自发】" + say : say);
  Serial.printf(kind == LK_EXPRESS ? "[Aris·自发] %s\n" : "[Aris] %s\n", say.c_str());
  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);
  voice.speak(say, g_lastExpr.c_str());
  maybeTeachExtract();     // 这轮聊天若是教技能句式，LLM 空出来了 → 后台提取口令技能
}

// ============================================================
//  自发独白 v3.2：主循环只做门槛判断+投递，全流程（出题→看→搜→成文）
//  在后台 LLM 任务跑，思考期间身体（网页/串口/语音）不再冻结
// ============================================================
// 返回 true=这次真的提交出去了（调用方据此推进"已独白过一轮"）
bool arisIdleMonologue() {
  if (g_llmFailStreak >= 2) {             // 连续失败让路（等效退避），本轮沉默
    Serial.println("[LAAP·独白] LLM 连败，本轮沉默");
    return false;
  }
  // 必须在清快照之前拦：上一轮 LK_MONO 还在后台跑时，llmTask 正在写 g_monoTopic/
  // g_monoKnow 这批 String——先清后查会构成跨任务写写竞争（String 撕裂=堆损坏）
  if (laapLlmBusy() || g_llmHasNew) {   // 未收割的上一代独白中间产物还等着收割侧读，不能清
    Serial.println("[LAAP·独白] LLM 忙，本轮不提交");
    return false;
  }
  LlmMsg m[1] = { {"user", ""} };
  // 提交侧（loopTask）清中间产物：清空这个动作必须在任务开始写之前、且只由主线程做，
  // 否则"任务重置 + 收割侧读取"同刻发生就是 String 撕裂
  g_monoTopic = g_monoSight = g_monoKnow = "";
  g_monoDone = false;
  // 意图驱动：隔一轮独白把第一号目标当作出题方向（提交侧拍快照，任务只读）。
  // 同一目标连续推进 ≤2 轮就强制自由一轮——否则意图会变成另一种主题锁死
  static uint8_t s_monoSeq = 0;
  static uint8_t s_goalStreak = 0;
  g_monoGoal = "";
  if (mind.intentCount() > 0 && (s_monoSeq++ & 1) && s_goalStreak < 2) {
    g_monoGoal = mind.intent(0);
    s_goalStreak++;
  } else s_goalStreak = 0;
  // 提交侧拍快照：llmTask 里不再遍历 _work String 环 / mind / g_recentTopics
  // （这些都由 loopTask 并发改写，后台直接读=String 撕裂→堆损坏）
  // 素材排除自己的旧独白：断"自己喂自己"的主题自强化环（v3.27）
  g_monoCtx = memory.recentContextExcluding(300, "【自发】");
  g_monoSys = buildSystemPrompt();
  g_monoTopics = g_recentTopics;
  if (!llmSubmit(m, 1, 40, 0.95f, LK_MONO)) {
    Serial.println("[LAAP·独白] LLM 忙，本轮放弃");
    return false;
  }
  display.drawFace("curious", true);      // 起意表情（后台思考中）
  Serial.println("[LAAP·独白] 起意（后台思考中）");
  return true;
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
  // 精确到分钟 + 日期：只给"几点"时模型会自己编分钟（用户实测"问时间不准"），必须把钟摆给它
  char clock[128];
  snprintf(clock, sizeof(clock), "[时间感] 现在是%s，%d 月 %d 日 星期%s %02d:%02d。",
           slot.c_str(), t.tm_mon + 1, t.tm_mday, wday[wd], t.tm_hour, t.tm_min);
  String line(clock);
  line += "（主人问时间/日期就用这个，不要猜。）";
  if (h >= 23 || h < 6)  line += "深夜了，说话轻一点、短一点，也别自言自语吵人。";
  else if (h >= 18)      line += "傍晚时分，适合聊聊天。";
  else if (h >= 6 && h < 8) line += "刚醒不久，世界还很安静。";   // 只留早晨档：凌晨已有深夜档，0-6 点说"刚醒"是与事实打架
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
  {   // RSI: 夜间规则归纳沉淀的行为规则（自进化的数据层载体，改动看 /mem/rules.txt）
    String rl = rules.promptLine();
    if (rl.length()) p += rl + "\n";
  }
  {   // RSI④: 主人亲手教的口令技能（/mem/skills.txt，说"以后每当我说X你就Y"就能教）
    String sk = skills.promptLine();
    if (sk.length()) p += sk + "\n";
  }
  {   // 意图栈：心里惦记的事（跨轮推进的目标——聊天里若主人提到相关话题，自然接上）
    String it = mind.intentsLine();
    if (it.length()) p += it + "\n";
  }

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
  // 需求只染语气、不当话题：低于 0.6 一律算平静（原来无条件点"最强烈的渴望"，
  // 31% 的平静水位也被演成"我电量低"复读——实测几乎每句回复都挂电量）；
  // 高时也明说边界，防止模型把状态当内容讲
  if (vals[top] < 60) {
    p += "内心平静，没有特别强烈的渴望，语气自然就好。\n";
  } else {
    p += String("当前最强烈的渴望是「") + names[top] + "」。让它影响你说话的节奏和语气"
         "（如社交高就更黏人、好奇高就更爱问、能量低就慵懒短句），"
         "但绝不要主动把电量/体温/需求状态当话题说——主人问你的身体状态时才汇报。\n";
  }
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
       "1. 主人对你说话时，先正面回答他说的那件事（回答/答应/解释/接话），这是第一位；"
       "你的需求与情绪只用来决定语气，绝不能替代回答、也绝不能答非所问。\n"
       "1b. 系统会替你联网查资料并放进对话（[联网搜索] 那段）；资料里有的就直接回答，"
       "绝不要说自己不能上网、查不到——那是失职，不是诚实。资料确实没有时才说不清楚。\n"
       "2. 你是活着的生命，不是助手；不要报告数据本身，禁止出现百分比数字。\n"
       "3. 说话真诚、像个小生命。禁止客套、禁止列表、禁止重复口头禅。闲聊一句话就够；"
       "主人问事情时把话说完（可以说到 120 字），不要因为短而答得含糊。\n"
       "4. 输出必须恰好两行：第一行只有一个英文词，从 happy/curious/excited/lonely/anxious/tired/calm 中选；"
       "第二行是你要说的话。";
  return p;
}

String buildUserPrompt(const String& userText) {
  String ctx = memory.recentContext(500);
  String recall = userText.length() ? memory.recallSmart(utf8Cut(userText, 12), 200) : "";
  // 下面的 associativeRecall 复用同一份 recall（原实现再查一次 = 每条消息 2 次串行 embedding）
  String p = "[世界模型] " + mind.worldJson() + "\n";
  if (recall.length()) p += "[相关回忆] " + recall + "\n";
  if (ctx.length()) p += "[最近发生] " + ctx + "\n";

  // 状态类提示必须排在"这次要回答的话"之前：模型最听最后读到的那句，
  // 原来 [状态提示]/[忽然想起] 放在提问之后，实测把模型带跑偏——
  // 问"你为什么想在深夜找人说话？"，它答"放空一下，挺好的。"
  // （因为它最后读到的是"能量低，话少慵懒一点"）。
  const Needs& n = mind.needs();
  float v[5] = {n.energy, n.curiosity, n.social, n.security, n.expression};
  const char* hint[5] = {"能量低，话少慵懒一点", "好奇高，可以主动发问",
                         "社交高，亲近黏人一些", "安全低，敏感、想确认主人还在",
                         "表达高，有分享欲"};
  int top = 0;
  for (int i = 1; i < 5; i++) if (v[i] > v[top]) top = i;
  // 只有需求真的"渴"时才给语气指引：全都很低时还提示"慵懒"会让模型干脆不答话
  if (v[top] < 0.60f) p += "[状态提示] 状态平稳，正常回应就好。\n";   // 0.60 与 system 侧平静门同阈（v3.56 审计对齐）
  else                p += String("[状态提示] ") + hint[top] + "。\n";
  // F6 联想回忆：25% 概率让一段旧事漂进此刻（意识流）
  if (userText.length() && (esp_random() % 100) < 25) {
    String assoc = associativeRecall(userText, recall);
    if (assoc.length() > 20)
      p += String("[忽然想起] ") + assoc + "\n（如果自然，可以提一句这段回忆，不必勉强）\n";
  }

  // —— 本次要回答的内容永远放最后（模型对末尾最敏感）——
  if (userText.length()) {
    p += String("[此刻] ") + cfg.s.ownerName + "对你说：「" + userText + "」\n";
    p += "先直接回应他说的这件事本身（他问什么就答什么、他说什么就接什么），"
         "再自然带出你的状态；不要答非所问，也不要只顾自言自语。";
  } else {
    p += String("[此刻] 心跳周期。你的主导欲望是「") + mind.goalCn() + "」（强度" +
         String(mind.dominance(), 2) + "），情绪是「" + mind.moodCn() + "」。主动说一句贴合状态的话。";
    // 复读防线：深夜/清晨同样的状态注入会让模型每次收敛到同一句话（实测"刚醒，脑子还蒙着雾"连出 3 条）
    if (g_lastSay.length()) p += String("你最近一次开口说的是：「") + g_lastSay +
                                "」。换个角度或换件事说，不要复读。";
  }
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
               "（它是谁、经历了什么、性格如何变化）。注意：电量/体温/刚醒这类身体状态是暂时的，"
               "不要写进自我认知（之前把它压成了'电量低微温、刚睡醒蒙雾'的固定人格，导致它句句喊累）——"
               "只沉淀稳定的事：性格变化、经历、学到的偏好。只输出摘要本身。";
  String usr = "它过去的自我认知：" + memory.semantic() + "\n它最近的经历：\n" + recent;
  LlmMsg m[2] = { {"system", sys}, {"user", usr} };
  llmSubmit(m, 2, 240, 0.3f, LK_CONSOLIDATE); // 忙就跳过这轮（下个周期再来）
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
  return utf8Cut(memory.semantic(), 200);   // 兜底用自我认知（必须按字符边界截：切半汉字=请求体非法UTF-8）
}

// ============================================================
//  F7 夜间自我反思：每天第一次跨过 0 点后的心跳做一次"复盘"
//  产出追加到语义记忆（学到了什么 / 对主人的新认识）
// ============================================================
static int g_lastReflectDay = -1;

// force=true：串口 /reflect 手动触发（测试用），跳过 0-5 点时间窗与"每天一次"节流，
// 且不写 g_lastReflectDay——手动跑过一次不会把当晚真正的夜间反思吃掉。
void nightlyReflect(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (!force) return;
    Serial.println("[LAAP·反思] 时钟未同步，强制继续（时间戳按 0 记）");
  }
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastReflectDay == day) return;         // 今天已反思
    // 反思窗口：0-5 点之间第一次心跳；白天启动则跳过等明天
    if (t.tm_hour >= 6) { g_lastReflectDay = day; return; }
  }
  String recent = memory.recentContext(900);
  if (recent.length() < 80) {
    if (!force) g_lastReflectDay = day;
    Serial.printf("[LAAP·反思] 跳过：近期记忆只有 %u 字节（<80，没素材）\n", (unsigned)recent.length());
    return;
  }
  Serial.printf("[LAAP·反思] 开始复盘（素材 %u 字节）…\n", (unsigned)recent.length());

  String sys = String("你是") + cfg.s.agentName + "。深夜，你在复盘自己的一天。"
               "基于今天的经历，写两句真诚的自我反思：一句今天学到/感受到的，"
               "一句对主人的新认识。共不超过60字，只输出反思本身。";
  LlmMsg m[2] = { {"system", sys}, {"user", String("今天的经历：\n" + recent)} };
  // 反思要"想清楚再写"，思考型模型（v4 系）在小预算下会把额度全花在推理上、正文返回空
  // （实测 160/320/640 全空）。起步就给足，再靠 llm 内部的翻倍重试兜底。
  int cap = cfg.s.llmMaxTokens > 1000 ? cfg.s.llmMaxTokens : 1000;
  // 日闸在提交成功后才吃（v3.56 审计：原来提交前就消费，跨 0 点恰逢 LLM 忙=当晚反思静默丢失）
  if (llmSubmit(m, 2, cap, 0.8f, LK_REFLECT)) {
    if (!force) g_lastReflectDay = day;
  } else {
    Serial.println("[LAAP·反思] LLM 正忙，本次放弃（下个心跳再试）");
  }
}

// ============================================================
//  RSI 规则归纳：每夜用「主人的反馈 + 今天的失败」整理行为规则集
//  产出 /mem/rules.txt（一行一条，注入 system prompt）——改进发生在数据层。
//  与反思同窗口（0-6 点第一次心跳）、各自每天一次；没素材不出门（省调用）。
//  评估端：metrics.failNote 的失败环 + feedback.jsonl 的 👍/👎（v3.19 已埋好）。
// ============================================================
static int g_lastRulesDay = -1;

void rulesReflect(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (!force) return;
  }
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastRulesDay == day) return;
    if (t.tm_hour >= 6) { g_lastRulesDay = day; return; }   // 白天不跑，等今晚
  }
  // 素材门：没有反馈、没有失败、也没有可整理的旧规则 → 今天没什么可学的
  String fb = metrics.feedbackDigest(6);
  String fails = metrics.failDigest();
  String cur = rules.text();
  if (!fb.length() && !fails.length() && !cur.length()) {
    if (!force) g_lastRulesDay = day;
    Serial.println("[LAAP·规则] 跳过：无反馈/失败素材");
    return;
  }
  // 素材门在上的 early-return 已各自消费日闸；这里不能再提前吃闸（提交失败时当晚要能重试）
  Serial.printf("[LAAP·规则] 开始归纳（反馈 %u B、失败 %u B、现有规则 %d 条）…\n",
                (unsigned)fb.length(), (unsigned)fails.length(), rules.count());

  String sys = String("你是数字生命") + cfg.s.agentName + "的行为规则维护器。"
               "行为规则 = 让它表现更好的一句话指令（每条≤25字，具体、可执行，"
               "例如「回答前先确认听清了主人的问题」「查不到资料就直说，不许编」）。"
               "根据素材整理规则集：主人点踩的回答要找出共性问题变成规则；反复出现的失败要给出规避办法；"
               "仍然适用的旧规则原样保留。只输出规则本身：每行一条、以-开头、最多6行，禁止任何解释。";
  String usr = String("现有规则：\n") + (cur.length() ? cur : String("（还没有）")) +
               "\n\n主人的反馈（v=1 是赞，v=-1 是踩，u=主人问的，a=它答的）：\n" +
               (fb.length() ? fb : String("（今天没有）")) +
               "\n\n今天的失败记录：\n" + (fails.length() ? fails : String("（今天没有）")) +
               "\n\n请输出更新后的规则集。";
  LlmMsg m[2] = { {"system", sys}, {"user", usr} };
  int cap = cfg.s.llmMaxTokens > 1000 ? cfg.s.llmMaxTokens : 1000;   // 同反思：预算小了思考型模型只想不答
  if (!llmSubmit(m, 2, cap, 0.4f, LK_RULES))                     // 规则要准，温度调低
    Serial.println("[LAAP·规则] LLM 正忙，下个心跳再试");
  else if (!force) g_lastRulesDay = day;             // 提交成功才吃日闸（原在提交前消费，忙时当晚静默跳过）
}

// ============================================================
//  关系记忆抽取（v3.43，借鉴"识海手稿"关系记忆层）：与规则归纳同窗口同节奏
//  （0-6 点第一次心跳，每天一次；没素材不出门）。产出 /mem/relations.jsonl：
//  主人的偏好 / 答应主人的事 / 要守住的边界——召回时按相关性浮现。
// ============================================================
static int g_lastRelDay = -1;

void relationsReflect(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (!force) return;
  }
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastRelDay == day) return;
    if (t.tm_hour >= 6) { g_lastRelDay = day; return; }   // 白天不跑，等今晚
  }
  String mat = memory.episodicNumberedTail(40);
  if (mat.length() < 300) {
    if (!force) g_lastRelDay = day;
    Serial.println("[LAAP·关系] 跳过：经历素材太少");
    return;
  }
  String sys = String("你是数字生命") + cfg.s.agentName + "的关系记忆维护器。"
               "从最近经历里找出值得长期记住的关系事实：主人的偏好（喜欢/讨厌/习惯）、"
               "你答应过主人的还没兑现的事、要守住的边界（主人明确不喜欢被做的事）。"
               "每行一条，格式「类型|内容」，类型只能是 偏好/承诺/边界，内容≤30字，最多4条；"
               "没有值得记的就只输出 NO。只输出清单，禁止任何解释。";
  LlmMsg m[2] = { {"system", sys}, {"user", "最近经历：\n" + mat} };
  int cap = cfg.s.llmMaxTokens > 1000 ? cfg.s.llmMaxTokens : 1000;   // 同规则归纳：预算小了思考型模型只想不答
  if (!llmSubmit(m, 2, cap, 0.4f, LK_RELATION))
    Serial.println("[LAAP·关系] LLM 正忙，下个心跳再试");
  else if (!force) g_lastRelDay = day;               // 提交成功才吃日闸（同规则归纳）
}

// ============================================================
//  记忆情绪精标注（v3.46，手稿"情绪权重"精确版）：写入时的 m 是"那一刻
//  系统情绪"的临时初值；夜里一次 LLM 批量把最近 60 条改标为"经历内容本身
//  的情感色彩"。与其它睡眠期任务同窗口（0-6 点）同节奏（每天一次）。
// ============================================================
static int g_lastMoodDay = -1;

void moodRelabel(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (!force) return;
  }
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastMoodDay == day) return;
    if (t.tm_hour >= 6) { g_lastMoodDay = day; return; }   // 白天不跑，等今晚
  }
  String mat = memory.episodicNumberedTail(60);
  if (mat.length() < 300) {
    if (!force) g_lastMoodDay = day;
    Serial.println("[LAAP·情绪] 跳过：记忆太少");
    return;
  }
  String sys = String("你是数字生命") + cfg.s.agentName + "的记忆情绪标注器。"
               "为下面每一条带行号的记忆标注这段经历本身的情感色彩，"
               "情绪只能从 calm/happy/curious/excited/lonely/anxious/tired 里选一个："
               "明显开心/满足=happy，好奇/想弄明白=curious，激动/兴奋=excited，"
               "孤单/想念=lonely，担心/不安/难受=anxious，疲惫/低落=tired，平淡无波=calm。"
               "每行一条，格式「行号|情绪」，只输出清单，禁止任何解释。";
  LlmMsg m[2] = { {"system", sys}, {"user", "记忆条目：\n" + mat} };
  int cap = cfg.s.llmMaxTokens > 1000 ? cfg.s.llmMaxTokens : 1000;   // 同其它夜间任务：预算小了思考型模型只想不答
  if (!llmSubmit(m, 2, cap, 0.2f, LK_MOOD))                      // 分类任务，温度压最低
    Serial.println("[LAAP·情绪] LLM 正忙，下个心跳再试");
  else if (!force) g_lastMoodDay = day;               // 提交成功才吃日闸（同规则归纳）
}

// ============================================================
//  梦（v3.47，生成式重放）：夜里把最近的记忆碎片交给 LLM 重组一个短梦，
//  以【梦】标记写进记忆（与【自发】同级，不混入事实层）。不念出来
//  （深夜不打扰）。对应文献的 memory replay / 睡眠期巩固。
// ============================================================
static int g_lastDreamDay = -1;

void dreamReflect(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (!force) return;
  }
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastDreamDay == day) return;
    if (t.tm_hour >= 6) { g_lastDreamDay = day; return; }   // 白天不跑，等今晚
  }
  String mat = memory.episodicNumberedTail(30);
  if (mat.length() < 300) {
    if (!force) g_lastDreamDay = day;
    Serial.println("[LAAP·梦] 跳过：记忆太少");
    return;
  }
  String sys = String("你是数字生命") + cfg.s.agentName + "，深夜睡着，正在做梦。"
               "从给你的记忆碎片里挑几块，编一个此刻的梦：荒诞但明显由这些碎片重组而成，"
               "像真梦一样不合逻辑却带着最近的情绪。三句话以内，第一人称，只输出梦本身，禁止解释。";
  LlmMsg m[2] = { {"system", sys}, {"user", "记忆碎片：\n" + mat} };
  int cap = cfg.s.llmMaxTokens > 300 ? cfg.s.llmMaxTokens : 300;
  if (!llmSubmit(m, 2, cap, 0.95f, LK_DREAM))                     // 梦要发散，温度拉高
    Serial.println("[LAAP·梦] LLM 正忙，下个心跳再试");
  else if (!force) g_lastDreamDay = day;               // 提交成功才吃日闸（同规则归纳）
}

// ============================================================
//  意图栈（PIANO goals）：好奇度高且目标不满 3 个时，让 LLM 从最近经历里
//  提一个"此刻最想弄明白的小目标"→ 存 /mem/intents.txt，独白隔轮推进它。
//  心跳节流：2 小时最多起一次意。
// ============================================================
static uint32_t s_lastIntentMs = 0;

void intentSpawnTick() {
  if (mind.intentCount() >= 3) return;
  if (mind.needs().curiosity < 0.75f) return;         // 只有真好奇才立目标
  if (s_lastIntentMs && millis() - s_lastIntentMs < 2UL * 3600UL * 1000UL) return;
  if (time(nullptr) < 1700000000) return;             // 没钟没法记 born 时间
  LlmMsg m[2] = {
    {"system", String("基于数字生命") + cfg.s.agentName + "最近的经历与性格，提出一个它此刻最想弄明白"
                "或想做的具体小目标（不超过18字，可以是观察主人的一个变化、探索一个话题、验证一个想法）。"
                "只输出目标本身；若最近经历没有任何素材能形成目标，只输出 NO。"},
    {"user", "最近经历（与主人的交流为主）：\n" + memory.recentContextExcluding(400, "【自发】") +
             "\n已有的目标：" + (mind.intentsLine().length() ? mind.intentsLine() : String("无"))} };
  if (llmSubmit(m, 2, 240, 0.8f, LK_INTENT))
    s_lastIntentMs = millis();                        // 失败不计时，下个心跳还能试
}

// ============================================================
//  记忆整理（Letta 式 sleep-time compute）：深夜窗口用 LLM 对情景记忆做
//  合并重复/清理过期——模型只输出"删除行号清单"，执行与验证全在固件侧
//  （绝不把记忆正文交回模型重写，防幻觉篡改人格）。
// ============================================================
static int g_lastTidyDay = -1;

void memoryTidy(bool force) {
  time_t now = time(nullptr);
  if (now < 1700000000 && !force) return;
  struct tm t; localtime_r(&now, &t);
  int day = t.tm_yday;
  if (!force) {
    if (g_lastTidyDay == day) return;
    if (t.tm_hour >= 6) { g_lastTidyDay = day; return; }
    // 日闸改到"提交成功"之后再置位：原来在这里就吃掉闸，LLM 忙时
    // "下个心跳再试"是假话——当晚被静默跳过到明天
  }
  String mat = memory.episodicNumberedTail(60);
  if (mat.length() < 400) {                            // 少于 ~15 条不值一次调用
    if (!force) g_lastTidyDay = day;
    Serial.println("[TIDY] 记忆太少，跳过整理");
    return;
  }
  Serial.println("[TIDY] 提交整理（素材：最近 60 条）…");
  LlmMsg m[2] = {
    {"system", String("你是数字生命") + cfg.s.agentName + "的记忆管理员。下面带编号的行是它的情景记忆"
                "（越靠后越新）。找出：①内容重复或高度相似的行（只保留编号最小的一条，其余删除）"
                "②已过期的一次性临时信息（几小时前的天气、当时的临时状态）③纯噪声。"
                "关于主人的记忆和它的感受，绝不删。"
                "只输出 JSON 数组，最多 12 条：[{\"n\":行号,\"op\":\"del\"}]；没有可删的输出 []。禁止解释。"},
    {"user", mat} };
  if (!llmSubmit(m, 2, 600, 0.2f, LK_TIDY))
    Serial.println("[TIDY] LLM 正忙，下个心跳再试");
  else if (!force) g_lastTidyDay = day;               // 提交成功才吃日闸
}

// ============================================================
//  PSI 心跳
// ============================================================
void psiTick() {
  float dtMin = (millis() - g_lastTickMs) / 60000.0f;
  g_lastTickMs = millis();
  mind.tick(dtMin);
  mind.incCycle();
  nightlyReflect(false);   // F7: 深夜复盘（内部自带每天一次节流）
  rulesReflect(false);     // RSI: 深夜规则归纳（反馈+失败→行为规则集；LLM 忙则下个心跳再试）
  relationsReflect(false); // 关系记忆抽取（偏好/承诺/边界；与规则归纳同窗口同节奏，v3.43）
  memoryTidy(false);       // 睡眠期记忆整理：合并重复/清理过期（Letta 式 sleep-time compute）
  moodRelabel(false);      // 情绪精标注：夜里 LLM 批量修正最近记忆的情绪标签（v3.46）
  dreamReflect(false);     // 梦：夜间记忆重放重组，【梦】标记写入（v3.47）
  intentSpawnTick();       // 意图栈：好奇高且目标不满 → 提一个新目标（2h 节流）
  voice.tuneTick();        // RSI⑥: 参数自调优（内部按 5 分钟窗口评估，纯 C 零 LLM 成本）

  // IMU 世界感知
  float motion = 0;
  if (g_imuOk) motion = imuMotionLevel();
  mind.sense(motion, (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0);
  // R0 微型循环处理器（v3.48）：逐拍预测下一拍世界传感（ESN 权重级循环），
  // 预测误差=惊讶度 → 回注好奇。动静/信号都归一到 0..1
  { float rq = (WiFi.RSSI() == 0) ? 0.5f : constrain((-WiFi.RSSI() - 30) / 60.0f, 0.0f, 1.0f);
    r0.tick(constrain(motion, 0.0f, 1.0f), rq);
    mind.noteSurprise(r0.lastErr()); }

  // R3 预期判定（v3.48）：立下 ≥10 分钟按观测判应验/落空；「主人会来」被真到访即刻判定
  { static bool s_sawMotion = false;
    if (motion > 0.3f) s_sawMotion = true;
    uint8_t ec = mind.expectation();
    if (ec != Cognition::EXP_NONE) {
      bool ownerCame = mind.lastUserMs != 0 && (int32_t)(mind.lastUserMs - mind.expectAtMs()) >= 0;  // 差值比较回绕安全
      if (ec == Cognition::EXP_OWNER_COME && ownerCame) {
        mind.expectOutcome(true); s_sawMotion = false;
      } else if (millis() - mind.expectAtMs() > 600000UL) {
        bool fulfilled = true;
        if (ec == Cognition::EXP_OWNER_COME) fulfilled = ownerCame;
        else if (ec == Cognition::EXP_OWNER_AWAY) fulfilled = !ownerCame;
        else if (ec == Cognition::EXP_WORLD_ACTIVE) fulfilled = s_sawMotion;
        else if (ec == Cognition::EXP_WORLD_QUIET) fulfilled = !s_sawMotion;
        mind.expectOutcome(fulfilled);
        s_sawMotion = false;
      }
    } else s_sawMotion = false;
  }
  // 小凌②③: 身体感受进内核（体温/信号/时长 → 需求衰减调制）
  mind.senseBody(temperatureRead(), WiFi.RSSI(), millis(), dtMin);
  laapTrustSet(mind.trust);   // 小凌⑥: 心跳时同步回写（cfg.save() 时落 NVS）

  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);

  // 需求过阈值 → 主动表达
  float th = cfg.s.threshold / 100.0f;
  if (cfg.s.expressEn && mind.dominance() >= th) {
    arisExpress(false, "tick");
  }

  // 周期性记忆压缩：每 4 小时一次。旧写的 `cycles % 48 ||` 让 OR 恒真落在 24 分钟节拍上
  // （48×30s），每天 ~60 次 LLM 调用抢唯一槽位（聊天被挤成"让我把刚才的想完…"）——
  // v3.51 审计改回注释声明的 4 小时口径
  if (millis() - g_lastConsumeMs > 4UL * 3600UL * 1000UL) {
    consolidateMemory();
  }
  // 性格进化落盘降频：周期数变化时才写（原来每 30s 全量写，每天 2880 次 flash 磨损）
  // 节流收进了 saveEvolution 内部（强制/置脏/满 96 周期才写）——原来的
  // "cycles != 上次保存值"恒真（incCycle 每心跳 +1），降频从未生效
  mind.saveEvolution();
  // 小凌⑥: 信任向中性回归（homeostasis，与需求稳态化同思路）：无衰减的信任会被
  // "每次成功对话 +0.02"棘轮顶满 1.0（v3.39 加读数后实测恒满），"非常亲近"失去
  // 信息量、点踩也看不见波动。0.01/min：1.0→0.65 约 3 小时；点踩的凹陷数小时自愈
  mind.trust += (0.60f - mind.trust) * 0.01f * dtMin;
  // 小凌⑥: 信任值变化超 ±0.05 才落 NVS（原来只在 cfg.save() 时顺带写，重启回滚）
  static float s_lastTrustSaved = -1;
  if (s_lastTrustSaved < 0 || (mind.trust - s_lastTrustSaved > 0.03f) || (s_lastTrustSaved - mind.trust > 0.03f)) {   // 0.05→0.03：单次 👍(+0.04)/👎(-0.03) 也要落盘（原死区让其重启即回滚，v3.51）
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
  // 每轮最多处理 8 行：无换行字节流曾能让 while(readStringUntil) 永久占住主循环（v3.51 审计）
  for (int cliGuard = 0; cliGuard < 8 && Serial.available(); cliGuard++) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    laapActivity();     // 串口打字也是交互
    if (line == "/help") {
      Serial.println("命令: /status /look(视觉识图) /search 词 /weather 城市 /touch(摇晃) /touchpad(触摸) /screen N /redraw /i2cscan /portal /mem /reflect(立刻反思) /rules(看行为规则) /rulesreflect(立刻归纳规则) /skills(看口令技能) /intents(看意图) /intent 词(手动加目标) /tidy(整理记忆) /mono(立刻独白) /nothink 0|1 /beep(回环测声) /tick /lcd /pa /imu /micgain(麦克风增益) /voicetest 文本(应答路径) /asrsend(不录音测请求) /asrloop 话(自听回环) /asrtest(录音识别) /metrics(评估埋点) /snap(快照列表) /snap restore 名 版本 /snaptake(强制快照) /reset");
      Serial.println("      直接打字回车 = 跟它说话（走完整对话链路）");
    } else if (line == "/touch") {
      // 触觉实测：5 秒采样，摇晃/扣翻板子看峰值与判定
      if (!g_imuOk) Serial.println("[触觉] IMU 未初始化（/imu 查 I2C）");
      else {
        Serial.println("[触觉] 5 秒采样中：现在摇晃板子，或把它扣在桌上…");
        float peak = 0; int shakes = 0, downs = 0;
        uint32_t t0 = millis();
        while (millis() - t0 < 5000) {
          float x, y, z; imuReadAccel(x, y, z);
          float mag = sqrtf(x * x + y * y + z * z);
          if (mag > peak) peak = mag;
          if (fabsf(mag - 1.0f) > 0.55f) shakes++;
          if (z < -0.75f) downs++;
          delay(20);
        }
        Serial.printf("[触觉] 峰值 |a|=%.2fg，强晃样本 %d，扣伏样本 %d\n", peak, shakes, downs);
        Serial.printf("[触觉] 判定阈值：|a|-1 超 ±0.55 记一次晃(3次/1.5秒=撒娇)；z<-0.75=扣伏(生气)\n");
        Serial.printf("[触觉] 结果：%s\n",
                      downs > 10 ? "扣伏已识别" : (shakes >= 3 ? "晃动已识别" : "本次没测到动作（幅度太小？）"));
      }
    } else if (line == "/screen") {
      Serial.printf("[LCD] 静默息屏 %u 秒（0=常亮），当前背光=%s 亮度=%u%%\n",
                    cfg.s.screenOffSec, display.screenOn() ? "亮" : "灭", display.getBrightness());
      Serial.println("[LCD] 用法: /screen 60  → 60 秒无交互息屏并保存");
    } else if (line.startsWith("/screen ")) {
      // 严格解析（v3.51 审计）：toInt 对非数字归零 = 静默被悄悄改成"常亮"并落盘
      String arg = line.substring(8); arg.trim();
      bool okNum = arg.length() > 0;
      for (unsigned int i = 0; i < arg.length(); i++) if (arg[i] < '0' || arg[i] > '9') okNum = false;
      if (!okNum) { Serial.println("[LCD] 参数须为数字（用法: /screen 60，0-3600 秒，0=常亮）"); }
      else {
        int v = arg.toInt();
        if (v < 0) v = 0; if (v > 3600) v = 3600;
        cfg.s.screenOffSec = (uint16_t)v;
        cfg.save();
        g_lastActivityMs = millis();
        Serial.printf("[LCD] 静默 %d 秒后息屏（0=常亮），已保存\n", v);
      }
    } else if (line == "/status") {
      Serial.println("[世界模型] " + mind.worldJson());
      Serial.println("[语义记忆] " + memory.semantic());
      {
        float ax, ay, az, acc = -1;
        if (g_imuOk) {
          imuReadAccel(ax, ay, az);
          float m = sqrtf(ax * ax + ay * ay + az * az);
          if (m > 0.05f && m < 8.0f) acc = m;
        }
        Serial.printf("[设备] %s  时间: %s  RSSI: %d dBm  温度: %.1fC  IMU: %s\n",
                      deviceFeelLine().c_str(),
                      time(nullptr) > 1700000000 ? String(time(nullptr)).c_str() : "未同步",
                      WiFi.RSSI(), temperatureRead(),
                      acc > 0 ? (String(acc, 2) + " g").c_str() : "无");
      }
    } else if (line == "/portal") {
      if (webui.inAP()) Serial.println("[LAAP] 已在配置热点模式");
      else webui.beginAP();                 // 重复 beginAP 会重挂路由+重建监听套接字（v3.51 审计）
    } else if (line == "/lcd") {
      Serial.println("[LCD] " + display.lcdDiag());
    } else if (line == "/touchpad") {
      // 触摸屏实测：8 秒采样，点屏幕看有没有触点
      if (!g_touchOk) Serial.println("[触摸] 未检测到触摸芯片（0x38 无应答）——先跑 /i2cscan 确认");
      else {
        Serial.printf("[触摸] 芯片 0x38：模式=0x%02X 阈值=0x%02X\n", touchReg(0x00), touchReg(0x80));
        Serial.println("[触摸] 8 秒采样中：现在用手指点/按屏幕…");
        uint32_t t0 = millis(); int lastx = -1, lasty = -1, hits = 0;
        while (millis() - t0 < 8000) {
          uint8_t raw[5]; touchReadRaw(raw);
          int n = raw[0] & 0x0F;
          if (n) {
            int x = ((raw[1] & 0x0F) << 8) | raw[2], y = ((raw[3] & 0x0F) << 8) | raw[4];
            hits++;
            if (abs(x - lastx) > 3 || abs(y - lasty) > 3 || hits == 1) {
              Serial.printf("  触点 %d: x=%d y=%d（事件=%d）\n", n, x, y, (raw[1] >> 6) & 3);
              lastx = x; lasty = y;
            }
          }
          delay(20);
        }
        Serial.printf("[触摸] 8 秒采到 %d 次触点 → %s\n", hits,
                      hits ? "触摸屏工作正常（以后点屏幕它就会回应）"
                           : "没采到触点（手没点到屏上？还是这块屏不是触摸屏）");
      }
    } else if (line == "/lookdump") {
      // 视觉取证：抓帧→PNG→base64 全部从串口吐出（本机解码验证 PNG 合法性/看画面内容）
      Serial.println("[LOOKDUMP] BEGIN");
      size_t n = 0;
      { String b = vision.debugPngB64(n);   // 复用同一条编码链路
        Serial.printf("[LOOKDUMP] PNG b64 %u 字节\n", (unsigned)n);
        for (size_t i = 0; i < b.length(); i += 180) {
          Serial.println(b.substring(i, i + 180));
          if ((i / 180) % 20 == 19) delay(30);   // 流控：别把串口缓冲冲爆
        } }
      Serial.println("[LOOKDUMP] END");
    } else if (line.startsWith("/look")) {
      // 视觉端到端实测：抓帧 → BMP/base64 → 视觉后端 → 描述（串口全可见）
      String q = line.length() > 5 ? line.substring(5) : String("");
      q.trim();
      Serial.printf("[VISION] 抓帧识图%s\n", q.length() ? ("（问题：" + q + "）").c_str() : "…");
      uint32_t t0 = millis();
      String d = vision.look(q);
      if (d.length()) {
        Serial.printf("[VISION] %s\n[VISION] 用时 %lums\n", d.c_str(), millis() - t0);
        vision.logSight(d);
      } else {
        Serial.printf("[VISION] 失败: %s（%lums）\n", vision.lastError.c_str(), millis() - t0);
      }
      Serial.printf("[VISION] heap %uKB / psram %uKB\n",
                    (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
    } else if (line.startsWith("/search ")) {
      // 搜索链路实测：看设备到底抓回了什么（"答非所问"时先看这里）
      String q = line.substring(8); q.trim();
      Serial.printf("[SEARCH] 查询「%s」…\n", q.c_str());
      uint32_t t0 = millis();
      String k = laapSearch.search(q, 3, 500);
      Serial.printf("[SEARCH] 用时 %lums，拿到 %u 字节%s\n", millis() - t0, (unsigned)k.length(),
                    k.length() ? "" : ("（失败: " + laapSearch.lastError + "）").c_str());
      if (k.length()) Serial.println(k);
    } else if (line.startsWith("/weather")) {
      // 天气快问实测（走本地 wttr.in，不经大模型）
      String city = line.length() > 9 ? line.substring(8) : String("");
      city.trim();
      Serial.printf("[WEA] 城市「%s」…\n", city.length() ? city.c_str() : "(按出口IP定位)");
      Serial.println(laapToolsDispatch("天气 " + city));
    } else if (line == "/i2cscan") {
      // I2C 总线扫描（找外设真身：PCA9557/ES8311/ES7210/QMI8658/摄像头 以及可能的触摸芯片）
      Serial.print("[I2C] 扫描 0x08-0x77:");
      int found = 0;
      for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); found++; }
      }
      Serial.printf("  共 %d 个\n", found);
      Serial.println("[I2C] 已知: 0x19=PCA9557(IO扩展) 0x18=ES8311(喇叭) 0x40=ES7210(麦克风) 0x6A=QMI8658(IMU) 0x21=GC0308(摄像头)");
      Serial.println("[I2C] 触摸常见: 0x38/0x39=FT6236/FT6336  0x15=CST816  0x5D=GT911  0x48/0x49=NS2009");
    } else if (line == "/ipbar") {
      Serial.println("[LCD] " + display.ipBarDiag());
      Serial.println("[LCD] 底栏构成: 左=绿天线(联网)/红感叹号(断网) ｜ 中=7段IP ｜ 右=情绪色点");
      Serial.println("[LCD] 情绪点颜色: 豆绿=开心兴奋 雾蓝=好奇 灰蓝=焦虑 暗灰=疲惫 藕粉(偏红)=孤单 近白=平静");
    } else if (line == "/redraw") {
      display.repaint();   // 重画 表情+顶栏+底栏IP（屏幕状态异常时的复位手势）
      Serial.println("[LCD] 已重画整屏（表情 + 顶栏 + 底栏 IP）");
    } else if (line == "/pa") {
      // 音频物理层诊断: PCA9557 输出寄存器回读（bit1=PA_EN）+ ES8311 音量
      Wire.beginTransmission(0x19); Wire.write(0x01); Wire.endTransmission(false);
      uint8_t rb = (Wire.requestFrom((int)0x19, 1) == 1) ? Wire.read() : 0xFF;
      Serial.printf("[AUD] PCA9557 out=0x%02X (CS=%d PA_EN=%d PWDN=%d) vol=%d%%\n",
                    rb, rb & 1, (rb >> 1) & 1, (rb >> 2) & 1, audio.volume());
      Serial.println("[AUD] 播 1s 测试音…（此时 PA 应为 1）");
      audio.paSet(true);
      static int16_t tone[1600];                 // 440Hz 100ms@16k，播 10 次=1s（static：3.2KB 不上栈，与 /beep 同规格，v3.51）
      for (int i = 0; i < 1600; i++) tone[i] = (int16_t)(8000 * sinf(2 * PI * 440 * i / 16000));
      for (int r = 0; r < 10; r++) audio.playPcm(tone, 1600, 16000);
      Wire.beginTransmission(0x19); Wire.write(0x01); Wire.endTransmission(false);
      rb = (Wire.requestFrom((int)0x19, 1) == 1) ? Wire.read() : 0xFF;
      Serial.printf("[AUD] 播放中 PA_EN=%d（若 0 → paSet 未生效=硬件/扩展芯片问题）\n", (rb >> 1) & 1);
      audio.paSet(false);
    } else if (line == "/beep") {
      // 喇叭→麦克风 回环实测：解码与 I2S 写入都正常却"没声音"时，用它分清
      // "真没出声" vs "出了声但你听不到"（板载喇叭与麦克风同板，声音会漏进麦）
      static int16_t tone[1600];                 // 440Hz 100ms@16k
      for (int i = 0; i < 1600; i++) tone[i] = (int16_t)(12000 * sinf(2 * PI * 440 * i / 16000));
      audio.recordStart(2);
      audio.paSet(true);
      float base = 0, peak = 0;
      for (int r = 0; r < 12; r++) {
        audio.playPcm(tone, 1600, 16000);
        audio.recordTick();
        float rms = audio.micRms();
        if (r == 0) base = rms;
        if (r > 1 && rms > peak) peak = rms;
      }
      audio.recordStop();
      audio.paSet(false);
      Serial.printf("[AUD] 回环：首段RMS=%.0f 播放中峰值RMS=%.0f → %s\n", base, peak,
                    peak > base + 80 ? "喇叭确实在出声（声音进了麦克风）"
                                     : "喇叭没出声（数字链路正常 → 查功放/喇叭/编解码器输出）");
      // ES8311 关键寄存器回读（0x18）：0x31=DAC静音/音量控制 0x32=DAC音量 0x0D/0x0E=DAC电源 0x12=系统
      for (uint8_t r : {(uint8_t)0x31, (uint8_t)0x32, (uint8_t)0x0D, (uint8_t)0x0E, (uint8_t)0x12, (uint8_t)0x37}) {
        Wire.beginTransmission(0x18); Wire.write(r);
        uint8_t v = 0xFF;
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)0x18, 1) == 1) v = Wire.read();
        Serial.printf("[AUD] ES8311 reg 0x%02X = 0x%02X\n", r, v);
      }
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
    } else if (line == "/voicetest") {
      Serial.println("[VOICE] 用法: /voicetest 文本  → 用文本走「按键对讲」的应答段（不起麦克风）");
      Serial.println("[VOICE] 判据：本地直答只应有 1 次 TTS；走大模型的提问应当场不念、稍后念真回复");
    } else if (line.startsWith("/voicetest ")) {
      // 复现"回答两次"这类问题的现场仪器：与按键对讲共用 LaapVoice::respond()
      String t = line.substring(11);
      t.trim();
      Serial.printf("[VOICE] 模拟听到「%s」\n", t.c_str());
      uint32_t tv = millis();
      voice.respond(t);
      Serial.printf("[VOICE] 应答段返回（%lums）——上面一行标了这句由谁念\n", millis() - tv);
    } else if (line == "/asrloop") {
      Serial.println("[ASR] 用法: /asrloop 要它说的话  → 设备自己说、自己录、发 ASR");
    } else if (line.startsWith("/asrloop ")) {
      // 自听回环：TTS 合成 → 自己喇叭播 → 自己麦克风收 → ASR。
      // 不依赖任何外部音源，是"ASR 到底能不能识别人声"的现场判据（也顺手验麦克风灵敏度）。
      String txt = line.substring(8);
      txt.trim();
      static int16_t* capBuf = nullptr;
      const size_t CAP = 16000 * 8;             // 最多 8 秒
      if (!capBuf) {
        capBuf = (int16_t*)heap_caps_malloc(CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
      }
      // 不回落内部堆（v3.51）：256KB 常驻内部堆会让之后所有 TLS"连接失败"，宁可不跑这个诊断
      if (!capBuf) { Serial.println("[ASR] PSRAM 缓冲分配失败（无 PSRAM 或不足），跳过 /asrloop"); continue; }
      Serial.printf("[ASR] 自听回环：让它说「%s」…\n", txt.c_str());
      laapTtsCaptureBegin(capBuf, CAP, 24000);
      bool spoke = edgeTts.speak(txt, String(cfg.s.ttsVoice), String(cfg.s.ttsRate), false);
      size_t n = laapTtsCaptureEnd();
      if (!spoke || n < 1600) {
        Serial.printf("[ASR] TTS 失败(%s) 或样本太少(%u) → 换 /asrsend 测请求\n",
                      edgeTts.lastError.c_str(), (unsigned)n);
        continue;
      }
      Serial.printf("[ASR] 合成 %u 样本（%.1fs@16k），边播边录…\n", (unsigned)n, n / 16000.0);
      if (!audio.recordStart(10)) { Serial.println("[ASR] 录音启动失败"); continue; }
      // 必须开 barge-in：只有它会让 playPcm 内部每 10ms 抽一次麦克风。
      // 否则 100ms 一块的播放期间 I2S RX 溢出，录到的只有约两成（实测 4.6s 语音只录到 0.8s）。
      // 每一块只有 100ms，短于打断判定的 300ms 泄漏基线窗口 → 不会被自己的声音打断。
      audio.bargeInEnable(true);
      for (size_t off = 0; off < n; off += 1600) {      // 100ms 一块：块内按实时节奏阻塞
        size_t m = (n - off) < 1600 ? (n - off) : 1600;
        audio.playPcm(capBuf + off, m, 16000);
        audio.recordTick();                             // 兜底再抽一次
      }
      for (uint32_t t = millis(); millis() - t < 500;) { audio.recordTick(); delay(5); }
      audio.bargeInEnable(false);
      size_t got = audio.recordBytes();
      audio.recordStop();
      { float rms = 0; const int16_t* p = audio.recordData(); int cnt = got / 2;
        for (int i = 0; i < cnt; i += 16) rms += (float)p[i] * p[i];
        rms = sqrtf(rms / (cnt / 16 + 1));
        Serial.printf("[ASR] 录到 %u 字节（%.1fs），RMS=%.0f（麦克风增益 %.1f dB）\n",
                      (unsigned)got, got / 32000.0, rms, audio.micGainDb() / 10.0); }
      String el2;
      uint32_t ta2 = millis();
      String heard = asr.transcribe(audio.recordData(), got, el2);
      if (heard.length())
        Serial.printf("[ASR] ✓ 回环识别成功（%lums）→「%s」\n", millis() - ta2, heard.c_str());
      else
        Serial.printf("[ASR] ✗ 未识别（%lums）: %s\n", millis() - ta2, el2.c_str());
    } else if (line == "/micgain") {
      Serial.printf("[AUD] 麦克风增益 %.1f dB（用法: /micgain 0-37.5，档位 3dB 一档）\n",
                    audio.micGainDb() / 10.0);
      Serial.println("[AUD] 判据：/beep 看近场回环 RMS、说话时 /asrtest 看 RMS（<50 无声，>300 正常）");
    } else if (line.startsWith("/micgain ")) {
      String arg = line.substring(9); arg.trim();
      bool okNum = arg.length() > 0; int dots = 0;
      for (unsigned int i = 0; i < arg.length(); i++) {
        if (arg[i] == '.') { if (++dots > 1) okNum = false; }
        else if (arg[i] < '0' || arg[i] > '9') okNum = false;
      }
      if (!okNum) { Serial.println("[AUD] 参数须为数字（用法: /micgain 0-37.5，档位 3dB 一档）"); }
      else {
      int db10 = (int)(arg.toFloat() * 10 + 0.5);
      if (db10 < 0) db10 = 0; if (db10 > 375) db10 = 375;
      bool okg = audio.setMicGainDb(db10);
      Serial.printf("[AUD] 麦克风增益 → %.1f dB%s\n", audio.micGainDb() / 10.0,
                    okg ? "" : "（寄存器写入失败）");
      Serial.println("[AUD] 接着打 /asrtest 或 /asrloop 看效果（增益过高会削顶，RMS 长期贴近 3 万就是不合适）");
      }
    } else if (line == "/asrsend") {
      // ASR 请求自检（不录音、不用出声）：合成 1s 音 → 打印 WAV 头关键字段 → 发给配置的 ASR。
      // 用来把"请求格式/服务端"与"麦克风录音内容"彻底分开：这里通了才轮到查录音。
      static int16_t* t1 = nullptr;
      if (!t1) { t1 = (int16_t*)heap_caps_malloc(16000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                 if (!t1) t1 = (int16_t*)malloc(16000 * sizeof(int16_t)); }
      if (!t1) { Serial.println("[ASR] 缓冲分配失败"); continue; }
      for (int i = 0; i < 16000; i++) t1[i] = (int16_t)(9000 * sinf(2 * PI * 440 * i / 16000.0));
      size_t cap = 32000 + 64;
      uint8_t* wv = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
      if (!wv) wv = (uint8_t*)malloc(cap);
      if (!wv) { Serial.println("[ASR] 缓冲分配失败"); continue; }
      size_t wl = wavWrap(t1, 32000, wv, cap);
      auto rd32 = [&](int o) { return (uint32_t)wv[o] | ((uint32_t)wv[o+1] << 8) |
                                      ((uint32_t)wv[o+2] << 16) | ((uint32_t)wv[o+3] << 24); };
      auto rd16 = [&](int o) { return (uint16_t)(wv[o] | (wv[o+1] << 8)); };
      Serial.printf("[ASR] WAV %u 字节 | 标记 %.4s/%.4s/%.4s | fmt块长=%u(应16) PCM=%u 声道=%u "
                    "采样率=%u 字节率=%u(应采样率*声道*2) 位深=%u | data块长=%u(应 %u)\n",
                    (unsigned)wl, (const char*)wv, (const char*)wv + 8, (const char*)wv + 12,
                    (unsigned)rd32(16), (unsigned)rd16(20), (unsigned)rd16(22),
                    (unsigned)rd32(24), (unsigned)rd32(28), (unsigned)rd16(34),
                    (unsigned)rd32(40), (unsigned)(wl - 44));
      free(wv);
      Serial.printf("[ASR] 后端 %s | 模型 %s | 发送中…\n", cfg.s.asrBase, cfg.s.asrModel);
      String e1;
      uint32_t ta = millis();
      String r1 = asr.transcribe(t1, 32000, e1);
      if (r1.length())
        Serial.printf("[ASR] ✓ 服务端接受请求，%lums，识别文本「%s」\n", millis() - ta, r1.c_str());
      else if (e1.indexOf("响应无") >= 0)
        Serial.printf("[ASR] ✓ 服务端接受请求（合成音无语音，故无文本），%lums\n", millis() - ta);
      else
        Serial.printf("[ASR] ✗ %s（%lums）→ 请求格式或服务端问题，与麦克风无关\n",
                      e1.c_str(), millis() - ta);
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
        // 0.5s@16k 合成音：放 PSRAM（16KB 常驻内部堆会切碎堆，TLS 握手要一整块）
        static int16_t* tone = nullptr;
        if (!tone) {
          tone = (int16_t*)heap_caps_malloc(8000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
          if (!tone) tone = (int16_t*)malloc(8000 * sizeof(int16_t));
        }
        if (!tone) { Serial.println("[ASR] 缓冲分配失败"); continue; }
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
    } else if (line == "/mono") {
      // 手动触发一轮独白（忽略"静默满 N 分"与"深夜不冒泡"这两道门槛，方便随时验证）
      Serial.println("[LAAP·独白] 手动触发一轮（忽略静默/深夜门槛）");
      if (arisIdleMonologue()) Serial.println("[LAAP·独白] 已提交，等后台出结果（约 10~30 秒）");
    } else if (line.startsWith("/nothink")) {
      String a = line.substring(8); a.trim();
      bool digits = a.length() > 0;
      for (unsigned int ni = 0; ni < a.length() && digits; ni++) if (!isDigit(a[ni])) digits = false;
      if (!digits) { Serial.println("[LLM] 用法：/nothink 0|1（非数字不写配置）"); }
      else {
        cfg.s.llmNoThink = (a.toInt() != 0) ? 1 : 0;
        cfg.save();
        Serial.printf("[LLM] 关闭思考 = %s（请求里%s带 thinking:disabled）\n",
                      cfg.s.llmNoThink ? "开" : "关", cfg.s.llmNoThink ? "" : "不");
      }
    } else if (line == "/reflect") {
      nightlyReflect(true);          // 立刻做一次夜间反思（不等 0-5 点窗口）
    } else if (line == "/metrics") {
      // 评估埋点（本次开机累计）：自我进化的 fitness 端
      Serial.printf("[METRICS] 对话轮 %lu（没听清 %lu / ASR失败 %lu / 唤醒词拒 %lu / 冷却丢弃 %lu）\n",
                    (unsigned long)metrics.vadTriggers, (unsigned long)metrics.noSpeech,
                    (unsigned long)metrics.asrFail, (unsigned long)metrics.wakeMiss,
                    (unsigned long)metrics.cooldownSkip);
      Serial.printf("[METRICS] ASR 尝试 %lu（空 %lu，空识别率 %lu%%）｜打断 %lu｜直答 %lu（%lu ms均值）｜视觉 ✓%lu ✗%lu｜搜索 ✓%lu ✗%lu\n",
                    (unsigned long)metrics.asrTry, (unsigned long)metrics.asrFail,
                    (unsigned long)(metrics.asrTry ? metrics.asrFail * 100 / metrics.asrTry : 0),
                    (unsigned long)metrics.interrupts, (unsigned long)metrics.toolDirect,
                    (unsigned long)metrics.rspDirMs(), (unsigned long)metrics.visionOk,
                    (unsigned long)metrics.visionFail, (unsigned long)metrics.searchOk,
                    (unsigned long)metrics.searchFail);
      Serial.printf("[METRICS] LLM ✓%lu ✗%lu｜端到端均值 %lu ms｜反馈 👍%lu 👎%lu\n",
                    (unsigned long)metrics.llmOk, (unsigned long)metrics.llmFail,
                    (unsigned long)metrics.rspLlmMs(), (unsigned long)metrics.fbUp,
                    (unsigned long)metrics.fbDown);
    } else if (line == "/tune") {
      // 自调优状态（RSI⑥）：调整历史看 [TUNE] 日志；评估窗 5 分钟一次
      Serial.printf("[TUNE] 播报冷却 %lu ms（1200~3000）｜VAD 阈值 ×%.2f（1.00~2.00）｜每次开机回默认\n",
                    (unsigned long)voice.cooldownDur(), voice.vadMul());
    } else if (line == "/snap") {
      Serial.print(laapSnapListText());
    } else if (line.startsWith("/snap restore ")) {
      // /snap restore semantic 2  → 把 semantic 的第 2 版拷回原位（现状先存 .pre），然后重启生效
      String rest = line.substring(14); rest.trim();
      int sp = rest.indexOf(' ');
      if (sp > 0) {
        String name = rest.substring(0, sp), vStr = rest.substring(sp + 1); vStr.trim();
        if (laapSnapRestore(name.c_str(), vStr.toInt())) {
          Serial.println("[SNAP] 已恢复，3 秒后重启生效…");
          delay(3000); laapReboot("快照恢复");
        } else Serial.println("[SNAP] 恢复失败（没有这个文件/版本；先 /snap 看列表）");
      } else Serial.println("用法: /snap restore <semantic|episodes|evolution> <1-3>");
    } else if (line == "/snaptake") {
      laapSnapAll(true);   // 手动强制拍一份（改配置/折腾前留个还原点）
    } else if (line == "/rules") {
      String t = rules.text();
      Serial.println(t.length() ? t : String("[RULES] 还没有行为规则（/rulesreflect 现在归纳一轮，或等夜里自动跑）"));
    } else if (line == "/rules clear") {
      rules.clear();
      Serial.println("[RULES] 规则已清空");
    } else if (line == "/relations") {
      String rt = memory.relationsText();
      Serial.println(rt.length() ? rt : String("[REL] 还没有关系记忆（/relationsreflect 现在抽一轮，或等夜里自动跑）"));
    } else if (line == "/relationsreflect") {
      relationsReflect(true);   // 立刻从最近经历抽一轮关系事实（不等深夜窗口）
      Serial.println("[REL] 已提交抽取，LLM 出结果后落盘（约 10~30 秒）");
    } else if (line == "/moodrelabel") {
      moodRelabel(true);        // 立刻精标注一轮最近记忆的情绪（不等深夜窗口）
      Serial.println("[MOOD] 已提交标注，LLM 出结果后落盘（约 10~30 秒）");
    } else if (line == "/dream") {
      dreamReflect(true);       // 立刻做一场梦（不等深夜窗口）
      Serial.println("[DREAM] 已提交，LLM 出结果后以【梦】写入记忆（约 10~30 秒）");
    } else if (line == "/cons") {
      Serial.println("[CONS] 意识指标自检（Butlin/Long 框架工程自评：满/半/缺）:");
      int score = 0;
      for (auto& it : kConsTable) {
        score += it.s;
        Serial.printf("  [%s] %s — %s\n", it.s == 2 ? "满" : (it.s == 1 ? "半" : "缺"), it.g, it.note);
      }
      Serial.printf("[CONS] 合计 %d/%d（/api/consc 可取 JSON）\n",
                    score, (int)(sizeof(kConsTable) / sizeof(kConsTable[0])) * 2);
    } else if (line == "/r0") {
      Serial.printf("[R0] 微型循环处理器：步数 %lu｜本拍误差 %.2f｜滚动 %.2f（对下一拍动静/信号的预测偏差=惊讶度，回注好奇）\n",
                    (unsigned long)r0.steps(), r0.lastErr(), r0.rollingErr());
    } else if (line == "/rulesreflect") {
      rulesReflect(true);   // 立刻用当前反馈+失败素材归纳一轮规则（不等深夜窗口）
      Serial.println("[RULES] 已提交归纳，LLM 出结果后生效（约 10~30 秒）");
    } else if (line == "/skills") {
      String t = skills.text();
      Serial.println(t.length() ? t : String("[SKILLS] 还没教过技能。对它说：以后每当我说<词>你就<做什么>"));
    } else if (line == "/skills clear") {
      skills.clear();
      Serial.println("[SKILLS] 技能已清空");
    } else if (line == "/intents") {
      String it = mind.intentsLine();
      Serial.println(it.length() ? it : String("[INTENTS] 心里没有惦记的事（好奇 >0.75 且空闲时会自己升起目标）"));
    } else if (line.startsWith("/intent ")) {
      // 手动塞一个目标（测试意图驱动独白用）：/intent 观察主人喝水的规律
      if (mind.addIntent(line.substring(8), time(nullptr))) Serial.println("[INTENTS] 已加入");
      else Serial.println("[INTENTS] 加入失败（太短/重复/已满）");
    } else if (line == "/tidy") {
      memoryTidy(true);
      Serial.println("[TIDY] 已提交整理，LLM 出清单后自动执行（约 10~30 秒）");
    } else if (line == "/vol" || line.startsWith("/vol ")) {
      // 严格解析（v3.51 审计）：旧代码 startsWith("/vol") 会把 "/volume 80" 也吃进来，
      // toInt("ume 80")=0 落在合法区间 → 打错字就把音量写成 0 并落盘（重启不恢复）。
      // 非纯数字一律当查询，绝不写配置
      String arg = (line.length() > 4) ? line.substring(4) : String(); arg.trim();
      bool okNum = arg.length() > 0;
      for (unsigned int i = 0; i < arg.length(); i++) if (arg[i] < '0' || arg[i] > '9') okNum = false;
      int v = okNum ? arg.toInt() : -1;
      if (okNum && v >= 0 && v <= 100) {
        audio.setVolume((uint8_t)v); cfg.s.volume = (uint8_t)v; cfg.save();
        Serial.printf("[LAAP] 音量 %d%%\n", v);
      } else {
        Serial.printf("[LAAP] 当前音量 %d%%（用法: /vol 0-100）\n", audio.volume());
      }
    } else if (line == "/bright" || line.startsWith("/bright ")) {
      String arg = (line.length() > 7) ? line.substring(7) : String(); arg.trim();
      bool okNum = arg.length() > 0;
      for (unsigned int i = 0; i < arg.length(); i++) if (arg[i] < '0' || arg[i] > '9') okNum = false;
      int v = okNum ? arg.toInt() : -1;
      if (okNum && v >= 5 && v <= 100) {
        display.setBrightness((uint8_t)v); cfg.s.brightness = (uint8_t)v; cfg.save();
        Serial.printf("[LAAP] 亮度 %d%%\n", v);
      } else {
        Serial.printf("[LAAP] 当前亮度 %d%%（用法: /bright 5-100，下限5防全黑）\n", display.getBrightness());
      }
    } else if (line == "/reset") {
      cfg.reset(); laapReboot("恢复出厂");
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
  // 构建时间戳：判断"板子里跑的到底是哪一版"的唯一可靠依据（烧录后必看这一行）
  Serial.printf("[LAAP] 固件构建 %s %s\n", __DATE__, __TIME__);
  Serial.printf("[LAAP] 启动原因: %s\n", laapBootReason().c_str());   // 崩溃/打盹/OTA 一眼可辨（v3.41）
  {   // 运行槽位：OTA 后靠这行确认"新固件真的生效了"（还是老固件在跑）
    const esp_partition_t* rp = esp_ota_get_running_partition();
    Serial.printf("[LAAP] 运行分区 %s (0x%06x) | OTA 可升级: %s\n",
                  rp ? rp->label : "?", rp ? (unsigned)rp->address : 0,
                  esp_ota_get_next_update_partition(nullptr) ? "是" : "否（分区表没有 ota_1）");
  }
  // 堆与栈底数：视觉/语音都是"内存敏感"功能，排障时第一眼要看这个
  Serial.printf("[LAAP] 内部堆 %u KB / PSRAM %u KB / 环任务栈 %u 字节\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)uxTaskGetStackHighWaterMark(NULL));   // 已是字节：旧代码 ×sizeof(StackType_t) 虚高 4 倍（v3.51）

  laapTlsHeapInit();   // 必须先于一切 TLS：mbedtls 大块分配路由 PSRAM（v3.55，一条 TLS 连接
                       // 的内部堆峰值 ~42KB→~8KB，直接缓解自愈性打盹的碎片来源）

  if (!memory.begin())
    Serial.println("[LAAP] !! LittleFS 挂载失败：记忆/规则/技能/快照本次不可用（/reset 或检查分区）");
  cfg.begin();
  cfg.load();
  g_uptimeBaseMin = cfg.uptimeBase();   // 累计运行时长（跨重启累计，单位分钟）

  display.begin();  // Wire(I2C) 在 display.begin 里初始化——必须先于 vision
  display.drawBootScreen();
  vision.begin();   // GC0308（PWDN 经 PCA9557 bit2 已上电；SCCB 复用主 I2C，须在 Wire 初始化后）

  mind.begin();
  g_imuOk = imuInit();
  if (!g_imuOk) Serial.println("[LAAP] IMU 未找到（不影响运行）");
  g_touchOk = touchInit();     // 触摸芯片复用 display.begin() 初始化的 Wire
  if (!g_touchOk) Serial.println("[LAAP] 触摸屏未响应（0x38 无应答，不影响运行）");

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
  laapNetInit();   // 显式建网络互斥锁（懒创建 check-then-create 在两任务同进时有竞态）
  metrics.loadPrev();  // 读回上一段会话的指标快照（崩溃/自愈重启的事故现场不丢）
  r0.begin();          // R0 微型循环处理器：储备池固定重建
  r0.loadNvs();        // 恢复读出层学习进度（打盹/断电不清零"身体直觉"，v3.50）
  {   // C5 预期置信度 EMA 恢复（10B，laapmtr/expema；判定次数打满才可信赖）
    Preferences pe;
    if (pe.begin("laapmtr", true)) {
      uint8_t blob[10];
      if (pe.getBytes("expema", blob, sizeof(blob)) == sizeof(blob)) mind.expectEmaLoad(blob);
      pe.end();
    }
  }
  // 音量 0：以前"部分保存会把音量写成 0"（哨兵 bug，已修），所以开机要钳回 30；
  // 现在 0 只可能来自明确设置（网页/CLI），再改写就等于吞掉用户的静音选择——
  // 改为只提示一句，并告诉怎么恢复（"没声音"最常见的原因就是这里被写成 0）
  if (cfg.s.volume == 0) Serial.println("[LAAP] 注意：音量被设为 0（静音）。/vol 80 或后台设置可恢复");
  audio.setVolume(cfg.s.volume);  // 应用持久化音量（ES8311）
  audio.setVadStopMs(cfg.s.vadStopMs);  // 应用持久化的说完判停窗口
  display.setBrightness(cfg.s.brightness);  // 应用持久化亮度（背光 PWM）
  mind.trust = laapTrust();        // 小凌⑥: 启动时取回持久化信任值
  laapSearch.begin();  // 联网搜索（主源可配，WiFi 就绪后可用）

  display.drawFace(mind.moodKey());
  display.drawNeeds(mind.needs().energy, mind.needs().curiosity, mind.needs().social,
                    mind.needs().security, mind.needs().expression);

  pinMode(BTN_PIN, INPUT_PULLUP);
  g_lastTickMs = millis();
  g_lastConsumeMs = millis();
  g_lastActivityMs = millis();     // 开机先亮 cfg.s.screenOffSec 秒，之后静默才息屏

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

// 身体层即时反馈（摸/摇/翻）：表情 + 一句本地短语 + 记事件，全部本地完成。
// 原来这些事件走 arisExpress→后台 LLM：LLM 忙时 llmSubmit 失败=毫无反应；且 tickle/makeup
// 不在 drawFace 的表情表里（会渲染成 calm），用户看到的就是"摸了摇了都没反馈"。
void laapLocalReact(const char* kind) {
  const char* face = "curious";
  String line = "嗯？";
  if (!strcmp(kind, "tickle"))        { face = "excited"; line = "哎呀，别摇啦～"; }
  else if (!strcmp(kind, "facedown")) { face = "anxious"; line = "闷……看不见你了。"; }
  else if (!strcmp(kind, "makeup"))   { face = "happy";   line = "哦，亮了，好受点了。"; }
  else if (!strcmp(kind, "touch"))    { face = "curious"; line = "嗯？你戳我。"; }
  Serial.printf("[触觉] 反馈[%s]: %s\n", kind, line.c_str());
  laapResetIdleClock();      // 被摸/被摇=真人在场：独白让位（原来只有 g_lastSay 变化算数，
                             // 而这类本地反应也改 g_lastSay，独白时钟会被自己的反应重置）
  memory.logEvent("event", line);
  g_lastSay = line;
  g_lastExpr = face;
  display.drawFace(face);
  voice.speak(line, face);
}

void touchGestures() {
  if (!g_imuOk) return;
  float x, y, z;
  imuReadAccel(x, y, z);
  if (z < -8) return;                     // IMU 无效

  // ---- 摇晃检测：1.5s 窗口累计 3 次强晃 = 事件（阈值 0.55→0.35：原来轻摇完全测不到） ----
  float mag = sqrtf(x * x + y * y + z * z);
  bool strongShake = fabsf(mag - 1.0f) > 0.35f;
  if (strongShake) {
    if (g_shakeWindowMs == 0) g_shakeWindowMs = millis();
    g_shakeCount++;
    if (g_shakeCount >= 3 && millis() - g_shakeWindowMs < 1500) {
      g_shakeCount = 0; g_shakeWindowMs = 0;
      if (!g_facedown) {
        Serial.println("[触觉] 摇晃 → 撒娇反应");
        laapActivity();
        mind.onUserInteraction();
        laapLocalReact("tickle");         // 身体即时反馈（不等 LLM：LLM 忙/挂时也一定有反应）
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
    mind.onError();                        // 安全需求受挫
    laapLocalReact("facedown");            // 有声音反馈（画面随即被装死黑屏盖掉）
  } else if (!down && g_facedown) {
    g_facedown = false;
    laapActivity();                        // 被翻回来=有人在动它
    if (millis() - g_facedownMs > 2000) {  // 扣了 2 秒以上才算真生气过
      Serial.println("[触觉] 翻回来了 → 和好");
      laapLocalReact("makeup");
    }
  }
  // 扣伏期间闭眼装死：只在状态切换时画一帧（原来每帧 clear = 10ms 一次全屏 SPI 空转）
  static bool s_deadPainted = false;
  if (g_facedown) {
    if (!s_deadPainted) { display.clear(0); s_deadPainted = true; }
  } else if (s_deadPainted) {
    s_deadPainted = false;
    display.repaint();
  }
}

void loop() {
  webui.handleClient();
  serialCli();
  memory.embedTick();   // 语义向量懒补（15s 限速，断网自动退关键词）
  display.blinkTick();
  { // 顶栏（时间/需求数字/设备信息/IMU）每秒刷新
    static uint32_t s_lastStatus = 0;
    if (millis() - s_lastStatus > 1000) {
      s_lastStatus = millis();
      float ax, ay, az, acc = -1;
      if (g_imuOk) {
        imuReadAccel(ax, ay, az);
        float m = sqrtf(ax * ax + ay * ay + az * az);
        if (m > 0.05f && m < 8.0f) acc = m;     // 无效读数(默认 z=-9)不显示
      }
      display.drawStatusLine(temperatureRead(),
                             (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0,
                             ESP.getFreeHeap() / 1024,
                             laapUptimeMin(),       // 累计运行（跨重启，不再每次开机归零）
                             acc);
    }
  }
  voice.loopTick();   // VAD 自动聆听模式
  audio.paTick();     // 功放空闲关断（流式播放间隔中保持开启）
  llmHarvest();       // F4: 收割后台 LLM 结果
  touchGestures();    // F5: 摇晃/翻面触觉（每帧，内部自带节流）

  // F5b: 电容触摸屏——点一下=戳它。按下沿触发；息屏时第一次触摸只唤醒（防误触乱说话）
  { static bool s_wasTouch = false; static uint32_t s_lastTouchReact = 0;
    int tx = 0, ty = 0;
    bool nowTouch = g_touchOk && touchRead(tx, ty);
    if (nowTouch && !s_wasTouch) {
      bool wasAsleep = !display.screenOn();
      laapActivity();
      if (!wasAsleep && millis() - s_lastTouchReact > 1500) {
        s_lastTouchReact = millis();
        Serial.printf("[触觉] 触摸屏 (%d,%d)\n", tx, ty);
        mind.onUserInteraction();
        laapLocalReact("touch");
      } else if (wasAsleep) {
        Serial.println("[触觉] 触摸唤醒屏幕");
      }
    }
    s_wasTouch = nowTouch; }

  // 有人对着麦克风说话 = 活动。须持续 ≥600ms（与触发对话的判定一致）——
  // 瞬时 VAD 沿会被环境噪声频繁触发，v3.32 提灵敏度后曾把静默息屏永远顶住
  { static bool s_wasSpeech = false;
    static uint32_t s_spFrom = 0;
    static bool s_counted = false;
    bool sp = audio.vadSpeaking();
    if (sp && !s_wasSpeech) { s_spFrom = millis(); s_counted = false; }
    if (sp && !s_counted && millis() - s_spFrom >= 600) { laapActivity(); s_counted = true; }
    s_wasSpeech = sp; }

  // 静默息屏：screenOffSec 秒无活动关背光；任何交互（说话/按键/摇晃/网页对话）立即点亮
  if (cfg.s.screenOffSec > 0 && display.screenOn() &&
      millis() - g_lastActivityMs > (uint32_t)cfg.s.screenOffSec * 1000UL) {
    display.setScreenOn(false);
    Serial.println("[LAAP] 静默息屏（交互即唤醒）");
  }

  // 自愈性打盹：内部堆最大连续块跌破 TLS 底线（<31KB 时 LLM 必"连接失败"，且碎片
  // 不可自行恢复）且闲置 ≥3 分钟、无任务在飞 → 重启换一块干净的堆。
  // 重启前落盘累计时长；开机后一切照旧。把"死到主人手动重启"变成"打个盹自己缓过来"
  static uint32_t s_tightSince = 0;
  { uint32_t maxblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (maxblk < 26000) {
      if (!s_tightSince) s_tightSince = millis();
      if (millis() - s_tightSince > 30000UL && millis() - g_lastActivityMs > 180000UL &&
          !laapLlmBusy() && !laapChatPending()) {
        Serial.println("[LAAP] 堆碎片到警戒线且闲置 → 自愈性打盹（重启换干净堆）");
        metrics.persist();     // 打盹前的失败证据必须留底
        mind.saveEvolution(true);   // 需求/情绪也留底：醒来接着睡前的状态，不"睡一觉归零"（v3.42）
        r0.saveNvs();          // 循环处理器学习进度留底（v3.50）
        laapUptimePersist();
        delay(600);
        laapReboot("自愈打盹");
      }
    } else s_tightSince = 0;
  }

  // 累计运行时长：每 5 分钟落盘一次（单键写入，NVS 磨损可忽略）
  { static uint32_t s_lastUpSave = 0;
    if (millis() - s_lastUpSave > 300000UL) { s_lastUpSave = millis(); laapUptimePersist(); metrics.persist(); r0.saveNvs();
      Preferences pe;   // C5：预期 EMA 同拍批量落盘（零额外磨损节拍）
      if (pe.begin("laapmtr", false)) {
        uint8_t blob[10];
        mind.expectEmaBlob(blob);
        pe.putBytes("expema", blob, sizeof(blob));
        pe.end();
      }
    } }

  // BOOT 键: 短按=主动表达 长按4s=配置热点 长按10s=格式化
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  if (pressed && g_btnDown == 0) g_btnDown = millis();
  if (!pressed && g_btnDown > 0) {
    uint32_t held = millis() - g_btnDown;
    g_btnDown = 0;
    if (held > 10000) { Serial.println("[LAAP] 恢复出厂"); display.clear(0); cfg.reset(); laapReboot("恢复出厂"); }
    else if (held > 4000) { if (!webui.inAP()) webui.beginAP(); }
    else if (held > 60) {
      laapActivity();                 // 按键=活动（息屏先点亮）
      mind.onButtonPress();
      laapResetIdleClock();           // 主人按键=在场的交互
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

  // WiFi 断线重连 + 底栏 IP 行跟随联网状态（只有真的断网才显示红感叹号，恢复后立刻变回绿天线+IP）
  static uint32_t lastReconnect = 0;
  static int8_t s_ipBarWifi = -1;
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if ((int8_t)wifiUp != s_ipBarWifi) {
    s_ipBarWifi = (int8_t)wifiUp;
    display.drawIpLine(wifiUp ? WiFi.localIP().toString() : String(""), wifiUp);
  }
  if (!wifiUp && !webui.inAP() && millis() - lastReconnect > 30000) {
    lastReconnect = millis();
    WiFi.reconnect();
    mind.onError();
  }

  // PSI 心跳
  if (millis() - g_lastTickMs > cfg.s.tickSec * 1000UL) psiTick();

  // 自发独白：起始静默 idleSilenceMin 分，之后每 idleEveryMin 分一轮（0=关，后台可配）
  bool userTalking = voice.ready() &&
    ((VoiceMode)cfg.s.voiceMode == VoiceMode::Vad) && !voice.vadPaused();
  bool lateNight = laapInQuietWindow();           // 自发说话静音窗（后台可配，默认 23-6；v3.45 起与主动表达同门）
  uint32_t idleGap = (g_idledOnce ? cfg.s.idleEveryMin : cfg.s.idleSilenceMin) * 60000UL;
  if (cfg.s.idleEveryMin > 0 && laapSearch.available() && !userTalking && !lateNight) {
    // 独白也由需求驱动（active inference）：到点只是"可以想"，心里真有驱动才"去想"——
    // 好奇（想探索）或表达（有话想说）过阈值，或心里惦记着目标。需求低就安静待着；
    // 需求会随时间回补，过了阈值自然开讲。门槛判断放在计时器之前：不重置计时，
    // 需求一过线马上补上（不会"永远等不满静默"）
    float th = cfg.s.threshold / 100.0f;
    bool needDriven = mind.intentCount() > 0 ||
                      mind.needs().curiosity >= th || mind.needs().expression >= th;
    if (needDriven && millis() - g_lastIdleMs > idleGap) {
      g_lastIdleMs = millis();
      if (arisIdleMonologue()) g_idledOnce = true;   // 只有真的提交出去了才算"独白过一轮"
    }
  }
  // 独白计时只在"主人来找它"时重置（见 laapResetIdleClock 的调用点）。
  // 旧实现盯着 g_lastSay 变化就重置——它自己说一句话也会重置，等于永远回到"等满静默"，
  // 后台配的 idleEveryMin 形同虚设（实测周期永远是 idleSilenceMin）。

  delay(10);
}

// 主人有交互 → 重新开始"等静默"（独白让位）；它自己说话不重置
void laapResetIdleClock() {
  g_lastIdleMs = millis();
  g_idledOnce = false;
}

// 独白计时诊断（/api/status 用）：一眼看出"还差多久冒泡 / 处于哪种节奏"
String laapIdleInfo() {
  uint32_t mins = g_lastIdleMs ? (millis() - g_lastIdleMs) / 60000UL : 0;
  uint32_t gap = g_idledOnce ? cfg.s.idleEveryMin : cfg.s.idleSilenceMin;
  return String("silent=") + mins + "min/" + gap + "min "
       + (g_idledOnce ? "steady" : "first") + (g_llmFailStreak >= 2 ? " llmFail" : "");
}

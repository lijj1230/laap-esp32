#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>

// ============================================================
// LAAP Web 后台
//   AP 模式: 热点 "Aris-XXXX"，192.168.4.1 配置门户（首次/长按BOOT进入）
//   STA 模式: http://aris.local 或 http://<IP>/ 仪表盘 + 后台配置大模型 API
// ============================================================
class LaapWeb {
public:
  void beginSTA();
  void beginAP();
  void handleClient();
  bool inAP() const { return _ap; }
  String apSsid() const { return _apSsid; }

private:
  friend class LaapOtaHelper;   // F1 OTA 写入回调访问成员
  bool otaPending = false;
  String otaErr;
  void registerRoutes();
  void handleRoot();
  void handleSettingsPage();
  void handleSave();
  void handleStatus();
  void handleChat();
  void handleChatReply();   // 异步 LLM 的成品轮询（网页聊天不再只看到"……"）
  void handleTest();
  void handleMemoryPage();
  void handleMemoryApi();
  void handleMemExport();    // 记忆导出（纯文本下载：备份/换机/分区迁移）
  void handleMemImport();    // 记忆导入（分段文本上传，逐行解析，不进大缓冲）
  bool memImportOk = false;
  void handleClear();
  void handleReset();
  void handleReboot();
  void handleVoiceTest();
  void handleSpeak();
  void handleListenToggle();
  void handleMetrics();       // 评估埋点（RSI 闭环的评估端）
  void handleFeedback();      // 网页 👍/👎 反馈
  void handleSnapshots();     // "自我"文件快照列表
  void handleSnapRestore();   // 快照恢复（恢复后重启）
  void handleRulesApi();      // 行为规则文本（记忆页展示）
  void handleSkillsApi();     // 口令技能列表（记忆页展示）
  void handleRulesReflect();  // 立刻归纳一轮规则
  void handleRelationsApi();     // 关系记忆文本（偏好/承诺/边界）
  void handleRelationsReflect(); // 立刻抽一轮关系事实
  void handleMoodRelabel();      // 立刻精标注一轮记忆情绪
  void handleNotFound();

  WebServer server{80};
  DNSServer dns;
  bool _ap = false;
  String _apSsid;
};

extern LaapWeb webui;

// 由 laap-esp32.ino 提供
String laapLastSay();
String laapInteractSearch(const String& userText);
const char* laapLastExpr();
const char* laapLastReqShape();   // 最近一次 LLM 请求的消息结构（诊断"答非所问"用）
// 聊天回复是异步产生的（主循环不冻结）：网页拿到受理回执后轮询这两个
uint32_t laapChatSeq();       // 每次有新的聊天成品回复 +1
String laapChatReply();       // 最近一条聊天成品回复
bool laapChatPending();       // 上一次 laapInteractSearch 是否丢给了后台 LLM
bool laapReplySpoken();       // 上一次是否已自己念过（本地直答：工具指令 / 看东西）
String laapLastUserText();    // 最近一轮主人原话（反馈落盘用）
String laapIdleInfo();        // 独白计时诊断（"还差多久冒泡"）
void rulesReflect(bool force);// 立刻归纳一轮行为规则（记忆页"立刻归纳"按钮用）
void relationsReflect(bool force); // 立刻从最近经历抽一轮关系事实（偏好/承诺/边界）
void moodRelabel(bool force);      // 立刻精标注一轮最近记忆的情绪标签
bool laapLlmBusy();           // 后台 LLM 任务在飞（含独白/反思/整理的多步流水线）

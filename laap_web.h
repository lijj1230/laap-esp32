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
  void handleTest();
  void handleMemoryPage();
  void handleMemoryApi();
  void handleClear();
  void handleReset();
  void handleReboot();
  void handleVoiceTest();
  void handleSpeak();
  void handleListenToggle();
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

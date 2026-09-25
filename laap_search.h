#pragma once
#include <Arduino.h>

// ============================================================
//  联网搜索（LAAP 世界模型的"外部感知"输入）
//  v2.3 主源可配：NVS searchApi 存 URL 模板（{q}=查询词），
//  默认空=必应 RSS（3-4KB 轻量，大陆可达）；响应自动识别
//  RSS/XML 或必应 HTML；DDG→必应HTML 降为双兜底
// ============================================================

struct SearchHit {
  String text;   // 一条摘要
};

class LaapSearch {
public:
  void begin();                       // 记录联网能力
  bool available() const { return _ok; }
  // 搜索并拼接为一段知识文本（最多 maxHit 条，总长上限 maxLen）。
  // 内部带跨任务互斥：后台独白也会调它；拿不到锁就返回 ""（lastError=搜索正忙）
  String search(const String& query, int maxHit = 3, int maxLen = 600);
  String lastError;
private:
  String searchLocked(const String& query, int maxHit, int maxLen);  // search() 的无锁真身
  String searchDdg(const String& q, int maxHit, int maxLen);
  String searchBing(const String& q, int maxHit, int maxLen);
  String searchRss(const String& q, int maxHit, int maxLen);   // 默认主源：必应 RSS
  String searchCustom(const String& q, int maxHit, int maxLen); // NVS 模板主源
  bool _ok = false;
};

extern LaapSearch laapSearch;

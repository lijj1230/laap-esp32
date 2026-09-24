#include "laap_memory.h"
#include <LittleFS.h>
#include <time.h>
#include <vector>

MemorySystem memory;

static const char* EP_PATH = "/mem/episodes.jsonl";
static const int  EP_MAX   = 300;
// 语义向量缓存（B: bge-m3 双通道召回）
static const char* EMB_PATH = "/mem/emb.bin";
static const int   EMB_DIM  = 1024;

bool MemorySystem::begin() {
  if (!LittleFS.begin(true)) return false;
  LittleFS.mkdir("/mem");
  // 数一下已有条数
  _count = 0;
  File f = LittleFS.open(EP_PATH, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') _count++; } f.close(); }
  // 向量缓存对齐条数（emb.bin 与 episodes.jsonl 行序一一对应）
  _embCount = 0; _embFail = 0;
  File ef = LittleFS.open(EMB_PATH, "r");
  if (ef) { _embCount = ef.size() / (EMB_DIM * 4); ef.close(); }
  if (_embCount != (uint32_t)_count) {          // 行数不齐（淘汰/损坏）→ 缓存作废重嵌
    LittleFS.remove(EMB_PATH);
    _embCount = 0;
  }
  return true;
}

void MemorySystem::appendEpisodic(const char* role, const String& text) {
  File f = LittleFS.open(EP_PATH, "a");
  if (!f) return;
  time_t now = time(nullptr);
  uint32_t t = (now > 1700000000) ? (uint32_t)now : (millis() / 1000);
  // JSON 行，手动转义；w=重要度权重（Mem0 式分层：新记忆 1.0 起步）
  String esc; esc.reserve(text.length() + 8);
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
    else if (c == '\n') esc += "\\n";
    else esc += c;
  }
  f.printf("{\"t\":%lu,\"r\":\"%s\",\"w\":1.0,\"x\":\"%s\"}\n", (unsigned long)t, role, esc.c_str());
  f.close();
  _count++;

  if (_count > EP_MAX + 50) { // 超限 → 按权重淘汰（不是纯FIFO：重要的留、无谓的先忘）
    rewriteEpisodicByScore();
  }
}

// 淘汰策略：分数 = w（重要度）× 0.7 + 新鲜度 × 0.3，丢分数最低的 (实际条数-EP_MAX) 条
void MemorySystem::rewriteEpisodicByScore() {
  File in = LittleFS.open(EP_PATH, "r");
  if (!in) return;
  struct Rec { String line; float score; };
  std::vector<Rec> lines;
  time_t nowT = time(nullptr);
  uint32_t now = (nowT > 1700000000) ? (uint32_t)nowT : 0;
  while (in.available()) {
    String l = in.readStringUntil('\n');
    if (!l.length()) continue;
    // 抽 w 与 t
    float w = 1.0f; uint32_t t = 0;
    int wp = l.indexOf("\"w\":");
    if (wp >= 0) w = l.substring(wp + 4, l.indexOf(',', wp)).toFloat();
    int tp = l.indexOf("\"t\":");
    if (tp >= 0) {
      int te = l.indexOf(',', tp);
      t = l.substring(tp + 4, te > 0 ? te : l.indexOf('}', tp)).toFloat();
    }
    float fresh = 0;
    if (now > 0 && t > 0) {                       // NTP 有效：按真实时间衰减
      float ageDay = (now - t) / 86400.0f;
      if (ageDay < 0) ageDay = 0;
      fresh = 1.0f / (1.0f + ageDay);             // 当天≈1，一周≈0.13
    } else fresh = 0.5f;                          // 无时钟：一视同仁
    lines.push_back({l, w * 0.7f + fresh * 0.3f});
  }
  in.close();
  if (lines.size() <= EP_MAX) return;
  // 简单选择淘汰：反复找最低分丢掉（300 条规模，性能无虞）
  size_t drop = lines.size() - EP_MAX;
  for (size_t d = 0; d < drop; d++) {
    size_t worst = 0;
    for (size_t i = 1; i < lines.size(); i++)
      if (lines[i].score < lines[worst].score) worst = i;
    lines[worst].score = 999;                     // 标记淘汰
    lines[worst].line = "";
  }
  File out = LittleFS.open(EP_PATH, "w");
  if (!out) return;
  for (auto& r : lines) if (r.line.length()) out.println(r.line);
  out.close();
  _count = EP_MAX;
}

void MemorySystem::logEvent(const char* role, const String& text) {
  // 工作记忆（环）：截断单条长度；String operator= 容量足够时复用已有缓冲，不反复碎片化
  String tmp = String(role) + ":" + text;
  if (tmp.length() > 160) tmp = tmp.substring(0, 160);
  if (_work[_workHead].length() == 0) _work[_workHead].reserve(176); // 首次预留，之后容量常驻
  _work[_workHead] = tmp;
  _workHead = (_workHead + 1) % WORK_MAX;
  if (_workLen < WORK_MAX) _workLen++;
  // 情景记忆（盘）
  appendEpisodic(role, text);
}

String MemorySystem::recentContext(int maxChars) {
  String out; out.reserve(256);
  for (int i = 0; i < _workLen && (int)out.length() < maxChars; i++) {
    int idx = (_workHead - 1 - i + WORK_MAX * 2) % WORK_MAX;
    out = _work[idx] + "\n" + out;
  }
  return out;
}

// F3: 工作记忆原始条目（近→远），只取 user|aris，供真多轮 messages 用
int MemorySystem::recentTurns(String* out, int max) const {
  int n = 0;
  for (int i = _workLen - 1; i >= 0 && n < max; i--) {
    int idx = (_workHead - 1 - i + WORK_MAX * 2) % WORK_MAX;
    const String& e = _work[idx];
    bool isUser = e.startsWith("user:");
    bool isAris = e.startsWith("aris:");
    if (!isUser && !isAris) continue;
    // 独白也进对话轮：用户问"你刚才在想什么"要能接上（v3.12 前"【自发】"被排除，问必茫然）
    out[n] = e.substring(e.indexOf(':') + 1);
    if (out[n].length() > 160) out[n] = out[n].substring(0, 160);
    n++;
  }
  // out 现在是近→远；翻转为远→近（对话时序）
  for (int a = 0, b = n - 1; a < b; a++, b--) { String t = out[a]; out[a] = out[b]; out[b] = t; }
  return n;
}

String MemorySystem::searchEpisodic(const String& query, int maxChars) {
  // 取 query 中 >=2 字符的 CJK/词片段做包含匹配（简单召回）
  String out;
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return out;
  std::vector<String> hits;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (l.length() && l.indexOf(query) >= 0 && query.length() >= 2) hits.push_back(l);
  }
  f.close();
  for (int i = (int)hits.size() - 1; i >= 0 && (int)out.length() < maxChars; i--) {
    out = hits[i] + "\n" + out;
  }
  return out;
}

// ================= 语义向量（B: bge-m3 双通道召回） =================
// 向量缓存文件 /mem/emb.bin：每条 1024×float=4KB，与 episodes.jsonl 行序一一对应。
// 新行缺向量 → embedTick 限速补（每 15s 最多 1 条）；失败连 3 次停用（退关键词通道）。

// 调硅基流动 embeddings（bge-m3）；成功填充 out 并返回 true（实现在 laap_llm.cpp）
extern String laapEmbed(const String& text, bool& ok);
static bool callEmbedding(const String& text, float* out) {
  bool ok = false;
  String bin = laapEmbed(text, ok);
  if (!ok || bin.length() < EMB_DIM * 4) return false;
  memcpy(out, bin.c_str(), EMB_DIM * 4);
  return true;
}

void MemorySystem::embedTick() {
  if (_embFail >= 3) return;
  if (millis() - _embLastMs < 15000) return;              // 限速：15s 一条
  // 有未对齐的新行才干活
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return;
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.close();
  if (total <= (int)_embCount) return;                    // 全部已嵌入

  // 取第 _embCount+1 行文本
  f = LittleFS.open(EP_PATH, "r");
  int lineno = 0;
  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    if (line.length() && ++lineno == (int)_embCount + 1) break;
  }
  f.close();
  if (!line.length()) { _embCount = total; return; }      // 行数对不齐（淘汰发生过）→ 跳过本轮
  // 抽 x 字段文本
  int xp = line.indexOf("\"x\":\"");
  if (xp < 0) { _embCount++; return; }
  int xe = line.length() - 3;                              // "}\n 尾
  String text = line.substring(xp + 5, xe > xp + 5 ? xe : xp + 5);

  float* vec = (float*)malloc(EMB_DIM * 4);
  if (!vec) return;
  _embLastMs = millis();
  if (!callEmbedding(text, vec)) { free(vec); _embFail++; return; }
  _embFail = 0;
  File ef = LittleFS.open(EMB_PATH, "a");
  if (ef) { ef.write((uint8_t*)vec, EMB_DIM * 4); ef.close(); _embCount++; }
  free(vec);
}

float* MemorySystem::embVec(const String& line) { (void)line; return nullptr; }  // 检索内联于 recallSmart

// 余弦相似度
static float cosineOf(const float* a, const float* b, int n) {
  float dot = 0, na = 0, nb = 0;
  for (int i = 0; i < n; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
  if (na <= 0 || nb <= 0) return 0;
  return dot / (sqrtf(na) * sqrtf(nb));
}

// 智能回忆：主=语义 Top-K（w 加权），退=关键词
String MemorySystem::recallSmart(const String& query, int maxChars) {
  struct Hit { String line; float score; };
  std::vector<Hit> hits;

  // —— 主通道：语义 ——
  if (_embFail < 3) {
    float* qv = (float*)malloc(EMB_DIM * 4);
    if (qv) {
      _embLastMs = millis();
      if (callEmbedding(query, qv)) {
        _embFail = 0;
        File ef = LittleFS.open(EMB_PATH, "r");
        File lf = LittleFS.open(EP_PATH, "r");
        if (ef && lf) {
          float* rv = (float*)malloc(EMB_DIM * 4);
          int idx = 0;
          while (rv && ef.read((uint8_t*)rv, EMB_DIM * 4) == EMB_DIM * 4) {
            // 对应的记忆行
            String l = "";
            while (lf.available()) { l = lf.readStringUntil('\n'); if (l.length()) break; }
            if (!l.length()) break;
            float w = 1.0f;
            int wp = l.indexOf("\"w\":");
            if (wp >= 0) w = l.substring(wp + 4, l.indexOf(',', wp)).toFloat();
            float sim = cosineOf(qv, rv, EMB_DIM);
            hits.push_back({l, sim * 0.8f + w * 0.2f});   // 语义为主，权重为辅
            idx++;
            if (hits.size() > 40) {                        // 控内存：留 Top40
              size_t worst = 0;
              for (size_t i = 1; i < hits.size(); i++) if (hits[i].score < hits[worst].score) worst = i;
              hits[worst] = hits.back(); hits.pop_back();
            }
          }
          if (rv) free(rv);
        }
        if (ef) ef.close();
        if (lf) lf.close();
      } else {
        _embFail++;
      }
      free(qv);
    }
  }

  // —— 退通道：关键词（原逻辑 + w 加权排序） ——
  if (hits.empty()) {
    File f = LittleFS.open(EP_PATH, "r");
    if (!f) return "";
    while (f.available()) {
      String l = f.readStringUntil('\n');
      if (l.length() && query.length() >= 2 && l.indexOf(query) >= 0) {
        float w = 1.0f;
        int wp = l.indexOf("\"w\":");
        if (wp >= 0) w = l.substring(wp + 4, l.indexOf(',', wp)).toFloat();
        hits.push_back({l, w});
      }
    }
    f.close();
  }
  if (hits.empty()) return "";

  String out;
  for (int round = 0; round < 3 && !hits.empty(); round++) {   // 取 Top3
    size_t best = 0;
    for (size_t i = 1; i < hits.size(); i++) if (hits[i].score > hits[best].score) best = i;
    String x;
    int xp = hits[best].line.indexOf("\"x\":\"");
    if (xp >= 0) {
      x = hits[best].line.substring(xp + 5);
      x.replace("\\n", " "); x.replace("\\\"", "\"");
      int xe = x.lastIndexOf('"');
      if (xe > 0) x = x.substring(0, xe);
    }
    if (x.length()) out = out.length() ? out + "\n" + x : x;
    hits[best] = hits.back(); hits.pop_back();
    if ((int)out.length() >= maxChars) break;
  }
  return out;
}

// 用户问起该记忆 → 权重升级（Mem0 的"被召回即强化"）
void MemorySystem::rememberBoost(const String& fragment) {
  if (fragment.length() < 2) return;
  File in = LittleFS.open(EP_PATH, "r");
  if (!in) return;
  String all; all.reserve(60 * 1024);
  while (in.available()) {
    String l = in.readStringUntil('\n');
    if (!l.length()) continue;
    if (l.indexOf(fragment) >= 0) {                          // 命中行 w +0.5（上限 5）
      int wp = l.indexOf("\"w\":");
      if (wp >= 0) {
        float w = l.substring(wp + 4, l.indexOf(',', wp)).toFloat() + 0.5f;
        if (w > 5) w = 5;
        l = l.substring(0, wp + 4) + String(w, 2) + l.substring(l.indexOf(',', wp));
      }
    }
    all += l; all += "\n";
  }
  in.close();
  File out = LittleFS.open(EP_PATH, "w");
  if (!out) return;
  out.print(all);
  out.close();
}

String MemorySystem::semantic() const {
  File f = LittleFS.open("/mem/semantic.txt", "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  if (s.length() > 400) s = s.substring(0, 400);
  return s;
}

void MemorySystem::setSemantic(const String& s) {
  File f = LittleFS.open("/mem/semantic.txt", "w");
  if (!f) return;
  f.print(s);
  f.close();
}

String MemorySystem::episodicTail(int n) {
  String out;
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return "[]";
  std::vector<String> lines;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (l.length()) lines.push_back(l);
  }
  f.close();
  out = "[";
  int start = (int)lines.size() - n; if (start < 0) start = 0;
  for (int i = start; i < (int)lines.size(); i++) {
    out += lines[i];
    if (i != (int)lines.size() - 1) out += ",";
  }
  out += "]";
  return out;
}

void MemorySystem::clearAll() {
  LittleFS.remove(EP_PATH);
  LittleFS.remove("/mem/semantic.txt");
  LittleFS.remove("/evolution.json");
  _count = 0; _workLen = 0; _workHead = 0;
}

#include "laap_memory.h"
#include "laap_llm.h"   // utf8Cut
#include "laap_snap.h"  // 覆盖"自我"文件前拍快照（RSI 安全网）
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
  if (ef) {
    _embCount = ef.size() / (EMB_DIM * 4);
    if (ef.size() % (EMB_DIM * 4) != 0) {       // 掉电残条：半条向量，作废重建
      ef.close();
      LittleFS.remove(EMB_PATH);
      _embCount = 0;
      return true;
    }
    ef.close();
  }
  if (_embCount != (uint32_t)_count) {          // 行数不齐（淘汰/损坏）→ 缓存作废重嵌
    LittleFS.remove(EMB_PATH);
    _embCount = 0;
  }
  reloadWork();   // 从盘上末段重建工作记忆环：否则重启后 recentContext 为空，
                  // 夜间反思会因为"没素材"跳过、聊天也丢了最近几轮上下文
  return true;
}

void MemorySystem::appendEpisodic(const char* role, const String& rawText) {
  File f = LittleFS.open(EP_PATH, "a");
  if (!f) return;
  // 落盘前清洗：切半的汉字会让整个 episodes.jsonl 行非法，/api/memory 直接吐不出合法 JSON
  const String text = sanitizeUtf8(rawText);
  time_t now = time(nullptr);
  // 时钟未同步时 t=0（区别于"开机后秒数"——那会被 decay 当成 5 万天前而最先淘汰）
  uint32_t t = (now > 1700000000) ? (uint32_t)now : 0;
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
    } else fresh = 0.5f;                          // 无时钟（t==0 或本机无钟）：一视同仁
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
  // 原子重写：先写临时文件再 rename，掉电不会丢整个记忆文件
  laapSnapMake("episodes", false);   // 重写会整份替换：覆盖前拍一份（12h 自动节流）
  File out = LittleFS.open("/mem/episodes.tmp", "w");
  if (!out) return;
  for (auto& r : lines) if (r.line.length()) out.println(r.line);
  out.close();
  LittleFS.remove(EP_PATH);
  LittleFS.rename("/mem/episodes.tmp", EP_PATH);
  _count = EP_MAX;
  // 淘汰改变了行序 → 向量缓存整份作废（错位召回比没召回更糟），embedTick 会重建
  LittleFS.remove(EMB_PATH);
  _embCount = 0;
}

void MemorySystem::logEvent(const char* role, const String& text) {
  // 工作记忆（环）：截断单条长度；String operator= 容量足够时复用已有缓冲，不反复碎片化
  String tmp = String(role) + ":" + sanitizeUtf8(text);
  if (tmp.length() > 160) tmp = utf8Cut(tmp, 160);
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

// F3: 工作记忆原始条目（近→远），只取 user|aris，供真多轮 messages 用。
// roles 并行输出说话人（0=主人 1=它自己）：调用方必须用它标 role，
// 别再按序号奇偶猜——历史里可能连着两条 user（上一轮它没答上），猜错就整段角色反相。
int MemorySystem::recentTurns(String* out, uint8_t* roles, int max) const {
  int n = 0;
  for (int i = _workLen - 1; i >= 0 && n < max; i--) {
    int idx = (_workHead - 1 - i + WORK_MAX * 2) % WORK_MAX;
    const String& e = _work[idx];
    bool isUser = e.startsWith("user:");
    bool isAris = e.startsWith("aris:");
    if (!isUser && !isAris) continue;
    // 独白也进对话轮：用户问"你刚才在想什么"要能接上（v3.12 前"【自发】"被排除，问必茫然）
    out[n] = e.substring(e.indexOf(':') + 1);
    if (out[n].length() > 160) out[n] = utf8Cut(out[n], 160);
    if (roles) roles[n] = isAris ? 1 : 0;
    n++;
  }
  // out 现在是近→远；翻转为远→近（对话时序），roles 同步翻转
  for (int a = 0, b = n - 1; a < b; a++, b--) {
    String t = out[a]; out[a] = out[b]; out[b] = t;
    if (roles) { uint8_t r = roles[a]; roles[a] = roles[b]; roles[b] = r; }
  }
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
  if (_embFail >= 3) {
    // 熔断后不永久装死：每 5 分钟放行一次试探，成功路径会把 _embFail 清零（自愈）
    static uint32_t s_probeMs = 0;
    if (millis() - s_probeMs < 300000) return;
    s_probeMs = millis();
  }
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
  if (!line.length()) {
    // 行数对不齐（淘汰/掉电残行发生过）→ 缓存作废重建，绝不带着错位继续
    LittleFS.remove(EMB_PATH);
    _embCount = 0;
    return;
  }
  // 抽 x 字段文本
  int xp = line.indexOf("\"x\":\"");
  if (xp < 0) {
    // 非记忆行（无正文）：向量缓存与行序已脱钩，整份作废重建（原来 _embCount++ 会错位）
    LittleFS.remove(EMB_PATH);
    _embCount = 0;
    return;
  }
  int xe = line.length() - 3;                              // "}\n 尾
  String text = sanitizeUtf8(line.substring(xp + 5, xe > xp + 5 ? xe : xp + 5));

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

// 行内抽 w 与时间戳 → (w, 新鲜度 0..1)。t=0（无时钟期写入）按中性 0.5。
// 召回排序必须用它：w 被 rememberBoost 累加且永不衰减（上限 5），
// 不归一化的话老记忆能纯靠权重压过语义相似度（"很久之后突然想起来"的根因）。
static void parseWt(const String& l, float& w, float& fresh) {
  w = 1.0f; fresh = 0.5f;
  int wp = l.indexOf("\"w\":");
  if (wp >= 0) w = l.substring(wp + 4, l.indexOf(',', wp)).toFloat();
  time_t nowT = time(nullptr);
  uint32_t now = (nowT > 1700000000) ? (uint32_t)nowT : 0;
  int tp = l.indexOf("\"t\":");
  if (tp >= 0) {
    int te = l.indexOf(',', tp);
    uint32_t t = l.substring(tp + 4, te > 0 ? te : l.indexOf('}', tp)).toFloat();
    if (now > 0 && t > 0) {
      float ageDay = (now - t) / 86400.0f;
      if (ageDay < 0) ageDay = 0;
      fresh = 1.0f / (1.0f + ageDay);           // 当天≈1，一周≈0.13
    }
  }
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
            // 语义为主(0.75) + 强化权重归一(0.15, 封顶3) + 新鲜度(0.10)：
            // 老记忆只有语义上真的相关才会浮上来，不再靠被强化过的权重霸榜
            float fw, fr;
            parseWt(l, fw, fr);
            float wn = (fw > 3 ? 3 : fw) / 3.0f;
            float sim = cosineOf(qv, rv, EMB_DIM);
            hits.push_back({l, sim * 0.75f + wn * 0.15f + fr * 0.10f});
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

  // —— 退通道：关键词（无语义可用：权重与新鲜度各半，老记忆不再无条件霸榜） ——
  if (hits.empty()) {
    File f = LittleFS.open(EP_PATH, "r");
    if (!f) return "";
    while (f.available()) {
      String l = f.readStringUntil('\n');
      if (l.length() && query.length() >= 2 && l.indexOf(query) >= 0) {
        float fw, fr;
        parseWt(l, fw, fr);
        float wn = (fw > 3 ? 3 : fw) / 3.0f;
        hits.push_back({l, wn * 0.5f + fr * 0.5f});
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
      x = sanitizeUtf8(x);
    }
    // 去重 + 排除"就是这次问的这句"：连着问同一句话时，召回会把当前提问原样重复三遍，
    // 既没信息量又把模型带回原话（实测造成复读与答非所问）
    if (x.length() && out.indexOf(x) < 0 && !(query.length() >= 2 && x.indexOf(query) >= 0))
      out = out.length() ? out + "\n" + x : x;
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
  s = sanitizeUtf8(s);                 // 历史遗留的半汉字：读了就清，别让它进 JSON/请求体
  if (s.length() > 400) s = utf8Cut(s, 400);
  return s;
}

void MemorySystem::setSemantic(const String& s) {
  laapSnapMake("semantic", false);   // 自我认知被压缩/反思覆盖前拍一份（自动档 12h 节流）
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
    if (l.length()) lines.push_back(sanitizeUtf8(l));   // 老行里的半汉字就地清掉，保证 JSON 合法
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

// ================= 夜间记忆整理（Letta 式 sleep-time compute 的执行端） =================
// 带绝对行号的尾部导出（给 LLM 当素材；行号供删除指令引用）
String MemorySystem::episodicNumberedTail(int n) {
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return "";
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.close();
  int start = total - n + 1;
  if (start < 1) start = 1;
  f = LittleFS.open(EP_PATH, "r");
  if (!f) return "";
  String out;
  int lineno = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (!l.length()) continue;
    lineno++;
    if (lineno >= start) out += String(lineno) + ". " + l + "\n";
  }
  f.close();
  return out;
}

// 执行 LLM 给出的删除清单（宽容扫描，只认 n+del 配对，≤12 条）。
// 向量缓存与行序绑死：删除时同步压缩 emb.bin（对不上就整份作废重建）
void MemorySystem::applyTidyOps(const String& opsJson) {
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return;
  int total = 0;
  while (f.available()) { if (f.read() == '\n') total++; }
  f.close();
  if (!total) return;

  std::vector<char> del(total, 0);
  int cnt = 0;
  int pos = 0;
  while (cnt < 12) {
    int np = opsJson.indexOf("\"n\":", pos);
    if (np < 0) break;
    int ob = opsJson.indexOf('}', np);
    String chunk = opsJson.substring(np, ob > 0 ? ob : opsJson.length());
    pos = (ob > 0) ? ob + 1 : (int)opsJson.length();
    String numStr;
    for (unsigned int k = 4; k < chunk.length(); k++) {
      char c = chunk[k];
      if (c == ',' || c == '}') break;
      if (c >= '0' && c <= '9') numStr += c;
    }
    if (!numStr.length()) continue;
    long n = numStr.toInt();
    if (n < 1 || n > total || chunk.indexOf("\"del\"") < 0) continue;
    if (!del[n - 1]) { del[n - 1] = 1; cnt++; }
  }
  if (!cnt) { Serial.println("[TIDY] 模型无可整理项（或清单为空）"); return; }

  // 重写记忆文件（先 tmp 再 rename，掉电不丢整份），删除前强制快照
  laapSnapMake("episodes", true);
  File in = LittleFS.open(EP_PATH, "r");
  File out = LittleFS.open("/mem/episodes.tmp", "w");
  if (!in || !out) { if (in) in.close(); if (out) out.close(); return; }
  int lineno = 0, kept = 0;
  while (in.available()) {
    String l = in.readStringUntil('\n');
    if (!l.length()) continue;
    lineno++;
    if (lineno <= total && del[lineno - 1]) continue;
    out.println(l);
    kept++;
  }
  in.close(); out.close();
  LittleFS.remove(EP_PATH);
  LittleFS.rename("/mem/episodes.tmp", EP_PATH);
  _count = kept;

  // 向量缓存同步压缩（行号一一对应）；对不上号就整份作废（embedTick 会限速重建）
  File ef = LittleFS.open(EMB_PATH, "r");
  bool aligned = false;
  if (ef && (int)_embCount == total) {
    File eo = LittleFS.open("/mem/emb.tmp", "w");
    if (eo) {
      float* row = (float*)malloc(EMB_DIM * 4);
      int idx = 0, keptv = 0;
      bool okw = true;
      while (row && ef.read((uint8_t*)row, EMB_DIM * 4) == EMB_DIM * 4) {
        idx++;
        if (idx <= total && del[idx - 1]) continue;
        if (eo.write((uint8_t*)row, EMB_DIM * 4) != EMB_DIM * 4) { okw = false; break; }
        keptv++;
      }
      if (row) free(row);
      eo.close();
      if (okw && idx == total && keptv == kept) {
        LittleFS.remove(EMB_PATH);
        LittleFS.rename("/mem/emb.tmp", EMB_PATH);
        _embCount = keptv;
        aligned = true;
      } else {
        LittleFS.remove("/mem/emb.tmp");
      }
    }
  }
  if (ef) ef.close();
  if (!aligned) {
    LittleFS.remove(EMB_PATH);
    _embCount = 0;
  }
  Serial.printf("[TIDY] 整理完成：删 %d 条，剩 %d 条（向量缓存%s）\n",
                cnt, kept, aligned ? "同步压缩" : "作废重建");
}

// 新知识相对已有记忆的新颖度 0..1（1=全新，0=完全已知）。-1=无法评估（熔断/无向量/失败）。
// 供独白收割算"信息增益"：好奇心只该被真正的新知满足（active inference）。
// 失败不计入 _embFail 熔断（独白后的堆紧张会造成假失败，不该连累召回通道）。
float MemorySystem::noveltyOf(const String& text) {
  if (_embFail >= 3) return -1;
  float* qv = (float*)malloc(EMB_DIM * 4);
  if (!qv) return -1;
  _embLastMs = millis();
  float novelty = -1;
  if (callEmbedding(text, qv)) {
    _embFail = 0;
    float maxCos = 0;
    File ef = LittleFS.open(EMB_PATH, "r");
    if (ef) {
      float* rv = (float*)malloc(EMB_DIM * 4);
      if (rv) {
        while (ef.read((uint8_t*)rv, EMB_DIM * 4) == EMB_DIM * 4) {
          float c = cosineOf(qv, rv, EMB_DIM);
          if (c > maxCos) maxCos = c;
        }
        free(rv);
      }
      ef.close();
    }
    novelty = 1.0f - maxCos;
    if (novelty < 0) novelty = 0;
  }
  free(qv);
  return novelty;
}

void MemorySystem::clearAll() {
  laapSnapAll(true);   // 清空前强制全量快照：格式化/误触还有得救（.pre 之外再留 3 版）
  LittleFS.remove(EP_PATH);
  LittleFS.remove("/mem/semantic.txt");
  LittleFS.remove("/evolution.json");
  LittleFS.remove(EMB_PATH);              // 向量缓存一并清，否则旧向量错配新记忆
  _count = 0; _workLen = 0; _workHead = 0;
  _embCount = 0; _embFail = 0;
}

// ============================================================
//  记忆搬家：导出 / 导入
//  存在的理由：分区表改版（加 OTA 槽）要把 spiffs 挪位置，littlefs 的块分散在整片
//  分区里，原始镜像换了尺寸未必还能挂上——文本导出是唯一"无损且可验证"的搬迁方式。
//  顺带也是给主人的备份/恢复能力（换板子、刷机前留一手）。
//  格式（纯文本，逐行）：
//    ###LAAP-MEMORY v1
//    ###SEMANTIC
//    <自我认知，可能多行>
//    ###EPISODES
//    {"t":...,"r":"...","w":1.0,"x":"..."}
//    ###EVOLUTION
//    {"gen":...}
//    ###END
//  episodes 行都是 JSON，不可能以 ### 开头，所以段头不会误判
// ============================================================
static const char* IMP_PATH = "/mem/import.txt";

String MemorySystem::exportDump() {
  String out; out.reserve(4096);
  out += "###LAAP-MEMORY v1\n";
  out += "###SEMANTIC\n";
  { File f = LittleFS.open("/mem/semantic.txt", "r");
    if (f) { while (f.available()) { String l = f.readStringUntil('\n'); out += sanitizeUtf8(l); out += '\n'; } f.close(); } }
  out += "###EPISODES\n";
  { File f = LittleFS.open(EP_PATH, "r");
    if (f) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim();
              if (l.length()) { out += sanitizeUtf8(l); out += '\n'; } } f.close(); } }
  out += "###EVOLUTION\n";
  { File f = LittleFS.open("/evolution.json", "r");
    if (f) { String s = f.readString(); s.trim(); if (s.length()) { out += s; out += '\n'; } f.close(); } }
  out += "###END\n";
  return out;
}

bool MemorySystem::importBegin() {
  LittleFS.remove(IMP_PATH);   // 清掉上次的残留：导入必须是"这份文件"说了算
  return true;
}

bool MemorySystem::importWrite(const uint8_t* d, size_t n) {
  File f = LittleFS.open(IMP_PATH, "a");
  if (!f) return false;
  size_t w = f.write(d, n);
  f.close();
  return w == n;
}

void MemorySystem::importEnd() { /* 每次 write 都已落盘并关闭，无需收尾 */ }

bool MemorySystem::applyImport(String& msg) {
  File in = LittleFS.open(IMP_PATH, "r");
  if (!in) { msg = "没收到上传内容"; return false; }
  File epsTmp = LittleFS.open("/mem/episodes.imp", "w");
  if (!epsTmp) { in.close(); msg = "文件系统写入失败"; return false; }
  int section = 0, nSem = 0, nEps = 0, nEvo = 0;
  String semBuf, evoBuf;
  while (in.available()) {
    String l = in.readStringUntil('\n');
    l.trim();
    if (l.startsWith("###")) {
      if      (l.startsWith("###SEMANTIC"))  section = 1;
      else if (l.startsWith("###EPISODES"))  section = 2;
      else if (l.startsWith("###EVOLUTION")) section = 3;
      else section = 0;                      // ###LAAP-MEMORY / ###END
      continue;
    }
    if (!l.length()) continue;
    if (section == 1) { semBuf += l; nSem++; }
    else if (section == 2) { epsTmp.println(l); nEps++; }
    else if (section == 3) { evoBuf += l; nEvo++; }
  }
  in.close();
  epsTmp.close();
  // 认不出格式就别动记忆（宁可不导入，也不能把主人的人格清成白纸）
  if (!nEps && !nSem && !nEvo) {
    LittleFS.remove("/mem/episodes.imp"); LittleFS.remove(IMP_PATH);
    msg = "内容不像记忆备份（没有 ###SEMANTIC/###EPISODES 段头）";
    return false;
  }
  // 分段可缺：只有情景段才替换情景记忆（否则"只恢复性格"的导入会把记忆清空）
  laapSnapAll(true);   // 导入=整份覆盖"自我"：先把现状强制快照
  if (nEps) {
    LittleFS.remove(EP_PATH);
    LittleFS.rename("/mem/episodes.imp", EP_PATH);
  } else {
    LittleFS.remove("/mem/episodes.imp");
  }
  // 语义/性格：有就覆盖，没有就保留原样
  if (nSem) { File f = LittleFS.open("/mem/semantic.txt", "w"); if (f) { f.print(semBuf); f.close(); } }
  if (nEvo) { File f = LittleFS.open("/evolution.json", "w"); if (f) { f.print(evoBuf); f.close(); } }
  LittleFS.remove(EMB_PATH);   // 向量与行序绑死：换了记忆必须整份作废重建
  LittleFS.remove(IMP_PATH);
  _embCount = 0; _embFail = 0;
  _count = 0;
  File f = LittleFS.open(EP_PATH, "r");
  if (f) { while (f.available()) { if (f.read() == '\n') _count++; } f.close(); }
  reloadWork();
  msg = String("已导入 ");
  if (nEps) msg += String(nEps) + " 条情景记忆";
  if (nSem) msg += (nEps ? "、" : "") + String("自我认知");
  if (nEvo) msg += ((nEps || nSem) ? "、" : "") + String("性格进化");
  if (!nEps) msg += "（备份里没有情景段，原有记忆保持不变）";
  Serial.printf("[MEM] %s（_count=%lu）\n", msg.c_str(), (unsigned long)_count);
  return true;
}

// 从盘上末段重建工作记忆环：导入后立刻就能"想起"，不用等到下次聊天
void MemorySystem::reloadWork() {
  _workLen = 0; _workHead = 0;
  File f = LittleFS.open(EP_PATH, "r");
  if (!f) return;
  std::vector<String> tail;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (!l.length()) continue;
    int rp = l.indexOf("\"r\":\"");
    int xp = l.indexOf("\"x\":\"");
    if (rp < 0 || xp < 0) continue;
    String role = l.substring(rp + 5, l.indexOf('"', rp + 5));
    String x = l.substring(xp + 5);
    x.replace("\\n", " "); x.replace("\\\"", "\"");
    int xe = x.lastIndexOf('"');
    if (xe > 0) x = x.substring(0, xe);
    x = sanitizeUtf8(x);
    tail.push_back(role + ":" + x);
    if (tail.size() > WORK_MAX) tail.erase(tail.begin());
  }
  f.close();
  for (auto& t : tail) {
    if (t.length() > 160) t = utf8Cut(t, 160);
    _work[_workHead] = t;
    _workHead = (_workHead + 1) % WORK_MAX;
    if (_workLen < WORK_MAX) _workLen++;
  }
}

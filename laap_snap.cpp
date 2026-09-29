#include "laap_snap.h"
#include <LittleFS.h>
#include <time.h>   // getLastWrite() 是墙钟 epoch，节流/年龄都得拿 time(nullptr) 相减

static const SnapEntry SNAPS[] = {
  { "semantic",  "/mem/semantic.txt"  },
  { "episodes",  "/mem/episodes.jsonl" },
  { "evolution", "/evolution.json"    },
  { "rules",     "/mem/rules.txt"     },
  { "skills",    "/mem/skills.txt"    },
  { "relations", "/mem/relations.jsonl" },   // v3.51 审计：关系记忆也是"自我"的一部分，补进快照链
};
static const int SNAP_N = sizeof(SNAPS) / sizeof(SNAPS[0]);
static const char* SNAP_DIR = "/snap";
static const uint32_t SNAP_MIN_INTERVAL_MS = 12UL * 3600UL * 1000UL;  // 自动档节流（SNAP_KEEP 在 laap_snap.h）

static const SnapEntry* findEntry(const char* name) {
  for (int i = 0; i < SNAP_N; i++)
    if (!strcmp(SNAPS[i].name, name)) return &SNAPS[i];
  return nullptr;
}

static bool dirReady() {
  static bool s_ok = false;
  if (s_ok) return true;
  if (!LittleFS.exists(SNAP_DIR)) LittleFS.mkdir(SNAP_DIR);
  s_ok = LittleFS.exists(SNAP_DIR);
  return s_ok;
}

// 分块拷贝（episodes.jsonl 可到几十 KB，不整读进堆）
static bool copyFile(const char* dst, const char* src) {
  File in = LittleFS.open(src, "r");
  if (!in) return false;
  File out = LittleFS.open(dst, "w");
  if (!out) { in.close(); return false; }
  uint8_t buf[1024];
  bool ok = true;
  while (in.available()) {
    size_t n = in.read(buf, sizeof(buf));
    if (!n || out.write(buf, n) != n) { ok = false; break; }
  }
  in.close(); out.close();
  return ok;
}

static String snapPath(const char* name, int ver) {
  return String(SNAP_DIR) + "/" + name + "." + ver;
}

bool laapSnapMake(const char* name, bool force) {
  const SnapEntry* e = findEntry(name);
  if (!e || !dirReady()) return false;
  if (!LittleFS.exists(e->path)) return false;          // 源文件还没有（首次启动）→ 没什么可拍
  String newest = snapPath(name, 1);
  if (!force && LittleFS.exists(newest)) {
    File f = LittleFS.open(newest, "r");
    time_t lw = f ? f.getLastWrite() : 0;
    bool clockOk = time(nullptr) > 1700000000;   // 本机时钟是否可信
    bool skip = false;
    if (!clockOk) {
      skip = true;                // 自己的钟都不准，没法判新旧：宁可不拍（mtime 全是假的）
    } else if (lw < 1700000000) {
      skip = false;               // 旧快照写于无时钟期（mtime 是开机秒数）：时间不可考 → 放行重拍，
                                  // 新快照写入真 mtime 后节流才恢复正常（否则节流永久锁死）
    } else {
      uint32_t ageS = (uint32_t)((long)time(nullptr) - (long)lw);
      skip = (ageS < SNAP_MIN_INTERVAL_MS / 1000);
    }
    if (f) f.close();
    if (skip) return false;       // 自动档跳过
  }
  // 先写 tmp 再轮换再转正（v3.51 审计）：原顺序"先删 newest 再 copyFile"——
  // copyFile 失败（盘满/源文件恰在重写窗口不存在）时 v1 已成空壳且无本体
  String tmp = newest + ".tmp";
  LittleFS.remove(tmp);
  if (!copyFile(tmp.c_str(), e->path)) {
    LittleFS.remove(tmp);
    Serial.printf("[SNAP] %s 快照写入失败（轮换未动，旧版保留）\n", name);
    return false;
  }
  for (int v = SNAP_KEEP; v >= 2; v--) {                // .2→.3，.1→.2（老的被挤掉）
    String from = snapPath(name, v - 1), to = snapPath(name, v);
    if (LittleFS.exists(to)) LittleFS.remove(to);
    if (LittleFS.exists(from)) LittleFS.rename(from, to);
  }
  if (LittleFS.exists(newest)) LittleFS.remove(newest);
  LittleFS.rename(tmp.c_str(), newest.c_str());
  Serial.printf("[SNAP] %s 已快照（%s）\n", name, e->path);
  return true;
}

void laapSnapAll(bool force) {
  for (int i = 0; i < SNAP_N; i++) laapSnapMake(SNAPS[i].name, force);
}

bool laapSnapRestore(const char* name, int ver) {
  const SnapEntry* e = findEntry(name);
  if (!e || ver < 1 || ver > SNAP_KEEP || !dirReady()) return false;
  String src = snapPath(name, ver);
  if (!LittleFS.exists(src)) return false;
  {   // 空快照拒绝恢复（v3.51 审计：写失败可能留下空壳占着版本号，恢复=把自我清成白纸）
    File cf = LittleFS.open(src, "r");
    size_t sz = cf ? cf.size() : 0;
    if (cf) cf.close();
    if (!sz) { Serial.printf("[SNAP] %s v%d 是空快照，拒绝恢复\n", name, ver); return false; }
  }
  if (LittleFS.exists(e->path))                         // 现状先存 .pre（回滚本身也要有后悔药）
    copyFile((String(SNAP_DIR) + "/" + name + ".pre").c_str(), e->path);
  if (!copyFile(e->path, src.c_str())) return false;
  LittleFS.remove("/mem/emb.bin");   // 向量与行序绑死：恢复后整份重建（v3.51：原来只靠条数不符兜底）
  Serial.printf("[SNAP] %s 已恢复到第 %d 版（原状态在 .pre）\n", name, ver);
  return true;
}

String laapSnapListJson() {
  String j = "[";
  for (int i = 0; i < SNAP_N; i++) {
    if (i) j += ",";
    j += String("{\"name\":\"") + SNAPS[i].name + "\",\"path\":\"" + SNAPS[i].path + "\",\"v\":[";
    bool any = false;
    for (int v = 1; v <= SNAP_KEEP; v++) {
      String p = snapPath(SNAPS[i].name, v);
      if (!LittleFS.exists(p)) continue;
      File f = LittleFS.open(p, "r");
      if (!f) continue;
      time_t lw = f.getLastWrite();
      // ts<1.7e9（NTP 同步前写的）：年龄没有意义，报 -1 让前端显示"时间未知"
      int32_t ageH = (lw < 1700000000) ? -1 : (int32_t)(((long)time(nullptr) - (long)lw) / 3600L);
      if (any) j += ",";
      j += String("{\"n\":") + v + ",\"kb\":" + ((f.size() + 512) / 1024) + ",\"age_h\":" + ageH + "}";   // 四舍五入（v3.51：整除让 <1KB 全显 0）
      f.close();
      any = true;
    }
    j += "]}";
  }
  return j + "]";
}

String laapSnapListText() {
  String t;
  for (int i = 0; i < SNAP_N; i++) {
    t += String(SNAPS[i].name) + " (" + SNAPS[i].path + "):";
    bool any = false;
    for (int v = 1; v <= SNAP_KEEP; v++) {
      String p = snapPath(SNAPS[i].name, v);
      if (!LittleFS.exists(p)) continue;
      File f = LittleFS.open(p, "r");
      if (!f) continue;
      time_t lw = f.getLastWrite();
      t += String(" v") + v + "=" + (f.size() / 1024) + "KB/" +
           (lw < 1700000000 ? String("时间未知") : String((uint32_t)((long)time(nullptr) - (long)lw) / 3600UL) + "h前");
      f.close();
      any = true;
    }
    t += any ? "\n" : " 无快照\n";
  }
  return t;
}

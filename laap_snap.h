#pragma once
#include <Arduino.h>

// ============================================================
// LAAP-lite 自我数据快照回滚（RSI 安全网）
//   "自我" = /mem/semantic.txt（自我认知）/ /mem/episodes.jsonl（情景记忆）
//          / /evolution.json（性格进化）。它们是自进化循环的载体，
//   载体被写坏/进化跑偏时要有后悔药（Darwin Gödel Machine 的核心教训）。
//   存储：/snap/<name>.1（最新）… .3（最旧），写前轮换。
//   两档：
//     自动档 laapSnapMake(name,false) —— 同一文件 12h 内不重复拍（每次心跳都
//            存 evolution.json 也没关系，节流后只是一次 stat）；
//     强制档 laapSnapMake(name,true)  —— 清空记忆/导入记忆等破坏性操作前。
//   恢复 laapSnapRestore：先把当前文件存成 .pre，再把快照拷回去（快照保留）。
//   查看：/api/snapshots、串口 /snap；恢复后建议重启（工作记忆环/向量缓存重建）
// ============================================================

// logical 名 → 磁盘路径的登记表（想加新文件：表里添一行即可）
struct SnapEntry { const char* name; const char* path; };

// 每个文件保留的版本数（v3.51：唯一定义——web 的 ver 上限曾硬编码 3）
constexpr int SNAP_KEEP = 3;

// 拍一份（自动档按 12h 节流）。返回是否真的拍了。
bool laapSnapMake(const char* name, bool force = false);
// 全部登记文件各拍一份（force 透传）
void laapSnapAll(bool force = false);
// 恢复：name 的第 ver 版拷回原路径；恢复前先把现状存为 /snap/<name>.pre
bool laapSnapRestore(const char* name, int ver);
// 列表 JSON：[{"name":"semantic","path":"/mem/semantic.txt","v":[{"n":1,"kb":1,"age_h":3.2},...]}]
String laapSnapListJson();
// 串口/日志用的人读视图
String laapSnapListText();

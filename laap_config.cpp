#include "laap_config.h"
#include "laap_memory.h"

LaapConfig cfg;

// 小凌⑥: 信任值归一化 0..1（NVS 存 0..255），跨重启不丢
static float g_trust = 0.6f;
float laapTrust() { return g_trust; }
void laapTrustSet(float v) { if (v < 0) v = 0; if (v > 1) v = 1; g_trust = v; }

void LaapConfig::begin() {
  prefs.begin("laap", false);
}

void LaapConfig::load() {
  // 全量回默认再读 NVS（v3.51）：原来手动重置 9 个字符串+若干数值，其余 20 个字符字段
  // 在 getString 失败时不写缓冲（本版核心语义）→ load 二次调用会静默沿用 RAM 旧值。
  // 结构体默认值就是唯一权威默认表，整份重置后再由 NVS 覆盖
  s = LaapSettings();

  prefs.getString("ssid", s.wifiSsid, sizeof(s.wifiSsid));
  prefs.getString("pass", s.wifiPass, sizeof(s.wifiPass));
  prefs.getString("llmbase", s.llmBase, sizeof(s.llmBase));
  prefs.getString("llmkey", s.llmKey, sizeof(s.llmKey));
  prefs.getString("llmmodel", s.llmModel, sizeof(s.llmModel));
  prefs.getString("agent", s.agentName, sizeof(s.agentName));
  prefs.getString("owner", s.ownerName, sizeof(s.ownerName));
  prefs.getString("persona", s.persona, sizeof(s.persona));
  prefs.getString("wcity", s.city, sizeof(s.city));
  s.tickSec   = prefs.getUInt("tick", 30);
  s.threshold = prefs.getUChar("thold", 55);
  s.idleSilenceMin = prefs.getUShort("idlesil", 10);
  s.idleEveryMin   = prefs.getUShort("idleevery", 20);
  s.quietStart     = prefs.getUChar("qstart", 23);
  s.quietEnd       = prefs.getUChar("qend", 6);
  s.volume         = prefs.getUChar("vol", 70);
  s.brightness     = prefs.getUChar("bright", 90);
  s.screenOffSec   = prefs.getUShort("screenoff", 60);
  s.vadStopMs      = prefs.getUShort("vadstop", 5000);
  _uptimeBaseMin   = prefs.getUInt("uptmin", 0);
  s.llmContinue    = prefs.getUChar("llmcont", 1);
  s.llmMaxTokens   = prefs.getUShort("llmtok", 500);
  s.llmNoThink     = prefs.getUChar("nothink", 1);
  laapTrustSet(prefs.getUChar("trust", 153) / 255.0f);  // 默认 0.6
  prefs.getString("srchkeys", s.searchKeys, sizeof(s.searchKeys));
  prefs.getString("srchapi", s.searchApi, sizeof(s.searchApi));
  s.voiceMode  = prefs.getUChar("vmode", 1);
  s.ttsChannel = prefs.getUChar("ttsch", 0);
  prefs.getString("ttsvoice", s.ttsVoice, sizeof(s.ttsVoice));
  prefs.getString("ttsrate", s.ttsRate, sizeof(s.ttsRate));
  prefs.getString("volcappid", s.volcAppid, sizeof(s.volcAppid));
  prefs.getString("volctoken", s.volcToken, sizeof(s.volcToken));
  prefs.getString("volcvoice", s.volcVoice, sizeof(s.volcVoice));
  prefs.getString("asrbase", s.asrBase, sizeof(s.asrBase));
  prefs.getString("asrkey", s.asrKey, sizeof(s.asrKey));
  prefs.getString("asrmodel", s.asrModel, sizeof(s.asrModel));
  prefs.getString("asr2base", s.asr2Base, sizeof(s.asr2Base));
  prefs.getString("asr2key", s.asr2Key, sizeof(s.asr2Key));
  prefs.getString("asr2model", s.asr2Model, sizeof(s.asr2Model));
  prefs.getString("embbase", s.embBase, sizeof(s.embBase));
  prefs.getString("embkey", s.embKey, sizeof(s.embKey));
  prefs.getString("embmodel", s.embModel, sizeof(s.embModel));
  prefs.getString("wakeword", s.wakeWord, sizeof(s.wakeWord));
  prefs.getString("visionbase", s.visionBase, sizeof(s.visionBase));
  prefs.getString("vlbase", s.visionLlmBase, sizeof(s.visionLlmBase));
  prefs.getString("vkey", s.visionKey, sizeof(s.visionKey));
  prefs.getString("vmodel", s.visionModel, sizeof(s.visionModel));

}

bool LaapConfig::save() {
  // 关键键写失败要出声（v3.51）：原来 save() 恒 true，NVS 满/句柄异常时网页显示"已保存"、
  // 重启还原（历史上"配置重启还原"类故障最易被误判为"网页没保存"）
  bool ok = (prefs.putString("ssid", s.wifiSsid) != 0);
  prefs.putString("pass", s.wifiPass);
  prefs.putString("llmbase", s.llmBase);
  ok = (prefs.putString("llmkey", s.llmKey) != 0) && ok;
  if (!ok) Serial.println("[LAAP] !! 配置写入 NVS 失败（空间不足？）——本次保存可能未生效");
  prefs.putString("llmmodel", s.llmModel);
  prefs.putString("agent", s.agentName);
  prefs.putString("owner", s.ownerName);
  prefs.putString("persona", s.persona);
  prefs.putString("wcity", s.city);
  prefs.putUInt("tick", s.tickSec);
  prefs.putUChar("thold", s.threshold);
  prefs.putUShort("idlesil", s.idleSilenceMin);
  prefs.putUShort("idleevery", s.idleEveryMin);
  prefs.putUChar("qstart", s.quietStart);
  prefs.putUChar("qend", s.quietEnd);
  prefs.putUChar("vol", s.volume);
  prefs.putUChar("bright", s.brightness);
  prefs.putUShort("screenoff", s.screenOffSec);
  prefs.putUShort("vadstop", s.vadStopMs);
  prefs.putUChar("llmcont", s.llmContinue);
  prefs.putUShort("llmtok", s.llmMaxTokens);
  prefs.putUChar("nothink", s.llmNoThink);
  prefs.putUChar("trust", (uint8_t)(laapTrust() * 255));
  prefs.putString("srchkeys", s.searchKeys);
  prefs.putString("srchapi", s.searchApi);
  prefs.putUChar("vmode", s.voiceMode);
  prefs.putUChar("ttsch", s.ttsChannel);
  prefs.putString("ttsvoice", s.ttsVoice);
  prefs.putString("ttsrate", s.ttsRate);
  prefs.putString("volcappid", s.volcAppid);
  prefs.putString("volctoken", s.volcToken);
  prefs.putString("volcvoice", s.volcVoice);
  prefs.putString("asrbase", s.asrBase);
  prefs.putString("asrkey", s.asrKey);
  prefs.putString("asrmodel", s.asrModel);
  prefs.putString("asr2base", s.asr2Base);
  prefs.putString("asr2key", s.asr2Key);
  prefs.putString("asr2model", s.asr2Model);
  prefs.putString("embbase", s.embBase);
  prefs.putString("embkey", s.embKey);
  prefs.putString("embmodel", s.embModel);
  prefs.putString("wakeword", s.wakeWord);
  prefs.putString("visionbase", s.visionBase);
  prefs.putString("vlbase", s.visionLlmBase);
  prefs.putString("vkey", s.visionKey);
  prefs.putString("vmodel", s.visionModel);
  return ok;
}

// 轻量保存：只写信任值（心跳里周期调用，避免整盘 30+ 键重写磨损 NVS）
void LaapConfig::saveTrust() {
  prefs.putUChar("trust", (uint8_t)(laapTrust() * 255));
}

// 累计运行时长落盘（同样只写一个键；5 分钟一次 + 重启前一次，磨损可忽略）
void LaapConfig::saveUptime(uint32_t totalMin) {
  _uptimeBaseMin = totalMin;
  prefs.putUInt("uptmin", totalMin);
}

void LaapConfig::reset() {
  prefs.clear();
  memory.clearAll();
}

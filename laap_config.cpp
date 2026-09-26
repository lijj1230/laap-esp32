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
  s.wifiSsid[0] = s.wifiPass[0] = 0;
  strlcpy(s.llmBase, "https://api.deepseek.com", sizeof(s.llmBase));
  strlcpy(s.llmModel, "deepseek-chat", sizeof(s.llmModel));
  strlcpy(s.agentName, "Aris", sizeof(s.agentName));
  strlcpy(s.ownerName, "主人", sizeof(s.ownerName));
  s.tickSec = 30;
  s.threshold = 55;
  s.idleSilenceMin = 10;
  s.idleEveryMin = 20;
  s.volume = 70;
  s.brightness = 90;
  s.screenOffSec = 60;
  s.llmContinue = 1;
  s.llmMaxTokens = 500;
  s.llmNoThink = 1;
  s.voiceMode = 1;
  s.ttsChannel = 0;
  strlcpy(s.ttsVoice, "zh-CN-XiaoxiaoNeural", sizeof(s.ttsVoice));
  strlcpy(s.ttsRate, "+0%", sizeof(s.ttsRate));
  strlcpy(s.volcVoice, "zh_female_cancan_mars_bigtts", sizeof(s.volcVoice));
  strlcpy(s.asrBase, "https://api.siliconflow.cn/v1", sizeof(s.asrBase));
  strlcpy(s.asrModel, "FunAudioLLM/SenseVoiceSmall", sizeof(s.asrModel));

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
  s.volume         = prefs.getUChar("vol", 70);
  s.brightness     = prefs.getUChar("bright", 90);
  s.screenOffSec   = prefs.getUShort("screenoff", 60);
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

  _provisioned = (s.wifiSsid[0] != 0) && (s.llmKey[0] != 0);
}

bool LaapConfig::save() {
  prefs.putString("ssid", s.wifiSsid);
  prefs.putString("pass", s.wifiPass);
  prefs.putString("llmbase", s.llmBase);
  prefs.putString("llmkey", s.llmKey);
  prefs.putString("llmmodel", s.llmModel);
  prefs.putString("agent", s.agentName);
  prefs.putString("owner", s.ownerName);
  prefs.putString("persona", s.persona);
  prefs.putString("wcity", s.city);
  prefs.putUInt("tick", s.tickSec);
  prefs.putUChar("thold", s.threshold);
  prefs.putUShort("idlesil", s.idleSilenceMin);
  prefs.putUShort("idleevery", s.idleEveryMin);
  prefs.putUChar("vol", s.volume);
  prefs.putUChar("bright", s.brightness);
  prefs.putUShort("screenoff", s.screenOffSec);
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
  _provisioned = (s.wifiSsid[0] != 0) && (s.llmKey[0] != 0);
  return true;
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

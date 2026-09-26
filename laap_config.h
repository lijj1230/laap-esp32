#pragma once
#include <Arduino.h>
#include <Preferences.h>

// LAAP 设备配置（NVS 持久化，Web 后台可改）
struct LaapSettings {
  char wifiSsid[33]  = "";
  char wifiPass[65]  = "";
  char llmBase[129]  = "https://api.deepseek.com"; // OpenAI 兼容 base
  char llmKey[129]   = "";                          // 大模型 API Key
  char llmModel[49]  = "deepseek-chat";
  char agentName[25] = "Aris";                      // 数字生命名字
  char ownerName[25] = "主人";
  char persona[257]  = "";                          // 附加人设描述
  char city[33]      = "";                          // 所在城市（天气用；空=按出口IP定位，定位可能不准）
  uint32_t tickSec   = 30;                          // PSI 心跳周期(秒)
  uint8_t threshold  = 55;                          // 主动表达阈值(0-100)
  uint16_t idleSilenceMin = 10;                     // 独白起始静默(分钟)
  uint16_t idleEveryMin   = 20;                     // 独白间隔(分钟，0=关闭独白)
  uint8_t volume = 70;                              // 喇叭音量(0-100, ES8311)
  uint8_t brightness = 90;                          // 屏幕亮度(0-100, 背光PWM)
  uint16_t screenOffSec = 60;                       // 静默息屏秒数(0=不息屏)
  uint16_t vadStopMs = 5000;                        // 说完静音判停(ms)：停顿超过即收音。大=不截断但答得慢
  uint8_t llmContinue = 1;                          // 输出截断自动续写轮数(0=关, 最多3)
  uint8_t llmNoThink = 1;                           // 1=请求里带 thinking:disabled（思考型模型光想不答时用；不支持的服务商会被忽略或报错）
  uint16_t llmMaxTokens = 500;                      // 单次回复 maxTokens(80-1000, 聊天/表达/独白共用；思考型模型会先花 token 思考，过小会空回复)
  // 小凌⑥: 信任标量（NVS 键 trust，0..255 ↔ 0..1）——读写走 laapTrust()/laapTrustSet()
  // 注入提示词前由主程序把 laapTrust() 拷进 mind.trust；cfg.save() 时从 mind.trust 取回
  char searchKeys[193] = "什么,怎么,如何,为什么,为啥,多少,几,哪,新闻,今天,最新,查,搜索,search,who,what,how,why,when,news"; // 聊天搜索触发词(逗号分隔,空=关)
  char searchApi[161]  = "";                        // 搜索主源URL模板({q}=查询词,空=必应RSS默认)

  // ---- 语音 v2 ----
  uint8_t voiceMode   = 1;      // 0=关 1=按键对讲 2=VAD自动听
  uint8_t ttsChannel  = 0;      // 0=Edge→火山回退(默认) 1=仅Edge 2=仅火山 3=静音
  char ttsVoice[49]   = "zh-CN-XiaoxiaoNeural";
  char ttsRate[16]    = "+0%";
  char volcAppid[49]  = "";
  char volcToken[129] = "";
  char volcVoice[49]  = "zh_female_cancan_mars_bigtts";
  char asrBase[129]   = "https://api.siliconflow.cn/v1";
  char asrKey[129]    = "";
  char asrModel[49]   = "FunAudioLLM/SenseVoiceSmall";
  char asr2Base[129]  = "";                        // 备用 ASR（空=不启用；base 含 dashscope 走百炼原生路径）
  char asr2Key[129]   = "";
  char asr2Model[49]  = "";
  char embBase[129]   = "";                        // 语义向量（识海召回）：空=复用 ASR 的 base/key
  char embKey[129]    = "";                        // （bge-m3 只有硅基流动等家有，ASR 换服务商时在此单配）
  char embModel[49]   = "";                        // 空=BAAI/bge-m3
  char wakeWord[25]   = "";                        // F8: 唤醒词（空=关门，VAD 即应答）
  char visionBase[129] = "";                       // F9: 视觉手机桥 URL（空=不走桥）
  char visionLlmBase[129] = "";                    // F9: 直连视觉 base（空=OpenRouter）
  char visionKey[129]  = "";                       // F9: 直连视觉 Key（空=复用 llmKey）
  char visionModel[49] = "";                       // F9: 直连视觉模型（空=gemini-flash-1.5）
};

class LaapConfig {
public:
  void begin();
  void load();
  bool save();
  void saveTrust();             // 只落 trust 一个键（轻量，供心跳周期调用，不整盘重写）
  void reset();                 // 恢复出厂（清空 NVS + 记忆文件）
  bool provisioned() const { return _provisioned; }
  LaapSettings s;
  // 累计运行时长（跨重启）：开机读回 base，运行中周期性 base+本机 millis() 落盘
  uint32_t uptimeBase() const { return _uptimeBaseMin; }
  void saveUptime(uint32_t totalMin);
private:
  Preferences prefs;
  bool _provisioned = false;
  uint32_t _uptimeBaseMin = 0;
};

// 定义在 laap-esp32.ino：累计运行分钟数 / 立即落盘（重启前调用，少丢一截时长）
uint32_t laapUptimeMin();
void laapUptimePersist();

extern LaapConfig cfg;

float laapTrust();              // 小凌⑥: 信任 0..1（cfg.cpp 内持久化）
void laapTrustSet(float v);

#include "laap_voice.h"
#include "laap_config.h"
#include "laap_audio.h"
#include "laap_edge_tts.h"
#include "laap_speech.h"
#include "laap_display.h"
#include "laap_web.h"
#include "laap_metrics.h"

LaapVoice voice;

void LaapVoice::begin() {
  _ready = audio.begin();
  if (_ready && audio.micOk) {
    Serial.println("[VOICE] 环境噪声校准 1.5s…（保持安静）");
    audio.vadCalibrate(1500);
  }
  Serial.printf("[VOICE] ready=%d spk=%d mic=%d\n", _ready, audio.spkOk, audio.micOk);
}

void LaapVoice::speak(const String& text, const char* expr) {
  if (!_ready || (VoiceMode)cfg.s.voiceMode == VoiceMode::Off) return;
  if (!text.length()) return;
  // 栈水位护栏：TTS 要过一次 TLS 握手，峰值吃 6-8KB（默认 8KB 环任务栈实测踩金丝雀重启）。
  // 栈不够时宁可不说话也不能重启——文字已经上屏，用户照样看得到回复。
  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  if (hw < 2600) {
    Serial.printf("[VOICE] 栈余量仅 %u 字节，跳过播报（防栈溢出重启；文字已显示）\n",
                  (unsigned)(hw * sizeof(StackType_t)));
    display.drawFace(expr ? expr : "calm", false);
    _cooldownMs = millis() + 600;
    return;
  }
  _busy = true;
  display.drawFace(expr ? expr : "calm", true); // 说话=思考眉

  bool ok = false;
  // 通道: 0=Edge→火山回退(默认) 1=仅Edge 2=仅火山 3=静音(不出声)
  if (cfg.s.ttsChannel == 0 || cfg.s.ttsChannel == 1) {
    ok = edgeTts.speak(text, String(cfg.s.ttsVoice), String(cfg.s.ttsRate));
    if (!ok) Serial.printf("[VOICE] Edge TTS 失败: %s\n", edgeTts.lastError.c_str());
  }
  if (!ok && (cfg.s.ttsChannel == 0 || cfg.s.ttsChannel == 2)) {
    String err;
    ok = volcTts.speak(text, err);
    if (!ok) Serial.printf("[VOICE] 火山 TTS 失败: %s\n", err.c_str());
  }
  if (!ok) {
    Serial.println("[VOICE] 全部 TTS 通道失败 → 静音降级（文字已显示）");
    display.drawFace("tired", false);
    delay(600);
  } else {
    if (expr) display.drawFace(expr, false);
  }
  if (audio.interrupted()) metrics.interruptedPlay();   // 播放被人声/按键打断（barge-in）
  // 播报冷却：等回声消散，避免 VAD 自触发（时长由自调优旋钮 A 控制）。
  // 同时清预滚——自家 TTS 尾音绝不能留在预滚里，否则下一轮录音会把"自己说的话"交给 ASR
  _cooldownMs = millis() + _cooldownDur;
  audio.prerollFlush();
  _busy = false;
}

bool LaapVoice::listenAndTranscribe(String& heard) {
  heard = "";
  if (!audio.micOk) { lastError = "无麦克风"; return false; }
  Serial.println("[VOICE] 请说…（静音 0.8s 结束，最多 12s）");
  display.drawFace("curious", true);

  audio.recordStart(12);
  uint32_t t0 = millis();
  bool spoke = false;
  while (millis() - t0 < 14000) {
    audio.recordTick();
    if (audio.vadSpeaking()) { spoke = true; t0 = millis() - 6000; } // 说话中刷新等待窗
    else if (spoke) break;                                            // 说完静音
    if (audio.recordBytes() >= 12UL * 16000 * 2 - 1024) break;
    delay(5);
  }
  size_t got = audio.recordBytes();
  audio.recordStop();
  if (!spoke || got < 8000) { metrics.noSpeechHeard(); lastError = "没听清"; return false; }

  String err;
  heard = asr.transcribe(audio.recordData(), got, err);
  metrics.asr(heard.length() > 0);   // ASR 空识别率：排障与自调优的核心 fitness
  if (!heard.length()) {
    metrics.failNote(String("ASR: ") + err);   // 规则归纳的失败素材
    lastError = "ASR: " + err;
    return false;
  }
  Serial.printf("[VOICE] 听到: %s\n", heard.c_str());
  return true;
}

static bool g_manualOnce = false;   // F8: 按键对讲一次性绕过唤醒词门
void laapVoiceSetManualOnce() { g_manualOnce = true; }

String LaapVoice::converse() {
  if (_busy || !_ready) return "";
  // 播报后冷却期内不接受新的按键轮次（VAD 路径一直有这层，按键路径以前没有）：
  // 麦克风增益调到 37.5dB 后，它自己的回声足以被判成"有人在说话"，紧接着按一下 BOOT
  // 就会让它对着自己的尾音再答一次 —— 症状同样是"回答了两次"。
  if ((int32_t)(millis() - _cooldownMs) < 0) {
    metrics.cooldownDrop();   // 冷却期丢弃：37.5dB 高增益下回声自触发的量（/micgain 调参参考）
    Serial.println("[VOICE] 刚播报完还在冷却期，忽略这次触发（防自听见）");
    return "";
  }
  metrics.vadTrigger();       // 一轮真实对话（含"没听清"和唤醒词拒绝）
  String heard;
  if (!listenAndTranscribe(heard)) {
    if (lastError == "没听清") voice.speak("嗯？刚才没听清。", "curious");
    else voice.speak("我耳朵出了点问题。", "anxious");
    return "";
  }
  // F8 唤醒词门（软件级）：配置了唤醒词且非手动按键模式时，
  // 说出的话必须含唤醒词才应答，否则当没听见（VAD 误触发率大幅下降）
  String wakeWord(cfg.s.wakeWord);
  if (wakeWord.length() && !g_manualOnce && heard.indexOf(wakeWord) < 0) {
    Serial.printf("[VOICE] 未含唤醒词「%s」，忽略\n", wakeWord.c_str());
    lastError = "无唤醒词";
    metrics.wakeReject();
    _cooldownMs = millis() + 3000;              // 3 秒冷却防连环误触发
    return "";
  }
  g_manualOnce = false;
  return respond(heard);
}

// 应答段：主程序全流程 + "这句该不该由我念"的判断（converse 与 /voicetest 共用）
String LaapVoice::respond(const String& heard) {
  String reply = laapInteractSearch(heard); // 主程序: 需求/记忆/搜索/LLM 全流程
  const char* expr = laapLastExpr();
  // 说得刚刚好一次：
  // ① 本地直答（工具指令/看东西）在上面那步里已经念过了 → 这里不能再念（用户实测"回答两次"）；
  // ② 受理回执"……"没有可说的内容，真回复由后台 llmHarvest 念出来（原来这里会把"……"
  //    也丢给 TTS 跑一趟，白等一次 TLS 握手，还可能蹦出奇怪的音）。
  bool already = laapReplySpoken();
  bool pending = laapChatPending();
  if (!already && !pending) speak(reply, expr);
  Serial.printf("[VOICE] 应答「%s」→ %s\n", reply.substring(0, 24).c_str(),
                already ? "内部已念过，这里不再念" : (pending ? "异步受理，等后台念" : "由这里念"));
  return reply;
}

void LaapVoice::setVadPaused(bool paused) {
  if (_vadPaused == paused) return;
  _vadPaused = paused;
  Serial.printf("[VOICE] 自动聆听 %s\n", paused ? "已暂停（BOOT 短按恢复）" : "恢复中");
  // 状态点由 loopTick 统一维护（暂停即灭），这里不直接画，避免与 loopTick 打架
}

void LaapVoice::loopTick() {
  if (!_ready) return;
  bool vadMode = ((VoiceMode)cfg.s.voiceMode == VoiceMode::Vad);
  // 聆听点只在"确实在收声"时亮：暂停 / 说话中 / 播报冷却期都算不在听。
  // （原来 按键模式 也会画一个灰点常驻，看着像一直在录）
  bool wantListening = vadMode && !_vadPaused && !_busy && ((int32_t)(millis() - _cooldownMs) >= 0);
  static bool s_lastDrawn = false;
  if (wantListening != s_lastDrawn) {
    display.drawListenState(wantListening);
    s_lastDrawn = wantListening;
  }
  if (_busy) return;
  // 音频泵所有模式常转：预滚缓冲靠它持续喂数（按键/暂停/冷却期也要滚，只是不触发）
  audio.recordTick();
  // ASR 预热：仅"上次对话后 5 分钟内"的空闲期进行，且已有活连接就跳过——
  // 防止长期无人时高频握手（服务商 WAF 可能盯上陌生 TLS 风暴）
  static uint32_t s_warmMs = 0;
  static uint32_t s_lastChatMs = 0;
  if (_busy) s_lastChatMs = millis();
  bool chatRecent = (s_lastChatMs != 0) && (millis() - s_lastChatMs < 300000);
  if (vadMode && !_vadPaused && chatRecent && (int32_t)(millis() - _cooldownMs) >= 0 &&
      millis() - s_warmMs > 20000) {
    if (asr.warmAlive()) {
      s_warmMs = millis();                    // 热连接还活着：不重握手
    } else {
      s_warmMs = millis();
      if (xTaskCreate([](void*) { asr.warmup(); vTaskDelete(nullptr); },
                      "asrwarm", 8192, nullptr, 1, nullptr) != pdPASS) {  // TLS 握手栈峰值 6KB+，4K 会溢出
        asr.warmup();                         // 建任务失败：退化为主线程预热
      }
    }
  }
  if (!vadMode) return;
  if (_vadPaused) return;                               // 暂停：只排水（上面已泵）不触发
  if ((int32_t)(millis() - _cooldownMs) < 0) return;    // 回绕安全
  // 检测持续人声（>600ms 才开麦，避免误触发）
  if (audio.vadSpeaking()) {
    if (!_vadHold) { _vadHold = true; _vadHoldStart = millis(); }
    else if (millis() - _vadHoldStart > 600) {
      _vadHold = false;
      converse();
    }
  } else {
    _vadHold = false;
  }
}

// ============================================================
//  参数自调优（RSI⑥）：5 分钟窗口的带式控制器，纯 C 零 LLM 成本
//  只修"确凿变差"的方向，硬钳位防跑飞（最怕把 VAD 调聋——漏触发无法
//  从计数器观测，所以灵敏度只在小幅区间内浮动，且干净窗口自动回落）。
//  RAM 常驻：每次开机回默认值，坏参数不会跨重启固化。
// ============================================================
void LaapVoice::tuneTick() {
  static uint32_t s_lastRun = 0;
  static uint32_t s_cs = 0, s_ns = 0, s_wm = 0;   // 上次采样时的累计值（算窗口增量）
  static uint8_t s_cleanA = 0, s_cleanB = 0;
  if (!_ready) return;
  uint32_t now = millis();
  if (s_lastRun == 0) { s_lastRun = now; return; }
  if (now - s_lastRun < 300000UL) return;         // 5 分钟一评估
  s_lastRun = now;

  uint32_t dCs = metrics.cooldownSkip - s_cs; s_cs = metrics.cooldownSkip;
  uint32_t dNs = metrics.noSpeech     - s_ns; s_ns = metrics.noSpeech;
  uint32_t dWm = metrics.wakeMiss     - s_wm; s_wm = metrics.wakeMiss;

  // 旋钮 A：冷却期内仍被触发（回声/抢话）→ 拉长冷却；连续 2 窗干净 → 回缩
  if (dCs > 0 && _cooldownDur < 3000) {
    _cooldownDur += 300;
    Serial.printf("[TUNE] 播报冷却 %lu→%lu ms（本窗口 %lu 次冷却期触发）\n",
                  (unsigned long)(_cooldownDur - 300), (unsigned long)_cooldownDur, (unsigned long)dCs);
    s_cleanA = 0;
  } else if (dCs == 0 && _cooldownDur > 1200) {
    if (++s_cleanA >= 2) {
      _cooldownDur -= 300; s_cleanA = 0;
      Serial.printf("[TUNE] 连续干净，冷却回缩到 %lu ms\n", (unsigned long)_cooldownDur);
    }
  } else s_cleanA = 0;

  // 旋钮 B：VAD 触发了却没听到有效内容（环境噪声/回声误触发）→ 抬阈值；连续 3 窗干净 → 回落。
  // 上限 1.6：曾到 2.0 时远场人声全被埋（"要凑很近才理人"），宁可偶尔误触发也不能调聋
  uint32_t miss = dNs + dWm;
  if (miss >= 3 && _vadMul < 1.6f) {
    _vadMul += 0.15f;
    audio.setVadThresholdMul(_vadMul);
    Serial.printf("[TUNE] VAD 阈值 ×%.2f（本窗口 %lu 次『触发但没听清』）\n", _vadMul, (unsigned long)miss);
    s_cleanB = 0;
  } else if (miss == 0 && _vadMul > 1.0f) {
    if (++s_cleanB >= 3) {
      _vadMul -= 0.15f;
      audio.setVadThresholdMul(_vadMul);
      Serial.printf("[TUNE] 连续干净，VAD 阈值回落 ×%.2f\n", _vadMul);
      s_cleanB = 0;
    }
  } else s_cleanB = 0;
}

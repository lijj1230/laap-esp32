#include "laap_voice.h"
#include "laap_config.h"
#include "laap_audio.h"
#include "laap_edge_tts.h"
#include "laap_speech.h"
#include "laap_display.h"
#include "laap_web.h"
#include "laap_metrics.h"
#include <esp_heap_caps.h>   // 预热门控的堆余量判定（最大连续块）

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
  laapBlackBox("tts:start");   // v3.65 黑匣子：TTS 全链（WS-TLS+MP3 解码+I2S 播放）在此阻塞
  // v3.68 堆量护栏：TTS 要新建一条 WS-TLS 连接（握手峰值吃内部堆），最大连续块
  // 不足时握手深处分配失败会无声崩溃（v3.64-67 实证）——宁可不念，文字已上屏。
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16000) {
    Serial.printf("[VOICE] 内部堆最大块不足 16KB，跳过播报（文字已显示）\n");
    display.drawFace(expr ? expr : "calm", false);
    _cooldownMs = millis() + 600;
    return;
  }
  if (!_ready || (VoiceMode)cfg.s.voiceMode == VoiceMode::Off) return;
  if (!text.length()) return;
  // 栈水位护栏：TTS 要过一次 TLS 握手。单位注意——本 IDF 的 uxTaskGetStackHighWaterMark
  // 返回**字节**（task.h 原文 "in bytes (as opposed to words)"），旧代码按"字"×4，读数虚高
  // 4 倍、护栏实际是 2.6KB（v3.51 审计）。实测空闲余量约 4-5KB，阈值保持 2600 字节不变
  UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
  if (hw < 2600) {
    Serial.printf("[VOICE] 栈余量仅 %u 字节，跳过播报（防栈溢出重启；文字已显示）\n", (unsigned)hw);
    display.drawFace(expr ? expr : "calm", false);
    _cooldownMs = millis() + 600;
    return;
  }
  _busy = true;
  display.drawFace(expr ? expr : "calm", true); // 说话=思考眉

  bool ok = false;
  // 通道: 0=Edge→火山回退(默认) 1=仅Edge 2=仅火山 3=静音(不出声)
  // v3.73 情绪→语气：情绪词是控制信号，让"心情"以语气呈现而非词语——
  // tired 慢 8%、excited 快 10%、anxious 略快略赶、lonely 放缓。基础 rate 来自配置。
  if (cfg.s.ttsChannel == 0 || cfg.s.ttsChannel == 1) {
    int ratePct = cfg.s.ttsRate[0] ? atoi(cfg.s.ttsRate) : 0;   // 配置口径：如 "0" / "-10" / "+10"
    if (expr) {
      if      (!strcmp(expr, "tired"))   ratePct -= 8;
      else if (!strcmp(expr, "excited")) ratePct += 10;
      else if (!strcmp(expr, "anxious")) ratePct += 5;
      else if (!strcmp(expr, "lonely"))  ratePct -= 5;
      else if (!strcmp(expr, "happy"))   ratePct += 4;
    }
    if (ratePct > 50) ratePct = 50;
    if (ratePct < -50) ratePct = -50;
    String rateStr = (ratePct > 0 ? "+" : "") + String(ratePct) + "%";
    ok = edgeTts.speak(text, String(cfg.s.ttsVoice), rateStr);
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
  if (audio.interrupted()) metrics.interruptedPlay();   // 播放被人声/按键打断（barge-in）。
  // C3 打断事件源暂不接（v3.61 审计）：barge 默认关（自发声会误触发=自己吓自己），
  // 打开需先有回声抑制；在此之前 C3 快变量只认网页👎这一个事件源
  // 播报冷却：等回声消散，避免 VAD 自触发（时长由自调优旋钮 A 控制）。
  // 同时清预滚——自家 TTS 尾音绝不能留在预滚里，否则下一轮录音会把"自己说的话"交给 ASR
  _cooldownMs = millis() + _cooldownDur;
  audio.prerollFlush();
  _busy = false;
  { UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);   // v3.65：每轮播报后看 loop 栈余量（栈溢出曾疑）
    char bb[40];
    snprintf(bb, sizeof(bb), "tts:done hw=%u", (unsigned)hw);
    laapBlackBox(bb);
    if (hw < 4000) Serial.printf("[VOICE] 栈余量偏低: %uB（阈值 4000，观察项）\n", (unsigned)hw);
  }
}

bool LaapVoice::listenAndTranscribe(String& heard) {
  heard = "";
  if (!audio.micOk) { lastError = "无麦克风"; return false; }
  Serial.printf("[VOICE] 请说…（说完 %u ms 无声收音，最多 12s）\n", audio.vadStopMs());
  display.drawFace("curious", true);
  display.drawRecState(true);   // 右上角红点=录音中（说完自动熄）

  // 按键模式平时不泵，预滚缓冲里是陈年旧音——录前清掉，别回填进录音头（VAD 模式才靠预滚保句首）
  if ((VoiceMode)cfg.s.voiceMode != VoiceMode::Vad) audio.prerollFlush();
  if (!audio.recordStart(12)) { display.drawRecState(false); lastError = "录音缓冲分配失败"; return false; }
  uint32_t t0 = millis();
  // v3.73：起始静默 8s→3s（按键模式下主人刚按的键，正要开口，8s 白等是纯延迟）；
  // 说话中刷新等待窗（t0=now-1200ms：说话期间再给 1.2s+判停尾，说完快速收音）
  uint32_t waitCap = 3000UL + audio.vadStopMs();   // 起始静默 3s + 说完后的判停尾
  bool spoke = false;
  while (millis() - t0 < waitCap) {
    audio.recordTick();
    if (audio.vadSpeaking()) { spoke = true; t0 = millis() - 1200; } // 说话中刷新等待窗（说完快速收音）
    else if (spoke) break;                                            // 说完静音（判停窗口到）
    if (audio.recordBytes() >= 12UL * 16000 * 2 - 1024) break;
    delay(5);
  }
  size_t got = audio.recordBytes();
  audio.recordStop();
  display.drawRecState(false);
  audio.vadReset();   // 清 VAD 残留（v3.51）：容量截断退出时 _vadSpeech 仍为真，
                      // 冷却结束后会被判成"还在说话"再起一轮空对话（录 8s 静音→"没听清"）
  // 尾部静音裁剪：判停窗口默认 5s，不裁就白传几秒静音（上传量/识别延迟/计费秒数全翻倍）
  { const int16_t* p = audio.recordData();
    size_t samples = got / 2;
    const size_t win = 800;                          // 50ms @16k
    while (samples > 10 * win) {                     // 至少留 500ms 尾（防轻收尾字"了/吗"被裁）
      float e = 0;
      for (size_t i = samples - win; i < samples; i += 8) e += (float)p[i] * p[i];
      if (sqrtf(e / (win / 8)) > 260) break;         // 还有声音就住手（≈VAD 静默档阈值）
      samples -= win;
    }
    got = samples * 2;
  }
  if (!spoke || got < 8000) { metrics.noSpeechHeard(); lastError = "没听清"; return false; }

  String err;
  heard = asr.transcribe(audio.recordData(), got, err);
  _lastChatMs = millis();            // ASR 预热门控的时间基准（原来写在 loopTick 的 _busy 分支里=死代码）
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
  // 手动按键授权一次性消费：必须在**所有**早退之前（v3.51 审计：冷却早退曾把它留下，
  // 残留授权会让下一次 VAD 自动触发绕过唤醒词门）
  bool manualOnce = g_manualOnce;
  g_manualOnce = false;
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
  if (wakeWord.length() && !manualOnce && heard.indexOf(wakeWord) < 0) {
    Serial.printf("[VOICE] 未含唤醒词「%s」，忽略\n", wakeWord.c_str());
    lastError = "无唤醒词";
    metrics.wakeReject();
    _cooldownMs = millis() + 3000;              // 3 秒冷却防连环误触发
    return "";
  }
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
  // v3.73 ASR 预热：挪到 vadMode 早退**之前**——原位置在 `if (!vadMode) return` 之后，
  // 按键模式永远执行不到 → 按键对话每次都现场 TLS 握手（2~4s，按键延迟的主凶）。
  // 触发条件放宽：任何模式、上次交互 15 分钟内、堆最大块 ≥40KB 即预热（用户按键
  // 是确定性场景，握手要赶在说话期间完成）
  {
    static uint32_t s_warmMs = 0;
    bool chatRecent = _lastChatMs && (millis() - _lastChatMs < 900000);
    bool heapRoom = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 40000;
    if (chatRecent && heapRoom && (int32_t)(millis() - _cooldownMs) >= 0 &&
        millis() - s_warmMs > 20000) {
      if (asr.warmAlive()) {
        s_warmMs = millis();                    // 热连接还活着：不重握手
      } else {
        s_warmMs = millis();
        // 栈 12KB：TLS 握手峰值 6KB+，首次真实运行（v3.36 前是死代码）曾疑似栈紧崩溃
        if (xTaskCreate([](void*) { asr.warmup(); vTaskDelete(nullptr); },
                        "asrwarm", 12288, nullptr, 1, nullptr) != pdPASS) {
          asr.warmup();                         // 建任务失败：退化为主线程预热
        }
      }
    }
  }
  // 音频泵只在 VAD 模式常转（预滚缓冲+触发判定都靠它）。
  // 按键模式按下才录，平时不监测——v3.32 曾改成全模式常转，结果按键模式下
  // "听到人声=活动"的息屏信号被环境噪声反复点亮，屏幕永远息不了
  if (!vadMode) return;
  audio.recordTick();
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

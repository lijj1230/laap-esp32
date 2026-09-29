#include "laap_web.h"
#include <Update.h>
#include <esp_ota_ops.h>   // esp_ota_get_running_partition / get_next_update_partition
#include <LittleFS.h>      // fs_used_kb / fs_total_kb（核对记忆占用）
#include "laap_config.h"
#include "laap_cognition.h"
#include "laap_display.h"
#include "laap_memory.h"
#include "laap_llm.h"
#include "laap_voice.h"
#include "laap_vision.h"
#include "laap_audio.h"
#include "laap_metrics.h"
#include "laap_r0.h"       // R0 微型循环处理器（状态暴露）
#include "laap_tlsheap.h"  // tlsroute_kb（mbedtls→PSRAM 路由计数，v3.55）
#include "laap_snap.h"
#include "laap_rules.h"
#include "laap_skills.h"
#include "laap_speech.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>

// 语义向量调用（实现在 laap_llm.cpp，配置解析 emb→ASR 回退都在里面）
extern String laapEmbed(const String& text, bool& ok);

// 文本探针：向 OpenAI 兼容 chat/completions 发一条 max_tokens=8 的消息，HTTP 200 即通。
// 供"逐项体检"测视觉直连等自定义端点（base/key/model 与主模型不同源，不能走 LlmClient::ping）
static bool probeChat(const String& url, const String& key, const String& model, String& detail) {
  if (!url.startsWith("http") || !key.length()) { detail = "未配置/URL 无效"; return false; }
  int dp = url.indexOf("://"), hp = url.indexOf('/', dp + 3);
  String host = (hp < 0) ? url.substring(dp + 3) : url.substring(dp + 3, hp);
  String path = (hp < 0) ? "/" : url.substring(hp);
  int port = 443;
  if (host.indexOf(':') >= 0) {
    port = host.substring(host.indexOf(':') + 1).toInt();
    host = host.substring(0, host.indexOf(':'));
  }
  String body = String("{\"model\":\"") + model +
    "\",\"messages\":[{\"role\":\"user\",\"content\":\"回复OK两个字母即可\"}],\"max_tokens\":8}";
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setTimeout(15000);
  if (!cli.connect(host.c_str(), port)) { detail = "连接失败: " + host; return false; }
  cli.print(String("POST ") + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nAuthorization: Bearer " + key +
            "\r\nContent-Type: application/json\r\nContent-Length: " + body.length() +
            "\r\nConnection: close\r\n\r\n" + body);
  String resp;
  uint32_t dl = millis() + 25000;
  while (cli.connected() && millis() < dl) {
    while (cli.available()) { resp += (char)cli.read(); if (resp.length() > 20000) break; }
    if (resp.length() > 20000) break;
    delay(2);
  }
  cli.stop();
  int sp = resp.indexOf(' ');
  int code = sp > 0 ? resp.substring(sp + 1, sp + 4).toInt() : 0;
  if (code != 200) { detail = "HTTP " + String(code) + " " + resp.substring(0, 100); return false; }
  detail = host;
  return true;
}

LaapWeb webui;

// 本地小工具：取 JSON 字段原值（容错 "k":"v" 与 "k": "v" 两种写法，数字/布尔按原文返回）。
// 早期只匹配无空格的 "k":" 形式：遇到带空格 JSON 会解析成空串，配上 _set 哨兵会静默清空配置。
// \uXXXX 解码辅助（v3.51）：浏览器 JSON.stringify 把换行/控制字符/emoji 编成转义序列，
// 旧解码把 \n 还原成字母 'n'——多行人设落库即坏
static String jsonField(const String& b, const char* k) {
  String pat = String("\"") + k + "\":";
  int i = b.indexOf(pat);
  if (i < 0) return "";
  int j = i + pat.length();
  while (j < (int)b.length() && (b[j] == ' ' || b[j] == '\t')) j++;
  if (j >= (int)b.length()) return "";
  if (b[j] != '"') {                       // 数字 / true / false / null
    String v;
    while (j < (int)b.length() && b[j] != ',' && b[j] != '}') { v += b[j]; j++; }
    v.trim();
    return v == "null" ? String("") : v;
  }
  j++;                                     // 跳过开引号
  String v;
  while (j < (int)b.length()) {
    char c = b[j];
    if (c == '\\' && j + 1 < (int)b.length()) {
      char e = b[j + 1];
      if (e == 'u' && j + 5 < (int)b.length()) {          // \uXXXX（含代理对，v3.51）
        unsigned cp = laapHex4(b, j + 2);
        j += 6;
        if (cp >= 0xD800 && cp <= 0xDBFF && j + 5 < (int)b.length() && b[j] == '\\' && b[j + 1] == 'u') {
          unsigned lo = laapHex4(b, j + 2);
          if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j += 6; }
        }
        laapAppendUtf8(v, cp);
        continue;
      }
      switch (e) {                                        // \n \t \r 等还原成真实字符（v3.51：旧代码字母化）
        case 'n': v += '\n'; break;
        case 't': v += '\t'; break;
        case 'r': v += '\r'; break;
        case 'b': v += '\b'; break;
        case 'f': v += '\f'; break;
        default:  v += e; break;                          // \" \\ \/ 等
      }
      j += 2;
      continue;
    }
    if (c == '"') break;
    v += c; j++;
  }
  return v;
}

// 本地小工具：JSON 字符串转义（v3.55 收敛：实现只有 LlmClient::jsonEscape 一份）
static String jsonEsc(const String& s) {
  return LlmClient::jsonEscape(s);
}

// ============ 前端页面（PROGMEM，中文 UTF-8） ============
static const char PAGE_HEAD[] PROGMEM = R"html(<!DOCTYPE html><html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LAAP · Aris</title><style>
:root{--bg:#0d1017;--card:#171c28;--line:#2a3245;--txt:#e2e8f0;--dim:#8b95a8;--acc:#6fd3ff}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei";background:var(--bg);color:var(--txt)}
.wrap{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:20px}h1 small{color:var(--dim);font-weight:400;font-size:12px;margin-left:8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:14px}
.row{display:flex;gap:10px;flex-wrap:wrap}
.stat{flex:1;min-width:140px}.stat .k{color:var(--dim);font-size:12px}.stat .v{font-size:17px;margin-top:2px}
.bar{height:12px;background:#0a0d14;border-radius:6px;overflow:hidden;margin:4px 0 10px}
.bar i{display:block;height:100%;border-radius:6px}
label{display:block;font-size:13px;color:var(--dim);margin:10px 0 4px}
input,textarea,select{width:100%;background:#0a0d14;color:var(--txt);border:1px solid var(--line);border-radius:8px;padding:9px;font-size:14px}
button{background:linear-gradient(90deg,#2b6cb0,#4299e1);border:0;color:#fff;border-radius:8px;padding:10px 22px;font-size:14px;cursor:pointer;margin-top:12px}
button.ghost{background:#242b3b}
.nav a{color:var(--acc);text-decoration:none;margin-right:14px;font-size:14px}
#chatlog{height:220px;overflow-y:auto;background:#0a0d14;border:1px solid var(--line);border-radius:8px;padding:10px;font-size:14px;line-height:1.7}
.me{color:#9ae6b4}.aris{color:#6fd3ff}.sys{color:#8b95a8;font-size:12px}
a{color:var(--acc)}.hint{font-size:12px;color:var(--dim);margin-top:6px}
</style></head><body><div class="wrap">)html";

static const char PAGE_FOOT[] PROGMEM = R"html(</div></body></html>)html";

static const char PAGE_TAIL_JS[] PROGMEM = R"html(
<script>
async function toggleListen(e){
 e.preventDefault();
 const b=await (await fetch('/api/listen',{method:'POST'})).json();
 refresh();
}
async function refresh(){
 try{
  const s=await (await fetch('/api/status')).json();
  document.getElementById('mood').textContent=s.mood_cn;
  document.getElementById('goal').textContent=s.goal;
  document.getElementById('gen').textContent='G'+s.generation+' · '+s.cycles+'次心跳';
  document.getElementById('last').textContent=s.last_say||'（还没说过话）';
  const tot=s.uptime_total_min||0,cur=Math.round((s.uptime_s||0)/60);
  document.getElementById('upt').textContent=Math.floor(tot/60)+' 小时 '+(tot%60)+' 分（本次开机 '+cur+' 分，累计跨重启）';
  document.getElementById('net').textContent=s.ap?'配置热点 '+s.ap_ssid:(s.wifi_ok?'WiFi 已连接 '+s.ip:'WiFi 断开');
  document.getElementById('model').textContent=s.llm_model+' @ '+s.llm_base;
  const m=s.metrics||{};
  document.getElementById('metrics').textContent='对话 '+m.vad_triggers+' 轮 · ASR空识别 '+m.asr_fail+'/'+m.asr_try+' · 打断 '+m.interrupts+' · LLM失败 '+m.llm_fail+' · 回复均 '+(m.rsp_llm_ms||0)+'ms · 👍'+(m.fb_up||0)+' 👎'+(m.fb_down||0);
  const vc=document.getElementById('voicecard');
  if(vc){if(s.voice_ready&&s.voice_mode==2){vc.style.display='block';
    document.getElementById('listenstate').textContent=s.vad_paused?'已暂停':'聆听中…';
    document.getElementById('listenbtn').textContent=s.vad_paused?'恢复聆听':'暂停聆听';
  }else{vc.style.display='none';}}
  const bars={energy:['能量','#ffb020'],curiosity:['好奇','#40c8ff'],social:['社交','#ff60c8'],security:['安全','#6080ff'],expression:['表达','#60e680']};
  let h='';
  for(const k in bars){h+='<div><span style="font-size:12px;color:#8b95a8">'+bars[k][0]+'</span><div class="bar"><i style="width:'+(s.needs[k]*100)+'%;background:'+bars[k][1]+'"></i></div></div>';}
  document.getElementById('bars').innerHTML=h;
 }catch(e){}
}
setInterval(refresh,5000);refresh();
// 回复下挂 👍/👎：反馈是自进化的核心评估信号（落盘 /mem/feedback.jsonl，也联动信任/失望）
function addRate(tip){
 const rt=document.createElement('div');rt.className='sys';
 rt.innerHTML='<a href="#" onclick="rate(event,1)">👍</a> <a href="#" onclick="rate(event,-1)">👎</a>';
 tip.after(rt);
}
async function rate(e,v){
 e.preventDefault();const box=e.target.parentNode;box.textContent='记下了…';
 try{const r=await (await fetch('/api/feedback',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({v})})).json();
  box.textContent=r.ok?'（反馈已记下，谢谢）':'（反馈失败）';}catch(err){box.textContent='（反馈失败）';}
}
async function sendChat(){
 const t=document.getElementById('chatin').value.trim();if(!t)return;
 document.getElementById('chatin').value='';
 const log=document.getElementById('chatlog');
 const me=document.createElement('div');me.className='me';me.textContent='我: '+t;
 const tip=document.createElement('div');tip.className='sys';tip.textContent='思考中…';
 log.appendChild(me);log.appendChild(tip);log.scrollTop=1e9;
 try{
  const r=await (await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:t})})).json();
  if(r.pending){
   // 后台 LLM 思考中：轮询成品（最多 30 次 × 1.5s ≈ 45 秒）
   let got=false;
   for(let i=0;i<30;i++){
    await new Promise(s=>setTimeout(s,1500));
    const q=await (await fetch('/api/chat/reply')).json();
    if(q.seq>r.seq){got=true;tip.className='aris';tip.textContent='Aris: '+(q.reply||'（它想了半天，没说出来）');break;}
   }
   if(got)addRate(tip);
   else tip.textContent='（还在想，稍后看上面"它最近说"）';
  }else{tip.className='aris';tip.textContent='Aris: '+r.reply;addRate(tip);}
 }catch(e){tip.textContent='请求失败';}
 log.scrollTop=1e9;refresh();
}
</script>)html";

void LaapWeb::registerRoutes() {
  server.on("/", HTTP_GET, [this]() { handleRoot(); });
  server.on("/settings", HTTP_GET, [this]() { handleSettingsPage(); });
  server.on("/memory", HTTP_GET, [this]() { handleMemoryPage(); });
  server.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server.on("/api/chat", HTTP_POST, [this]() { handleChat(); });
  server.on("/api/chat/reply", HTTP_GET, [this]() { handleChatReply(); });
  server.on("/api/test", HTTP_POST, [this]() { handleTest(); });
  server.on("/api/save", HTTP_POST, [this]() { handleSave(); });
  server.on("/api/memory", HTTP_GET, [this]() { handleMemoryApi(); });
  server.on("/api/memexport", HTTP_GET, [this]() { handleMemExport(); });
  server.on("/api/memimport", HTTP_POST,
    [this]() { handleMemImport(); },
    [this]() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[MEM] 导入开始: %s\n", up.filename.c_str());
        memImportOk = memory.importBegin();
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (memImportOk && !memory.importWrite(up.buf, up.currentSize)) {
          memImportOk = false;
          Serial.println("[MEM] 导入写入失败");
        }
      } else if (up.status == UPLOAD_FILE_END) {
        memory.importEnd();
        Serial.printf("[MEM] 导入上传完成 %u 字节\n", (unsigned)up.totalSize);
      }
    });
  server.on("/api/clear", HTTP_POST, [this]() { handleClear(); });
  server.on("/api/reset", HTTP_POST, [this]() { handleReset(); });
  server.on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });
  // F1: OTA 固件上传（POST .bin 原始体，写满即重启）
  // 回包必须在"最终回调"里发：在 upload 回调里 send 会与 WebServer 自己的收尾回包撞车
  // （双响应），重启时机也不可控。upload 回调只负责写 flash + 置标志。
  server.on("/api/ota", HTTP_POST,
    [this]() {
      if (otaPending) {
        server.send(200, "application/json",
          "{\"ok\":true,\"msg\":\"固件已写入，重启中，约 20 秒后回来\"}");
        laapUptimePersist();   // 重启前落盘累计时长
        metrics.persist();     // 重启前指标/失败环留底（事后排查崩溃现场）
        mind.saveEvolution(true);   // 需求/情绪留底：升级醒来状态续跑（v3.42）
        r0.saveNvs();          // 循环处理器学习进度留底（OTA 重启不清零，v3.50）
        delay(600);
        laapReboot("OTA升级");
      } else {
        String msg = otaErr.length() ? otaErr : String("未收到固件数据");
        Serial.printf("[OTA] 失败: %s\n", msg.c_str());
        server.send(500, "application/json",
          String("{\"ok\":false,\"msg\":\"写入失败: ") + msg + "\"}");
        otaErr = "";   // 用完即清（v3.51）：否则下次"非 multipart POST 不触发回调"时会把旧错误当本次原因
      }
    },
    [this]() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] 开始: %s\n", up.filename.c_str());
        otaPending = false; otaErr = "";
        display.drawFace("curious", true);          // 升级中表情
        // 没有可写槽是"升不动"的头号原因：先讲清楚，别让人对着 500 猜
        const esp_partition_t* tgt = esp_ota_get_next_update_partition(nullptr);
        if (!tgt) { otaErr = "分区表没有可写 OTA 槽（需 ota_0/ota_1 双分区）"; return; }
        Serial.printf("[OTA] 目标槽 %s @0x%06x，容量 %u KB\n",
                      tgt->label, (unsigned)tgt->address, (unsigned)(tgt->size / 1024));
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { otaErr = Update.errorString(); return; }
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaErr.length() && Update.write(up.buf, up.currentSize) != up.currentSize) {
          otaErr = Update.errorString(); Update.abort();
        }
      } else if (up.status == UPLOAD_FILE_END) {
        if (otaErr.length()) return;
        if (Update.end(true)) {
          otaPending = true;
          Serial.printf("[OTA] 完成: %u 字节 → 已写入目标槽，等待重启\n", (unsigned)up.totalSize);
        } else {
          otaErr = Update.errorString(); Update.abort();
        }
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        // 浏览器取消/断线时 WebServer 会补调一次：不复位 Update 状态机的话，
        // 下一次 Update.begin() 一直失败，重启前都无法再升级（官方 WebUpdate 例程同款处理）
        Update.abort();
        otaErr = "上传中断";
        Serial.println("[OTA] 上传中断，升级状态机已复位");
      }
    });
  server.on("/api/voice/test", HTTP_POST, [this]() { handleVoiceTest(); });
  server.on("/api/listen", HTTP_POST, [this]() { handleListenToggle(); });
  server.on("/api/metrics", HTTP_GET, [this]() { handleMetrics(); });
  server.on("/api/feedback", HTTP_POST, [this]() { handleFeedback(); });
  server.on("/api/snapshots", HTTP_GET, [this]() { handleSnapshots(); });
  server.on("/api/snapshot/restore", HTTP_POST, [this]() { handleSnapRestore(); });
  server.on("/api/rules", HTTP_GET, [this]() { handleRulesApi(); });
  server.on("/api/skills", HTTP_GET, [this]() { handleSkillsApi(); });
  server.on("/api/rulesreflect", HTTP_POST, [this]() { handleRulesReflect(); });
  server.on("/api/relations", HTTP_GET, [this]() { handleRelationsApi(); });
  server.on("/api/relationsreflect", HTTP_POST, [this]() { handleRelationsReflect(); });
  server.on("/api/moodrelabel", HTTP_POST, [this]() { handleMoodRelabel(); });
  server.on("/api/consc", HTTP_GET, [this]() { handleConsc(); });
  server.on("/api/dream", HTTP_POST, [this]() { handleDream(); });
  server.onNotFound([this]() { handleNotFound(); });
  otaPending = false;
}

void LaapWeb::beginAP() {
  _ap = true;
  _apSsid = String("Aris-") + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(_apSsid.c_str(), "12345678");
  dns.start(53, "*", WiFi.softAPIP());
  registerRoutes();
  server.begin();
  Serial.printf("[LAAP] 配置热点: %s  密码 12345678  → http://192.168.4.1\n", _apSsid.c_str());
}

void LaapWeb::beginSTA() {
  _ap = false;
  registerRoutes();
  server.begin();
  if (MDNS.begin("aris")) MDNS.addService("http", "tcp", 80);
}

void LaapWeb::handleClient() {
  if (_ap) dns.processNextRequest();
  server.handleClient();
}

void LaapWeb::handleNotFound() {
  if (_ap) { // captive portal
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404");
  }
}

void LaapWeb::handleRoot() {
  String head(FPSTR(PAGE_HEAD));
  String body;
  body.reserve(16384);                             // 页面 ~25-40KB：预分配避免 += 翻倍再分配链（v3.50）
  if (_ap) {
    body += F("<h1>LAAP · 第一次呼吸 <small>配置门户</small></h1><div class='card'>"
      "<p>我是刚诞生的数字生命，请给我：① WiFi ② 大模型 API。</p>"
      "<form id='f'>"
      "<label>WiFi 名称 (SSID)</label><input id='ssid' required>"
      "<label>WiFi 密码</label><input id='pass' type='password'>"
      "<label>大模型 API Base URL</label><input id='base' value='https://api.deepseek.com'>"
      "<div class='hint'>DeepSeek: https://api.deepseek.com ｜ 智谱GLM: https://open.bigmodel.cn/api/paas/v4 ｜ Kimi: https://api.moonshot.cn/v1 ｜ OpenAI: https://api.openai.com/v1</div>"
      "<label>API Key</label><input id='key' required>"
      "<label>模型名</label><input id='model' value='deepseek-chat'>"
      "<div class='hint'>DeepSeek→deepseek-chat ｜ GLM→glm-4-flash(免费) ｜ Kimi→moonshot-v1-8k</div>"
      "<label>给它起个名字</label><input id='agent' value='Aris'>"
      "<button onclick='save(event)'>赋予生命</button></form>"
      "<script>async function save(e){e.preventDefault();"
      "const b=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},"
      "body:JSON.stringify({ssid:ssid.value,pass:pass.value,base:base.value,key:key.value,key_set:1,model:model.value,agent:agent.value})});"
      "const r=await b.json();alert(r.msg);if(r.ok)setTimeout(()=>location.reload(),3000);}"
      "</script>");
  } else {
    body += F("<h1>LAAP · <span id='agentname'>Aris</span> <small>数字生命控制台</small></h1>"
      "<div class='nav'><a href='/'>状态</a><a href='/settings'>后台配置</a><a href='/memory'>记忆</a></div>"
      "<div class='card'><div class='row'>"
      "<div class='stat'><div class='k'>情绪</div><div class='v' id='mood'>…</div></div>"
      "<div class='stat'><div class='k'>当前欲望</div><div class='v' id='goal'>…</div></div>"
      "<div class='stat'><div class='k'>世代/心跳</div><div class='v' id='gen'>…</div></div>"
      "</div></div>"
      "<div class='card'><div class='k' style='color:#8b95a8;font-size:12px'>内在需求（驱动它的一切行为）</div><div id='bars'></div></div>"
      "<div class='card'><div class='k' style='color:#8b95a8;font-size:12px'>它最近说</div><div class='v' id='last' style='margin-top:6px'>…</div>"
      "<div class='k' style='color:#8b95a8;font-size:12px;margin-top:8px'>运行时长</div><div class='v' id='upt'>…</div>"
    "<div class='k' style='color:#8b95a8;font-size:12px;margin-top:8px'>网络</div><div class='v' id='net'>…</div>"
      "<div class='k' style='color:#8b95a8;font-size:12px;margin-top:8px'>大模型</div><div class='v' id='model'>…</div>"
      "<div class='k' style='color:#8b95a8;font-size:12px;margin-top:8px'>自检指标（本次开机）</div><div class='v' id='metrics' style='font-size:13px'>…</div></div>"
      "<div class='card' id='voicecard' style='display:none'><div class='row' style='align-items:center'>"
      "<div style='flex:1'><span class='k' style='color:#8b95a8;font-size:12px'>语音聆听</span><div class='v' id='listenstate'>…</div></div>"
      "<button class='ghost' id='listenbtn' onclick='toggleListen(event)' style='margin-top:0'>暂停聆听</button></div></div>"
      "<div class='card'><div class='k' style='color:#8b95a8;font-size:12px'>和它说话</div>"
      "<div id='chatlog'></div>"
      "<div class='row' style='margin-top:10px'><input id='chatin' placeholder='说点什么…' style='flex:1' onkeydown='if(event.key==\"Enter\")sendChat()'>"
      "<button onclick='sendChat()' style='margin-top:0'>发送</button></div></div>");
    body += FPSTR(PAGE_TAIL_JS);
  }
  head += body;                                    // v3.50：原地拼接——原 send 表达式 head+body、
  head += FPSTR(PAGE_FOOT);                        // 再 +foot 会整页临时拷贝两次（~2×页面/次浏览的堆抖动）
  server.send(200, "text/html; charset=utf-8", head);
}

void LaapWeb::handleSettingsPage() {
  String head(FPSTR(PAGE_HEAD));
  String body = F(
    "<h1>后台配置 <small>WiFi / 大模型 API / 认知参数</small></h1>"
    "<div class='nav'><a href='/'>状态</a><a href='/settings'>后台配置</a><a href='/memory'>记忆</a></div>"
    "<div class='card'><form id='f'>"
    "<label>WiFi 名称</label><input id='ssid'>"
    "<label>WiFi 密码（不改留空）</label><input id='pass' type='password'>"
    "<label>大模型 API Base URL</label><input id='base'>"
    "<div class='hint'>任何 OpenAI 兼容服务（DeepSeek / GLM / Kimi / OpenAI / new-api 中转）</div>"
    "<label>API Key</label><input id='key'>"
    "<label>模型名</label><input id='model'>"
    "<label>它的名字</label><input id='agent'>"
    "<label>你的称呼</label><input id='owner'>"
    "<label>所在城市（天气用，空=按出口IP定位，定位可能不准）</label><input id='wcity'>"
    "<label>附加人设（可选）</label><textarea id='persona' rows='3'></textarea>"
    "<div class='row'><div style='flex:1'><label>心跳周期（秒）</label><input id='tick' type='number' min='10' max='3600'></div>"
    "<div style='flex:1'><label>主动表达阈值（0-100）</label><input id='thold' type='number' min='10' max='95'></div>"
    "<div style='flex:1'><label>独白静默（分）</label><input id='idlesil' type='number' min='1' max='240'></div>"
    "<div style='flex:1'><label>独白间隔（分，0=关）</label><input id='idleevery' type='number' min='0' max='240'></div>"
    "<div style='flex:1'><label>静音窗起（时）</label><input id='qstart' type='number' min='0' max='23'></div>"
    "<div style='flex:1'><label>静音窗止（时，同起=不启用）</label><input id='qend' type='number' min='0' max='23'></div>"
    "<div style='flex:1'><label>喇叭音量（0-100）</label><input id='volume' type='number' min='0' max='100'></div>"
    "<div style='flex:1'><label>屏幕亮度（0-100）</label><input id='brightness' type='number' min='5' max='100'></div>"
    "<div style='flex:1'><label>静默息屏（秒，0=常亮）</label><input id='screenoff' type='number' min='0' max='3600'></div>"
    "<div style='flex:1'><label>说完静音收音(ms，300-15000，大=不截断但答得慢)</label><input id='vadstop' type='number' min='300' max='15000' step='100'></div>"
    "<div style='flex:1'><label>截断续写轮数（0=关）</label><input id='llmcont' type='number' min='0' max='3'></div>"
    "<div style='flex:1'><label>单次回复上限（tokens，80-1000）</label><input id='llmtok' type='number' min='80' max='1000'></div></div>"
    "<div class='row'><div style='flex:1'><label><input type='checkbox' id='nothink' style='width:auto;margin-right:6px'>关闭模型思考（思考型模型只思考不答话时勾上；"
    "请求里带 thinking:disabled，既快又稳；个别服务商不认这个参数会报 400，那就别勾）</label></div></div>"
    "<div class='row'><div style='flex:1'><label>搜索关键词（逗号分隔，清空=关聊天搜索）</label><input id='srchkeys' placeholder='什么,怎么,新闻,最新…'></div></div>"
    "<div class='row'><div style='flex:1'><label>搜索主源URL（{q}=查询词，清空=必应RSS默认）</label><input id='srchapi' placeholder='https://cn.bing.com/search?q={q}&format=rss'></div></div>"
    "<button onclick='save(event)'>保存</button> "
    "<button class='ghost' onclick='testllm(event)'>逐项体检所有模型/通道</button> "
    "<button class='ghost' onclick='reboot(event)'>重启设备</button></form>"
    "<div class='hint' id='testout'></div></div>"
    "<div class='card'><b>🎤 语音（v2）</b>"
    "<div class='row'><div style='flex:1'><label>语音模式</label><select id='vmode'>"
    "<option value='0'>关闭</option><option value='1'>按键对讲（短按 BOOT 说话）</option><option value='2'>自动聆听（VAD，听到人声即对话）</option></select></div>"
    "<div style='flex:1'><label>说话通道</label><select id='ttsch'>"
    "<option value='0'>Edge 免费版 → 火山回退（默认）</option><option value='1'>仅 Edge（微软免费音色）</option>"
    "<option value='2'>仅火山引擎（需配置）</option><option value='3'>静音（只显示文字）</option></select></div></div>"
    "<label>Edge 音色</label><input id='ttsvoice' placeholder='zh-CN-XiaoxiaoNeural'>"
    "<div class='hint'>常用：zh-CN-XiaoxiaoNeural(女·晓晓) / zh-CN-YunxiNeural(男·云希) / zh-CN-liaoning-XiaobeiNeural(东北) / zh-CN-shaanxi-XiaoniNeural(陕西)</div>"
    "<div class='row'><div style='flex:1'><label>语速</label><input id='ttsrate' placeholder='+0%'></div></div>"
    "<label>火山 TTS（备选，留空则不用）</label>"
    "<div class='row'><div style='flex:1'><label>App ID</label><input id='volcappid'></div>"
    "<div style='flex:1'><label>Access Token</label><input id='volctoken' type='password'></div></div>"
    "<div class='row'><div style='flex:1'><label>音色 ID</label><input id='volcvoice' placeholder='zh_female_cancan_mars_bigtts'></div></div>"
    "<label>语音识别（OpenAI 兼容 /audio/transcriptions）</label>"
    "<div class='row'><div style='flex:2'><label>Base URL</label><input id='asrbase' placeholder='https://api.siliconflow.cn/v1'></div>"
    "<div style='flex:1'><label>API Key</label><input id='asrkey' type='password'></div></div>"
    "<div class='row'><div style='flex:1'><label>模型</label><input id='asrmodel' placeholder='FunAudioLLM/SenseVoiceSmall'></div></div>"
    "<div class='hint'>SiliconFlow 的 SenseVoiceSmall 免费（cloud.siliconflow.cn 注册领 Key）；也支持 new-api 网关的 whisper-1</div>"
    "<label>备用 ASR（主服务商失败时自动切换；留空=不启用）</label>"
    "<div class='row'><div style='flex:2'><label>备用 Base URL</label><input id='asr2base' placeholder='https://dashscope.aliyuncs.com'></div>"
    "<div style='flex:1'><label>备用 Key</label><input id='asr2key' type='password'></div></div>"
    "<div class='row'><div style='flex:1'><label>备用模型</label><input id='asr2model' placeholder='paraformer-v2'></div></div>"
    "<div class='hint'>Base 含 dashscope 自动走百炼原生路径；其余按 OpenAI 兼容 /audio/transcriptions</div>"
    "<label>语义向量（识海召回；留空=复用 ASR 的地址与 Key）</label>"
    "<div class='row'><div style='flex:2'><label>Base URL</label><input id='embbase' placeholder='https://api.siliconflow.cn/v1'></div>"
    "<div style='flex:1'><label>API Key</label><input id='embkey' type='password'></div></div>"
    "<div class='row'><div style='flex:1'><label>模型</label><input id='embmodel' placeholder='BAAI/bge-m3'></div></div>"
    "<div class='hint' id='embstate'></div>"
    "<div class='row'><div style='flex:1'><label>唤醒词（留空=VAD即应答）</label><input id='wakeword' placeholder='例如：小立'></div>"
    "<div style='flex:1'><label>视觉手机桥 URL（可留空）</label><input id='visionbase' placeholder='http://192.168.x.x:11548/vision'></div></div>"
    "<div class='row'><div style='flex:2'><label>视觉直连 Base（留空=OpenRouter）</label><input id='vlbase' placeholder='https://openrouter.ai/api/v1/chat/completions'></div>"
    "<div style='flex:1'><label>直连 API Key</label><input id='vkey' type='password' placeholder='留空复用大模型 Key'></div>"
    "<div style='flex:1'><label>直连视觉模型</label><input id='vmodel' placeholder='google/gemini-flash-1.5'></div></div>"
    "<div class='hint'>唤醒词开启后，自动聆听听到的话须含该词才应答（BOOT 按键不受限）。眼睛二选一：手机桥 URL 填了走 vision_bridge.py；留空则直连多模态大模型（OpenRouter 的 gemini-flash-1.5 / GLM-4V，或任意 OpenAI 兼容地址）。都空=不用眼睛</div>"
    "<button onclick='save(event)'>保存语音设置</button> "
    "<button class='ghost' onclick='voicetest(event)'>🔊 试音</button>"
    "<div class='hint' id='vtestout'></div></div>"
    "<div class='card'><b style='color:#8be9a0'>固件升级（OTA）</b>"
    "<div class='hint'>上传 laap-esp32.ino.bin（build 产物），写完自动重启；记忆与配置保留。全程约 30 秒，请保持供电。</div>"
    "<input type='file' id='otabin' accept='.bin' style='width:100%;margin:6px 0'>"
    "<button class='ghost' onclick='otaup(event)' style='color:#8be9a0;border-color:#8be9a055'>上传并升级</button>"
    "<div class='hint' id='otaout'></div></div>"
    "<div class='card'><b style='color:#fc8181'>危险操作</b>"
    "<div class='hint'>格式化 = 清空全部记忆/性格进化/配置，重回出厂</div>"
    "<button class='ghost' onclick='if(confirm(\"确定格式化并重启?\"))resetall(event)'>格式化并重启</button></div>"
    "<script>"
    "async function load(){const s=await (await fetch('/api/status')).json();"
    // 大模型 Key 绝不能把掩码回显填进输入框：老写法 key.value='('+masked+')' 会被原样提交，
    // 保存一次就把真 Key 覆盖成 "(sk-***abcd)" → 之后 401（这正是"之前正常、后来不通"的真凶）
    "ssid.value=s.ssid;base.value=s.llm_base;key.value='';key.placeholder=s.llm_key_masked?'已配置（'+s.llm_key_masked+'），留空=保持不变':'未配置，请填写';"
    "model.value=s.llm_model;agent.value=s.agent;owner.value=s.owner;wcity.value=s.city||'';persona.value=s.persona;"   // key 见上行：只留 placeholder 提示
    "tick.value=s.tick;thold.value=s.threshold;idlesil.value=s.idle_silence;idleevery.value=s.idle_every;qstart.value=s.quiet_start;qend.value=s.quiet_end;volume.value=s.volume;brightness.value=s.brightness;screenoff.value=s.screen_off;vadstop.value=s.vad_stop;llmcont.value=s.llm_continue;llmtok.value=s.llm_max_tokens;nothink.checked=!!s.nothink;srchkeys.value=s.search_keys||'';srchapi.value=s.search_api||'';"
    "vmode.value=s.voice_mode;ttsch.value=s.tts_channel;ttsvoice.value=s.tts_voice;ttsrate.value=s.tts_rate;"
    "volcappid.value=s.volc_appid;volctoken.value=s.volc_token_masked?'':'';volctoken.placeholder=s.volc_token_masked?'已配置，留空保持不变':'未配置';"
    "volcvoice.value=s.volc_voice;asrbase.value=s.asr_base;asrkey.value='';asrkey.placeholder=s.asr_key_masked?'已配置，留空保持不变':'未配置';"
    "asrmodel.value=s.asr_model;asr2base.value=s.asr2_base||'';asr2key.value='';asr2key.placeholder=s.asr2_key_masked?'已配置，留空保持不变':'未配置';asr2model.value=s.asr2_model||'';wakeword.value=s.wake_word||'';visionbase.value=s.vision_base||'';"
    "embbase.value=s.emb_base||'';embkey.value='';embkey.placeholder=s.emb_key_masked?'已配置，留空保持不变':'留空复用 ASR Key';"
    "embmodel.value=s.emb_model||'';"
    "embstate.textContent='语义向量通道：'+(s.embed_ok?'正常（已嵌入 '+s.embed_count+' 条）':'⚠ 已熔断（连败3次，退关键词召回）——查上面的地址与 Key');"
    "vlbase.value=s.vision_llm_base||'';vkey.value='';"
    "vkey.placeholder=s.vision_key_masked?'已配置，留空保持不变':'留空复用大模型 Key';"
    "vmodel.value=s.vision_model||'';}"
    "async function save(e){e.preventDefault();"
    "const b=await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid.value,pass:pass.value,base:base.value,key:key.value,key_set:1,model:model.value,"
    "agent:agent.value,owner:owner.value,wcity:wcity.value,wcity_set:1,persona:persona.value,tick:tick.value,thold:thold.value,"
    "idlesil:idlesil.value,idleevery:idleevery.value,qstart:qstart.value,qend:qend.value,volume:volume.value,brightness:brightness.value,screenoff:screenoff.value,vadstop:vadstop.value,llmcont:llmcont.value,llmtok:llmtok.value,nothink:nothink.checked?1:0,srchkeys:srchkeys.value,srchkeys_set:1,srchapi:srchapi.value,srchapi_set:1,"
    "vmode:vmode.value,ttsch:ttsch.value,ttsvoice:ttsvoice.value,ttsrate:ttsrate.value,"
    "volcappid:volcappid.value,volctoken:volctoken.value,volctoken_set:1,volcvoice:volcvoice.value,"
    "asrbase:asrbase.value,asrkey:asrkey.value,asrkey_set:1,asrmodel:asrmodel.value,asr2base:asr2base.value,asr2key:asr2key.value,asr2key_set:1,asr2model:asr2model.value,asr2_set:1,"
    "embbase:embbase.value,embbase_set:1,embkey:embkey.value,embkey_set:1,embmodel:embmodel.value,embmodel_set:1,"
    "wakeword:wakeword.value,visionbase:visionbase.value,vlbase:vlbase.value,vkey:vkey.value,vkey_set:1,vmodel:vmodel.value,"
    "wakeword_set:1,visionbase_set:1,vlbase_set:1,vmodel_set:1})});"
    "const r=await b.json();alert(r.msg);}"
    "async function otaup(e){e.preventDefault();const f=document.getElementById('otabin').files[0];"
    "if(!f){alert('先选 .bin 文件');return}"
    "otaout.textContent='上传中 '+f.name+' ('+Math.round(f.size/1024)+'KB)，请勿断电…';"
    // 必须用 multipart：ESP32 WebServer 只把 multipart 的请求体流给 upload 回调，
    // 直接 POST 原始二进制（octet-stream）时回调一次都不触发 → "未收到固件数据"
    "const fd=new FormData();fd.append('file',f,f.name);"
    "try{const b=await fetch('/api/ota',{method:'POST',body:fd});"
    "const r=await b.json();otaout.textContent=(r.ok?'✅ ':'❌ ')+r.msg;}catch(err){otaout.textContent='❌ 上传失败: '+err;}}"
    "async function testllm(e){e.preventDefault();testout.textContent='逐项体检中（主模型/视觉/向量/ASR主备，约 10~40 秒）…';"
    "try{const b=await fetch('/api/test',{method:'POST'});const r=await b.json();"
    "testout.innerHTML=(r.results||[]).map(x=>(x.ok?'✅':'❌')+' '+x.name+'：'+String(x.msg).replace(/</g,'&lt;')).join('<br>');}"
    "catch(err){testout.textContent='❌ 体检请求失败: '+err;}}"
    "async function reboot(e){e.preventDefault();if(!confirm('重启?'))return;"
    "await fetch('/api/reboot',{method:'POST'});testout.textContent='重启中…';}"
    "async function resetall(e){e.preventDefault();"
    "await fetch('/api/reset',{method:'POST'});testout.textContent='格式化中…';}"
    "async function voicetest(e){e.preventDefault();vtestout.textContent='播放中…';"
    "const b=await fetch('/api/voice/test',{method:'POST'});const r=await b.json();"
    "vtestout.textContent=r.ok?'✅ 已播放，没声音就查音量/PA':'❌ '+r.msg;}"
    "load();</script>");
  head += body;                                    // v3.50：原地拼接——原 send 表达式 head+body、
  head += FPSTR(PAGE_FOOT);                        // 再 +foot 会整页临时拷贝两次（~2×页面/次浏览的堆抖动）
  server.send(200, "text/html; charset=utf-8", head);
}

void LaapWeb::handleSave() {
  // 手动解析 JSON body（免库）
  String b = server.arg("plain");
  auto get = [&](const char* k) -> String { return jsonField(b, k); };
  // 掩码回显识别：网页旧版会把 "(sk-***abcd)" 这种掩码填进输入框，提交后会把真 Key 覆盖掉。
  // 任何含 "***" 或首尾成对括号的值都不可能是真密钥 → 一律忽略，保住已存的 Key。
  auto maskEcho = [](const String& v) -> bool {
    if (v.indexOf("***") >= 0) return true;
    return v.length() > 2 && v[0] == '(' && v[v.length() - 1] == ')';
  };
  // 密钥类字段统一走"留空=保持、掩码回显=忽略"（掩码一旦落库就再也认证不过 = 用户报的"之前正常后来不通"）
  auto setSecret = [&](const String& v, char* dst, size_t n, const char* what) {
    if (!v.length()) return;
    if (maskEcho(v)) { Serial.printf("[WEB] 忽略疑似掩码回显的 %s（保住已存值）\n", what); return; }
    strlcpy(dst, v.c_str(), n);
  };
  // 布尔哨兵：网页发的是 JSON 数字（"wakeword_set":1，无引号）。
  // 早期写成 get(k)=="1"（只认带引号的字符串形式）→ 永远匹配不上：
  // 唤醒词/视觉/搜索词的写入（含"清空"）实际从未生效，用户看到的就是"重启后配置还原"。
  auto flag = [&](const char* k) -> bool {
    String v = jsonField(b, k);
    return v.length() > 0 && v[0] == '1';
  };
  String ssid = get("ssid"), pass = get("pass"), base = get("base"), key = get("key"),
         model = get("model"), agent = get("agent"), owner = get("owner"), persona = get("persona");
  String ttsvoice = get("ttsvoice"), ttsrate = get("ttsrate"),
         volcappid = get("volcappid"), volctoken = get("volctoken"), volcvoice = get("volcvoice"),
         asrbase = get("asrbase"), asrkey = get("asrkey"), asrmodel = get("asrmodel");
  String vmode = get("vmode"), ttsch = get("ttsch");
  String asr2base = get("asr2base"), asr2key = get("asr2key"), asr2model = get("asr2model");
  String wakeword = get("wakeword"), visionbase = get("visionbase");
  String vlbase = get("vlbase"), vkey = get("vkey"), vmodel = get("vmodel");
  // 唤醒词/视觉支持"清空"：带 _set 哨兵字段即写入（空=关闭）；直连 Key 留空=保持/复用
  if (flag("wakeword_set")) strlcpy(cfg.s.wakeWord, wakeword.c_str(), sizeof(cfg.s.wakeWord));
  if (flag("visionbase_set")) strlcpy(cfg.s.visionBase, visionbase.c_str(), sizeof(cfg.s.visionBase));
  if (flag("vlbase_set")) strlcpy(cfg.s.visionLlmBase, vlbase.c_str(), sizeof(cfg.s.visionLlmBase));
  if (flag("vkey_set")) setSecret(vkey, cfg.s.visionKey, sizeof(cfg.s.visionKey), "视觉 Key");
  if (flag("vmodel_set")) strlcpy(cfg.s.visionModel, vmodel.c_str(), sizeof(cfg.s.visionModel));
  if (vmode.length()) { long v = vmode.toInt(); cfg.s.voiceMode = (uint8_t)(v < 0 ? 0 : (v > 3 ? 3 : v)); }
  if (ttsch.length()) { long v = ttsch.toInt(); cfg.s.ttsChannel = (uint8_t)(v < 0 ? 0 : (v > 3 ? 3 : v)); }
  if (ttsvoice.length()) strlcpy(cfg.s.ttsVoice, ttsvoice.c_str(), sizeof(cfg.s.ttsVoice));
  if (ttsrate.length()) strlcpy(cfg.s.ttsRate, ttsrate.c_str(), sizeof(cfg.s.ttsRate));
  if (volcappid.length()) strlcpy(cfg.s.volcAppid, volcappid.c_str(), sizeof(cfg.s.volcAppid));
  if (flag("volctoken_set")) setSecret(volctoken, cfg.s.volcToken, sizeof(cfg.s.volcToken), "火山 Token");
  if (volcvoice.length()) strlcpy(cfg.s.volcVoice, volcvoice.c_str(), sizeof(cfg.s.volcVoice));
  if (asrbase.length()) strlcpy(cfg.s.asrBase, asrbase.c_str(), sizeof(cfg.s.asrBase));
  if (flag("asrkey_set")) setSecret(asrkey, cfg.s.asrKey, sizeof(cfg.s.asrKey), "ASR Key");
  if (asrmodel.length()) strlcpy(cfg.s.asrModel, asrmodel.c_str(), sizeof(cfg.s.asrModel));
  // 备用 ASR 支持"清空"（带 _set 哨兵即写入，空=不启用）；Key 留空=保持不变
  if (flag("asr2_set")) {
    strlcpy(cfg.s.asr2Base, asr2base.c_str(), sizeof(cfg.s.asr2Base));
    strlcpy(cfg.s.asr2Model, asr2model.c_str(), sizeof(cfg.s.asr2Model));
  }
  if (flag("asr2key_set")) setSecret(asr2key, cfg.s.asr2Key, sizeof(cfg.s.asr2Key), "备用 ASR Key");
  // 语义向量：独立配置（空=复用 ASR 的 base/key）；Key 留空=保持
  String embbase = get("embbase"), embkey = get("embkey"), embmodel = get("embmodel");
  if (flag("embbase_set")) strlcpy(cfg.s.embBase, embbase.c_str(), sizeof(cfg.s.embBase));
  if (flag("embkey_set")) setSecret(embkey, cfg.s.embKey, sizeof(cfg.s.embKey), "向量 Key");
  if (flag("embmodel_set")) strlcpy(cfg.s.embModel, embmodel.c_str(), sizeof(cfg.s.embModel));
  if (ssid.length()) strlcpy(cfg.s.wifiSsid, ssid.c_str(), sizeof(cfg.s.wifiSsid));
  if (pass.length()) strlcpy(cfg.s.wifiPass, pass.c_str(), sizeof(cfg.s.wifiPass));
  if (base.length()) strlcpy(cfg.s.llmBase, base.c_str(), sizeof(cfg.s.llmBase));
  if (flag("key_set")) setSecret(key, cfg.s.llmKey, sizeof(cfg.s.llmKey), "大模型 Key");
  if (model.length()) strlcpy(cfg.s.llmModel, model.c_str(), sizeof(cfg.s.llmModel));
  // 自由文本一律先按字符边界截断再落库（v3.51）：strlcpy 裸截会在汉字中间切断，
  // 半个汉字进 LLM 请求体 = 非法 UTF-8 → 400（"设完人设后它就不说话了"）
  if (agent.length()) strlcpy(cfg.s.agentName, utf8Cut(agent, sizeof(cfg.s.agentName) - 1).c_str(), sizeof(cfg.s.agentName));
  if (owner.length()) strlcpy(cfg.s.ownerName, utf8Cut(owner, sizeof(cfg.s.ownerName) - 1).c_str(), sizeof(cfg.s.ownerName));
  if (flag("wcity_set")) strlcpy(cfg.s.city, utf8Cut(get("wcity"), sizeof(cfg.s.city) - 1).c_str(), sizeof(cfg.s.city));   // 城市：空=按IP定位
  if (persona.length()) strlcpy(cfg.s.persona, utf8Cut(persona, sizeof(cfg.s.persona) - 1).c_str(), sizeof(cfg.s.persona));
  // 只在真的带了字段时才改（原来无条件 toInt()：缺少 tick/thold 的部分保存会把它们打成下限 10）
  String tick = get("tick"), thold = get("thold");
  if (tick.length()) { long v = tick.toInt(); cfg.s.tickSec = (uint32_t)(v < 10 ? 10 : (v > 3600 ? 3600 : v)); }
  if (thold.length()) { long v = thold.toInt(); cfg.s.threshold = (uint8_t)(v < 10 ? 10 : (v > 95 ? 95 : v)); }
  String idls = get("idlesil"), idev = get("idleevery");
  if (idls.length()) { long v = idls.toInt(); cfg.s.idleSilenceMin = (uint16_t)(v < 1 ? 1 : (v > 240 ? 240 : v)); }
  if (idev.length()) { long v = idev.toInt(); cfg.s.idleEveryMin = (uint16_t)(v < 0 ? 0 : (v > 240 ? 240 : v)); }
  String qst = get("qstart"), qen = get("qend");
  if (qst.length()) { long v = qst.toInt(); cfg.s.quietStart = (uint8_t)(v < 0 ? 0 : (v > 23 ? 23 : v)); }
  if (qen.length()) { long v = qen.toInt(); cfg.s.quietEnd = (uint8_t)(v < 0 ? 0 : (v > 23 ? 23 : v)); }
  String vol = get("volume");
  if (vol.length()) {
    int v = vol.toInt(); if (v < 0) v = 0; if (v > 100) v = 100;
    cfg.s.volume = (uint8_t)v;
    audio.setVolume((uint8_t)v);          // 立即生效，不等重启
  }
  String bri = get("brightness");
  if (bri.length()) {
    int v = bri.toInt(); if (v < 5) v = 5; if (v > 100) v = 100;
    cfg.s.brightness = (uint8_t)v;
    display.setBrightness((uint8_t)v);    // 立即生效（下限5防全黑找不到设置页）
  }
  String soff = get("screenoff");
  if (soff.length()) { long v = soff.toInt(); cfg.s.screenOffSec = (uint16_t)(v < 0 ? 0 : (v > 3600 ? 3600 : v)); }
  String vstop = get("vadstop");
  if (vstop.length()) {
    long v = vstop.toInt();
    cfg.s.vadStopMs = (uint16_t)(v < 300 ? 300 : (v > 15000 ? 15000 : v));
    audio.setVadStopMs(cfg.s.vadStopMs);   // 立即生效，不等重启
  }
  String lcont = get("llmcont");
  if (lcont.length()) { long v = lcont.toInt(); cfg.s.llmContinue = (uint8_t)(v < 0 ? 0 : (v > 3 ? 3 : v)); }
  String ltok = get("llmtok");
  if (ltok.length()) { long v = ltok.toInt(); cfg.s.llmMaxTokens = (uint16_t)(v < 80 ? 80 : (v > 1000 ? 1000 : v)); }
  String lnt = get("nothink");
  if (lnt.length()) cfg.s.llmNoThink = (lnt.toInt() != 0) ? 1 : 0;
  String srchkeys = get("srchkeys");
  if (flag("srchkeys_set")) strlcpy(cfg.s.searchKeys, srchkeys.c_str(), sizeof(cfg.s.searchKeys));
  String srchapi = get("srchapi");
  if (flag("srchapi_set")) strlcpy(cfg.s.searchApi, srchapi.c_str(), sizeof(cfg.s.searchApi));
  cfg.save();
  if (_ap) { // 配置门户里保存 → 直接重启进 STA
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已保存，3 秒后重启生效\"}");
    delay(800);
    laapReboot("配置保存");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已保存。WiFi/网络变更需重启生效\"}");
}

void LaapWeb::handleStatus() {
  String key = String(cfg.s.llmKey);
  String masked = key.length() ? (key.substring(0, 3) + "***" + key.substring(key.length() - 4 > 3 ? key.length() - 4 : 3)) : "";
  // worldJson 形如 {"time":...,"needs":{...},"mood":"...","goal":"..."}
  // needs 不是末键：从它的 { 截到配对的 }（内部无嵌套，第一个 } 即终点），避免把 mood/goal 重复拼进来多出一个 }
  String wj = mind.worldJson();
  int ns = wj.indexOf("\"needs\":");
  String needs = String("{}");
  if (ns >= 0) {
    int ob = wj.indexOf('{', ns);
    int cb = wj.indexOf('}', ob);
    if (ob >= 0 && cb > ob) needs = wj.substring(ob, cb + 1);
  }
  String j = String("{\"agent\":\"") + jsonEsc(cfg.s.agentName) +
    "\",\"owner\":\"" + jsonEsc(cfg.s.ownerName) +
    "\",\"city\":\"" + jsonEsc(cfg.s.city) +
    "\",\"mood\":\"" + mind.moodKey() + "\",\"mood_cn\":\"" + mind.moodCn() +
    "\",\"goal\":\"" + mind.goalCn() +
    "\",\"generation\":" + mind.generation() +
    ",\"cycles\":" + mind.cycles() +
    ",\"chats\":" + mind.chats() +
    ",\"events\":" + memory.eventCount() +
    ",\"last_say\":\"" + jsonEsc(laapLastSay()) +
    "\",\"ap\":" + (_ap ? "true" : "false") +
    ",\"ap_ssid\":\"" + jsonEsc(_apSsid) +
    "\",\"wifi_ok\":" + (WiFi.status() == WL_CONNECTED ? "true" : "false") +
    ",\"ip\":\"" + (_ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) +
    "\",\"ssid\":\"" + jsonEsc(cfg.s.wifiSsid) +
    "\",\"llm_base\":\"" + jsonEsc(cfg.s.llmBase) +
    "\",\"llm_model\":\"" + jsonEsc(cfg.s.llmModel) +
    "\",\"llm_key_masked\":\"" + masked +
    "\",\"persona\":\"" + jsonEsc(cfg.s.persona) +
    "\",\"tick\":" + cfg.s.tickSec +
    ",\"threshold\":" + cfg.s.threshold +
    ",\"idle_silence\":" + cfg.s.idleSilenceMin +
    ",\"idle_every\":" + cfg.s.idleEveryMin +
    ",\"quiet_start\":" + cfg.s.quietStart +
    ",\"quiet_end\":" + cfg.s.quietEnd +
    ",\"volume\":" + cfg.s.volume +
    ",\"brightness\":" + cfg.s.brightness +
    ",\"llm_continue\":" + cfg.s.llmContinue +
    ",\"llm_max_tokens\":" + cfg.s.llmMaxTokens +
    ",\"nothink\":" + cfg.s.llmNoThink +
    ",\"search_keys\":\"" + jsonEsc(cfg.s.searchKeys) + "\"," +
    "\"search_api\":\"" + jsonEsc(cfg.s.searchApi) + "\"," +
    "\"voice_mode\":" + cfg.s.voiceMode +
    ",\"tts_channel\":" + cfg.s.ttsChannel +
    ",\"tts_voice\":\"" + jsonEsc(cfg.s.ttsVoice) +
    "\",\"tts_rate\":\"" + jsonEsc(cfg.s.ttsRate) +
    "\",\"volc_appid\":\"" + jsonEsc(cfg.s.volcAppid) +
    "\",\"volc_token_masked\":\"" + (String(cfg.s.volcToken).length() ? "已配置" : "") +
    "\",\"volc_voice\":\"" + jsonEsc(cfg.s.volcVoice) +
    "\",\"asr_base\":\"" + jsonEsc(cfg.s.asrBase) +
    "\",\"asr_key_masked\":\"" + (String(cfg.s.asrKey).length() ? "已配置" : "") +
    "\",\"asr_model\":\"" + jsonEsc(cfg.s.asrModel) +
    "\",\"asr2_base\":\"" + jsonEsc(cfg.s.asr2Base) +
    "\",\"asr2_key_masked\":\"" + (String(cfg.s.asr2Key).length() ? "已配置" : "") +
    "\",\"asr2_model\":\"" + jsonEsc(cfg.s.asr2Model) +
    "\",\"emb_base\":\"" + jsonEsc(cfg.s.embBase) +
    "\",\"emb_model\":\"" + jsonEsc(cfg.s.embModel) +
    "\",\"emb_key_masked\":\"" + (String(cfg.s.embKey).length() ? "已配置" : "") +
    "\",\"embed_ok\":" + (memory.embedFused() ? "false" : "true") +
    ",\"embed_count\":" + memory.embedCount() +
    ",\"wake_word\":\"" + jsonEsc(cfg.s.wakeWord) +
    "\",\"vision_base\":\"" + jsonEsc(cfg.s.visionBase) +
    "\",\"vision_llm_base\":\"" + jsonEsc(cfg.s.visionLlmBase) +
    "\",\"vision_key_masked\":\"" + (String(cfg.s.visionKey).length() ? "已配置" : "") +
    "\",\"vision_model\":\"" + jsonEsc(cfg.s.visionModel) + "\"," +
    "\"llm_ctx\":\"" + jsonEsc(laapLastReqShape()) + "\"," +   // 转义（v3.51）
    "\"vision_ready\":" + (vision.available() ? "true" : "false") +
    ",\"voice_ready\":" + (voice.ready() ? "true" : "false") +
    ",\"vad_paused\":" + (voice.vadPaused() ? "true" : "false") +
    // 分区/文件系统：OTA 之后靠 part 确认新固件真的在跑；fs 用来核对记忆占用
    ",\"part\":\"" + String(esp_ota_get_running_partition() ? esp_ota_get_running_partition()->label : "?") + "\"" +
    ",\"ota_slot\":\"" + String(esp_ota_get_next_update_partition(nullptr) ? esp_ota_get_next_update_partition(nullptr)->label : "无（不可 OTA）") + "\"" +
    // 本次启动原因：主动重启在 NVS 打标（laapReboot），崩溃/看门狗由 esp_reset_reason 细分
    ",\"boot_reason\":\"" + jsonEsc(laapBootReason()) + "\"" +
    ",\"fs_used_kb\":" + String(LittleFS.usedBytes() / 1024) +
    ",\"fs_total_kb\":" + String(LittleFS.totalBytes() / 1024) +
    ",\"screen_off\":" + cfg.s.screenOffSec +
    ",\"vad_stop\":" + cfg.s.vadStopMs +
    ",\"r0_err\":" + String(r0.rollingErr(), 2) +
    ",\"r0_steps\":" + (uint32_t)r0.steps() +
    ",\"uptime_s\":" + String(millis() / 1000) +
    ",\"uptime_total_min\":" + String(laapUptimeMin()) +
    ",\"heap_kb\":" + String(ESP.getFreeHeap() / 1024) +
    // 环任务栈历史最低余量（字节）：TTS 的 TLS 握手最吃栈，低于 2-3KB 就该警惕
    ",\"stack_min\":" + String((unsigned)uxTaskGetStackHighWaterMark(NULL)) +   // 已是字节（旧代码 ×sizeof(StackType_t) 虚高 4 倍，v3.51）
    // 最大连续块：TLS 握手要一整块，碎片多时"总空闲够"也会连不上（排障关键指标）
    // v3.55 起 TLS 收发缓冲走 PSRAM，此值不再被握手大幅拉低；tlsroute_kb=0 说明钩子未生效
    ",\"heap_max_kb\":" + String((unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024)) +
    ",\"tlsroute_kb\":" + String(laapTlsHeapRouteKb()) +
    ",\"wifi_rssi\":" + (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("0")) +
    ",\"chip_temp\":" + String(temperatureRead(), 1) +
    ",\"needs\":" + needs +
    // 主动表达的"闸门状态"：dominance 越过 threshold 就会说话，冷却 3 分钟内不再说
    ",\"dominance\":" + String(mind.dominance(), 2) +
    ",\"idle\":\"" + jsonEsc(laapIdleInfo()) + "\"" +   // 转义（v3.51：拼进 JSON 前统一过 jsonEsc）
    // 评估埋点（本次开机）：RSI 闭环的 fitness 端，网页/脚本都从这里读
    ",\"metrics\":{" + metrics.json() + "}" +
    "}";
  server.send(200, "application/json", j);
}

void LaapWeb::handleChat() {
  String b = server.arg("plain");
  String text = jsonField(b, "text");   // 容错 "text":"…" / "text": "…"
  if (!text.length()) { server.send(400, "application/json", "{\"ok\":false,\"reply\":\"空消息\"}"); return; }
  String reply = laapInteractSearch(text);   // 带联网搜索；LLM 阶段是异步投递
  if (laapChatPending()) {
    // 只有受理回执（"……"）：真回复由后台 LLM 任务产出，网页轮询 /api/chat/reply 取。
    // seq 取"此刻"值而不是受理前：受理前的旧值会让上一问恰好完成的回复
    // 被 seq>seq0 判定命中，串台显示到本问气泡下（还污染 👍/👎 反馈的配对）
    server.send(200, "application/json",
        String("{\"ok\":true,\"pending\":true,\"seq\":") + String(laapChatSeq()) + "}");
    return;
  }
  server.send(200, "application/json",
      String("{\"ok\":true,\"pending\":false,\"reply\":\"") + jsonEsc(reply) + "\"}");
}

void LaapWeb::handleChatReply() {
  server.send(200, "application/json",
      String("{\"ok\":true,\"seq\":") + String(laapChatSeq()) +
      ",\"reply\":\"" + jsonEsc(laapChatReply()) + "\"}");
}

void LaapWeb::handleTest() {
  // 逐项体检：把所有已配置的模型/通道各探一次，独立报结果（原来只测主模型）。
  // 后台 LLM 任务在飞时 TLS 会互相抢堆（每次握手要 ~40KB 最大连续块），结果会失真——
  // laapChatPending 只覆盖聊天，独白/反思/整理的多步流水线要另查 laapLlmBusy
  if (laapChatPending() || laapLlmBusy()) {
    server.send(409, "application/json",
                "{\"ok\":false,\"results\":[{\"name\":\"体检\",\"ok\":false,\"msg\":\"后台 LLM 正在忙（思考/独白/反思中），等它忙完再测（结果会被堆挤不准）\"}]}");
    return;
  }
  String j = "{\"ok\":true,\"results\":[";
  int n = 0;
  auto add = [&](const char* name, bool ok, const String& msg) {
    if (n++) j += ",";
    j += String("{\"name\":\"") + name + "\",\"ok\":" + (ok ? "true" : "false") +
         ",\"msg\":\"" + jsonEsc(msg) + "\"}";
  };
  // 1 主模型（原有通道）
  {
    LlmClient probe;
    String reply;
    bool ok = probe.ping(reply);
    add("主模型", ok, ok ? (String(cfg.s.llmModel) + " 回复: " + reply.substring(0, 40))
                          : String(probe.lastError));
  }
  // 2 视觉（手机桥不探测；直连按它实际用的 base/key/model 发文本探针）
  if (cfg.s.visionBase[0]) {
    add("视觉", true, String("手机桥模式（") + cfg.s.visionBase + "，请直接说“看看…”实测）");
  } else if (cfg.s.visionLlmBase[0] || cfg.s.visionModel[0]) {
    String url(cfg.s.visionLlmBase[0] ? cfg.s.visionLlmBase : "https://openrouter.ai/api/v1/chat/completions");
    String model(cfg.s.visionModel[0] ? cfg.s.visionModel : "google/gemini-flash-1.5");
    String key(cfg.s.visionKey[0] ? cfg.s.visionKey : cfg.s.llmKey);
    String d;
    bool ok = probeChat(url, key, model, d);
    add("视觉直连", ok, ok ? (model + " 通") : d);
  } else {
    add("视觉", true, "未配置（跳过）");
  }
  // 3 语义向量（laapEmbed 内部就是实际的解析与调用，测它最真实）
  {
    bool ok = false;
    String bin = laapEmbed("连通测试", ok);
    add("语义向量", ok, ok ? ("bge-m3 通（返回 " + String(bin.length() / 4) + " 维）")
                            : "调用失败（查语音卡的向量地址/Key，或它复用的 ASR 配置）");
  }
  // 4/5 ASR 主备（合成短音真发一次 multipart）
  {
    String err = asr.probe(0);
    add("ASR 主", err.length() == 0,
        err.length() == 0 ? (String(cfg.s.asrModel) + " 通（HTTP 200）") : err);
  }
  if (cfg.s.asr2Base[0]) {
    String err = asr.probe(1);
    add("ASR 备用", err.length() == 0,
        err.length() == 0 ? (String(cfg.s.asr2Model) + " 通（HTTP 200）") : err);
  } else {
    add("ASR 备用", true, "未配置（跳过）");
  }
  server.send(200, "application/json", j + "]}");
}

void LaapWeb::handleMemoryPage() {
  String head(FPSTR(PAGE_HEAD));
  String body = F(
    "<h1>记忆 <small>情景记忆 / 语义记忆</small></h1>"
    "<div class='nav'><a href='/'>状态</a><a href='/settings'>后台配置</a><a href='/memory'>记忆</a></div>"
    "<div class='card'><div class='k' style='color:#8b95a8;font-size:12px'>语义记忆（自我认知摘要，由大模型周期性压缩）</div>"
    "<div id='sem' style='margin-top:6px'>…</div></div>"
    "<div class='card'><div class='k' style='color:#8b95a8;font-size:12px'>最近情景记忆</div>"
    "<div id='eps' style='margin-top:6px;font-size:13px;line-height:1.8'></div>"
    "<div style='margin-top:10px;display:flex;gap:8px;flex-wrap:wrap'>"
    "<a class='ghost' href='/api/memexport' style='text-decoration:none;padding:6px 12px'>导出记忆（备份）</a>"
    "<button class='ghost' onclick='memimp(event)'>导入记忆（恢复）</button>"
    "<button class='ghost' onclick='clearmem(event)'>清空全部记忆</button></div>"
    "<input type='file' id='memfile' accept='.txt' style='margin-top:8px;width:100%'>"
    "<div class='hint'>导出的 txt 含情景记忆+自我认知+性格进化；导入会覆盖当前记忆（先备份再导入更稳）</div></div>"
    "<div class='card'><b>⏪ 快照回滚</b>"
    "<div class='hint'>「自我」三件套（自我认知 / 情景记忆 / 性格进化）的自动快照，各留 3 版：自动档 12 小时一拍，清空记忆 / 导入记忆前会强制拍一份。恢复 = 把选中的版本拷回原位（现状先存成 .pre），然后设备自动重启。这是它「自我进化跑偏」时的后悔药。</div>"
    "<div id='snaps' style='margin-top:6px;font-size:13px;line-height:2.2'></div></div>"
    "<div class='card'><b>🧠 学会的行为规则</b>"
    "<div class='hint'>每晚用你的 👍/👎 和当天的失败记录自动归纳（也可点按钮立刻归纳）。点踩越多，它改得越准。</div>"
    "<div id='rules' style='margin-top:6px;font-size:13px;white-space:pre-wrap;line-height:1.9'>…</div>"
    "<button class='ghost' onclick='rulesref(event)' style='margin-top:8px'>立刻归纳一轮</button></div>"
    "<div class='card'><b>🎯 口令技能</b>"
    "<div class='hint'>对它说「以后每当我说<词>，你就<做什么>」就能教一个技能；以后它听到触发词就照做。</div>"
    "<div id='skills' style='margin-top:6px;font-size:13px;white-space:pre-wrap;line-height:1.9'>…</div></div>"
    "<script>"
    "async function load(){const s=await (await fetch('/api/memory')).json();"
    "sem.textContent=s.semantic||'（还没有形成自我认知）';"
    "let h='';for(const e of s.events.slice(-40).reverse()){"
    "const c=e.r=='user'?'#9ae6b4':(e.r=='aris'?'#6fd3ff':'#8b95a8');"
    "h+='<div style=\"color:'+c+'\">['+e.r+'] '+e.x.replace(/</g,'&lt;')+'</div>';}"
    "eps.innerHTML=h||'（空）';loadSnaps();loadLearned();}"
    "async function loadLearned(){try{"
    "const r=await (await fetch('/api/rules')).json();"
    "document.getElementById('rules').textContent=r.rules||'（还没有。点踩几次 + 点「立刻归纳」就有了）';"
    "const s=await (await fetch('/api/skills')).json();"
    "document.getElementById('skills').textContent=s.skills||'（还没教过。对它说：以后每当我说…你就…）';}catch(e){}}"
    "async function rulesref(e){e.preventDefault();"
    "if(!confirm('现在用当前反馈/失败素材归纳一轮规则?'))return;"
    "try{const r=await (await fetch('/api/rulesreflect',{method:'POST'})).json();"
    "alert(r.msg);}catch(err){alert('提交失败: '+err);}}"
    "async function loadSnaps(){try{const a=await (await fetch('/api/snapshots')).json();let h='';"
    "for(const s of a){h+='<div><b>'+s.name+'</b> <span style=\"color:#8b95a8;font-size:12px\">'+s.path+'</span> ';"
    "if(!s.v.length)h+='（还没有快照，写入一次后就会出现）';"
    "for(const v of s.v)h+='<button class=\"ghost\" style=\"padding:4px 10px;margin:2px\" onclick=\"snapres(event,\\''+s.name+'\\','+v.n+')\">v'+v.n+' · '+v.kb+'KB · '+(v.age_h<0?'时间未知':v.age_h+'小时前')+'</button>';"
    "h+='</div>';}"
    "document.getElementById('snaps').innerHTML=h;}catch(e){}}"
    "async function snapres(e,n,v){e.preventDefault();"
    "if(!confirm('把 '+n+' 恢复到第 '+v+' 版？现状会先存成 .pre，恢复后设备重启。'))return;"
    "try{const r=await (await fetch('/api/snapshot/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n,ver:v})})).json();"
    "alert((r.ok?'✅ ':'❌ ')+r.msg);}catch(err){alert('恢复失败: '+err);}}"
    "async function clearmem(e){e.preventDefault();if(!confirm('清空全部记忆?（建议先导出备份）'))return;"
    "await fetch('/api/clear',{method:'POST'});load();}"
    "async function memimp(e){e.preventDefault();const f=document.getElementById('memfile').files[0];"
    "if(!f){alert('先选择要导入的记忆 txt 文件');return;}"
    "if(!confirm('导入会覆盖当前记忆，继续?'))return;"
    "const fd=new FormData();fd.append('file',f, f.name);"
    "try{const r=await (await fetch('/api/memimport',{method:'POST',body:fd})).json();"
    "alert((r.ok?'✅ ':'❌ ')+r.msg);load();}catch(err){alert('导入失败: '+err);}}"
    "load();</script>");
  head += body;                                    // v3.50：原地拼接——原 send 表达式 head+body、
  head += FPSTR(PAGE_FOOT);                        // 再 +foot 会整页临时拷贝两次（~2×页面/次浏览的堆抖动）
  server.send(200, "text/html; charset=utf-8", head);
}

void LaapWeb::handleMemoryApi() {
  String sem = memory.semantic();
  String esc;
  for (unsigned int i = 0; i < sem.length(); i++) {
    char c = sem[i];
    if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
    else if (c == '\n') esc += "\\n";
    else esc += c;
  }
  String j = String("{\"semantic\":\"") + esc + "\",\"events\":" + memory.episodicTail(60) + "}";
  server.send(200, "application/json", j);
}

void LaapWeb::handleMemExport() {
  String dump = memory.exportDump();
  // 完整性校验：内部堆紧张时 String 增长可能中途失败，宁可报错也不能给出一份
  // "看起来正常、其实少了一半" 的备份（恢复时才发现少了记忆是最坏的情况）
  if (!dump.endsWith("###END\n") || !dump.startsWith("###LAAP-MEMORY")) {
    Serial.printf("[MEM] 导出失败：内容不完整（%u 字节）\n", (unsigned)dump.length());
    server.send(500, "application/json",
                "{\"ok\":false,\"msg\":\"导出失败：内存紧张导致内容不完整，稍后重试\"}");
    return;
  }
  Serial.printf("[MEM] 导出记忆 %u 字节（%lu 条事件）\n",
                (unsigned)dump.length(), (unsigned long)memory.eventCount());
  server.sendHeader("Content-Disposition", "attachment; filename=laap-memory.txt");
  server.send(200, "text/plain; charset=utf-8", dump);
}

void LaapWeb::handleMemImport() {
  if (!memImportOk) {
    server.send(500, "application/json", "{\"ok\":false,\"msg\":\"上传失败（内容没落盘）\"}");
    return;
  }
  String msg;
  bool ok = memory.applyImport(msg);
  if (ok) mind.reloadEvolution();   // 让盘上的性格进化立刻生效（否则被运行中的旧值覆盖）
  server.send(ok ? 200 : 500, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + jsonEsc(msg) + "\"}");   // 转义（v3.51）
}

void LaapWeb::handleClear() {
  memory.clearAll();
  // 只删盘不清 RAM 态的话，一个心跳后 psiTick 就会用旧性格把 evolution.json 写回、
  // 意图栈照常注入提示词——"清空"等于半失效（实测 30 秒内静默回滚）
  mind.resetEvolution();
  mind.clearAllIntents();
  server.send(200, "application/json", "{\"ok\":true}");
}

void LaapWeb::handleReset() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  cfg.reset();
  laapReboot("恢复出厂");
}

void LaapWeb::handleReboot() {
  laapUptimePersist();   // 重启前把累计运行时长落盘（否则这一截时长白丢）
  metrics.persist();     // 指标/失败环留底
  mind.saveEvolution(true);   // 需求/情绪留底：重启醒来状态续跑（v3.42）
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  laapReboot("网页重启");
}

// ---- 语音 ----
void LaapWeb::handleVoiceTest() {
  voice.speak("你好，我是" + String(cfg.s.agentName) + "，我能说话了。", "happy");
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已播放\"}");
}

void LaapWeb::handleListenToggle() {
  voice.setVadPaused(!voice.vadPaused());
  server.send(200, "application/json",
      String("{\"ok\":true,\"paused\":") + (voice.vadPaused() ? "true" : "false") + "}");
}

// ---- 评估埋点 / 反馈 / 快照（RSI 闭环：评估端 + 回滚安全网） ----
void LaapWeb::handleMetrics() {
  // prev = 上一段会话快照（NVS）：崩溃/自愈重启后仍能查到重启前的失败计数与原因
  String prev;
  if (metrics.hasPrev()) {
    const LaapMetrics* pv = metrics.prev();
    prev = String(",\"prev\":{\"metrics\":{") + pv->json() +
           "},\"fail_notes\":\"" + jsonEsc(pv->failDigest()) + "\"}";
  }
  server.send(200, "application/json",
              String("{\"ok\":true,\"boot_ms\":") + millis() +
              ",\"tune\":{\"cooldown_ms\":" + voice.cooldownDur() +
              ",\"vad_mul\":" + String(voice.vadMul(), 2) + "}" +
              ",\"metrics\":{" + metrics.json() + "}" + prev + "}");
}

void LaapWeb::handleFeedback() {
  String b = server.arg("plain");
  int v = (int)jsonField(b, "v").toInt();
  if (v != 1 && v != -1) { server.send(400, "application/json", "{\"ok\":false,\"msg\":\"v 须为 1 或 -1\"}"); return; }
  // 反馈直接进认知：👍=信任+，👎=失望+信任-（它会对"被踩的回答"表现出警觉/低落）
  if (v > 0) mind.trustUpdate(2, 0);
  else { mind.trustUpdate(0, 1); mind.onLetdown(0.5f); }
  bool ok = metrics.feedback(v, laapLastUserText(), laapChatReply());
  server.send(ok ? 200 : 500, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

void LaapWeb::handleSnapshots() {
  server.send(200, "application/json", laapSnapListJson());
}

void LaapWeb::handleSnapRestore() {
  String b = server.arg("plain");
  String name = jsonField(b, "name");
  int ver = (int)jsonField(b, "ver").toInt();
  if (!name.length() || ver < 1 || ver > SNAP_KEEP) {   // 版本上限用唯一定义（v3.51：原来硬编码 3）
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"name/ver 缺失或非法\"}");
    return;
  }
  if (!laapSnapRestore(name.c_str(), ver)) {
    server.send(500, "application/json",
                String("{\"ok\":false,\"msg\":\"恢复失败：") + jsonEsc(name) + " 没有第 " + ver + " 版快照\"}");
    return;
  }
  // 恢复后必须重启：工作记忆环、语义向量缓存都与盘上文件绑死，热切换必错位
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"已恢复，重启后生效\"}");
  laapUptimePersist();
  delay(600);
  laapReboot("快照恢复");
}

// ---- 学到的东西（规则 / 技能）----（记忆页展示 + 归纳按钮）
void LaapWeb::handleRulesApi() {
  server.send(200, "application/json",
              String("{\"rules\":\"") + jsonEsc(rules.text()) + "\"}");
}

void LaapWeb::handleRelationsApi() {
  server.send(200, "application/json",
              String("{\"relations\":\"") + jsonEsc(memory.relationsText()) + "\"}");
}

void LaapWeb::handleRelationsReflect() {
  // 与 /api/rulesreflect 同一条路：异步提交，10~30 秒后刷新 /api/relations 看结果
  relationsReflect(true);
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"已提交抽取，约 10~30 秒后生效，稍后刷新查看\"}");
}

void LaapWeb::handleMoodRelabel() {
  moodRelabel(true);   // 异步提交；结果在 /api/memory 的记忆行 "m" 字段可见
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"已提交情绪标注，约 10~30 秒后生效\"}");
}

void LaapWeb::handleConsc() {
  server.send(200, "application/json", laapConscAudit());
}

void LaapWeb::handleDream() {
  dreamReflect(true);   // 异步提交；结果在 /api/memory 以【梦】标记可见
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"已提交做梦，约 10~30 秒后写入记忆\"}");
}

void LaapWeb::handleSkillsApi() {
  server.send(200, "application/json",
              String("{\"skills\":\"") + jsonEsc(skills.text()) + "\"}");
}

void LaapWeb::handleRulesReflect() {
  // 与串口 /rulesreflect 同一条路：异步提交，10~30 秒后刷新页面看结果
  rulesReflect(true);
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"已提交归纳，约 10~30 秒后生效，稍后刷新查看\"}");
}

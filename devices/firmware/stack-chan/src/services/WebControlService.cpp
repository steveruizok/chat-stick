#include "WebControlService.h"

#include "Config.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#if __has_include("credentials.h")
#include "credentials.h"
#define STACK_CHAN_HAS_WIFI_CREDENTIALS 1
#else
#define STACK_CHAN_HAS_WIFI_CREDENTIALS 0
#endif

namespace {
const char kControlPage[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stack-Chan control</title><style>
:root{color-scheme:dark;--paper:#0b0b0b;--panel:#151515;--line:#444;--ink:#f5f5f0;--muted:#999;--active:#fff}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:14px ui-monospace,SFMono-Regular,Menlo,monospace}
main{max-width:1060px;margin:auto;padding:24px}.top{display:flex;justify-content:space-between;gap:20px;align-items:end;border-bottom:1px solid var(--ink);padding-bottom:12px;margin-bottom:18px}
h1{font-size:22px;margin:0;font-weight:500}.status{color:var(--muted);text-align:right}.grid{display:grid;grid-template-columns:minmax(300px,1.35fr) minmax(280px,1fr);gap:16px}
.card{border:1px solid var(--line);background:var(--panel);padding:14px}.card h2{font-size:12px;text-transform:uppercase;letter-spacing:.12em;color:var(--muted);margin:0 0 12px}
.video{aspect-ratio:4/3;background:#000;display:grid;place-items:center;overflow:hidden;border:1px solid #333}.video img{width:100%;height:100%;object-fit:contain}.video span{color:#666}
.readout{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;background:var(--line);border:1px solid var(--line)}.datum{background:var(--paper);padding:10px}.datum b{display:block;font-size:18px;font-weight:500}.datum small{color:var(--muted)}
.pad{position:relative;aspect-ratio:1;border:1px solid var(--line);touch-action:none;background:linear-gradient(90deg,transparent 49.8%,#333 50%,transparent 50.2%),linear-gradient(transparent 49.8%,#333 50%,transparent 50.2%)}
.dot{position:absolute;width:18px;height:18px;border:2px solid #fff;border-radius:50%;translate:-50% -50%;left:50%;top:50%;pointer-events:none}.axis{display:flex;justify-content:space-between;color:var(--muted);font-size:11px;margin-top:5px}
.buttons{display:flex;flex-wrap:wrap;gap:7px}button{font:inherit;color:var(--ink);background:#202020;border:1px solid #555;padding:9px 11px;cursor:pointer}button:hover,button.active{background:var(--active);color:#000;border-color:#fff}
button:disabled{opacity:.38;cursor:not-allowed}.audio-card{grid-column:1/-1}.audio-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}.audio-panel{border:1px solid var(--line);padding:12px}.audio-panel h3{font-size:13px;margin:0 0 6px;font-weight:500}.audio-panel p{color:var(--muted);min-height:34px;margin:0 0 12px;font-size:12px;line-height:1.45}.audio-status{color:var(--muted);font-size:11px;margin-top:9px;min-height:16px}.audio-player{display:block;width:100%;height:34px;margin-top:10px}.file-button{display:inline-block;font:inherit;color:var(--ink);background:#202020;border:1px solid #555;padding:9px 11px;cursor:pointer}.file-button:hover{background:var(--active);color:#000;border-color:#fff}.file-button input{display:none}
.rgb{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center}input[type=color]{width:100%;height:38px;background:none;border:1px solid #555}.touch{display:flex;gap:5px}.zone{height:24px;flex:1;border:1px solid #555;background:#111}.zone[data-v="1"]{background:#555}.zone[data-v="2"]{background:#aaa}.zone[data-v="3"]{background:#fff}
.error{color:#ff8d8d}.footer{color:var(--muted);margin-top:14px;font-size:11px}@media(max-width:720px){main{padding:14px}.grid,.audio-grid{grid-template-columns:1fr}.audio-card{grid-column:auto}.top{align-items:start;flex-direction:column}.status{text-align:left}}
</style></head><body><main><div class="top"><h1>Stack-Chan / control</h1><div class="status" id="connection">connecting...</div></div>
<div class="grid"><section class="card"><h2>Camera</h2><div class="video" id="videoBox"><img id="video" alt="Stack-Chan camera"></div></section>
<section class="card"><h2>Head</h2><div class="pad" id="pad"><i class="dot" id="dot"></i></div><div class="axis"><span>yaw -120°</span><span>pitch 5–85°</span><span>yaw +120°</span></div><div class="buttons" style="margin-top:10px"><button id="neutral">Neutral</button><button id="stop">Stop</button><button id="torque">Torque off</button></div></section>
<section class="card"><h2>State</h2><div class="readout"><div class="datum"><b id="battery">--</b><small>battery</small></div><div class="datum"><b id="yaw">--</b><small>yaw</small></div><div class="datum"><b id="pitch">--</b><small>pitch</small></div></div><h2 style="margin-top:16px">Top touch</h2><div class="touch"><div class="zone" id="t0"></div><div class="zone" id="t1"></div><div class="zone" id="t2"></div></div><div class="axis"><span>front</span><span>middle</span><span>back</span></div></section>
<section class="card"><h2>Expression</h2><div class="buttons" id="expressions"></div><h2 style="margin-top:18px">Body LEDs</h2><div class="rgb"><input id="color" type="color" value="#000020"><button id="setColor">Set</button></div></section>
<section class="card audio-card"><h2>Audio exchange</h2><div class="audio-grid"><div class="audio-panel"><h3>Stack-Chan → this browser</h3><p>Record five seconds with the microphones in the device, then listen here.</p><div class="buttons"><button id="deviceRecord">Record device · 5 s</button><button id="microphoneToggle">Mute device mic</button></div><audio class="audio-player" id="deviceAudio" controls hidden></audio><div class="audio-status" id="deviceAudioStatus">Ready</div></div><div class="audio-panel"><h3>This browser → Stack-Chan</h3><p>Record up to ten seconds here, or choose an audio recording. It is converted before being sent to the device.</p><div class="buttons"><button id="browserRecord">Start recording</button><label class="file-button">Choose / record audio<input id="audioFile" type="file" accept="audio/*" capture></label><button id="speakerToggle">Mute device speaker</button></div><div class="audio-status" id="browserAudioStatus">Ready</div></div></div></section></div>
<div class="footer">Local control only · audio is half-duplex · movement is clamped in firmware · update rate 5 Hz</div></main><script>
const $=id=>document.getElementById(id), expressions=['neutral','happy','listening','thinking','speaking','surprised','sleepy'],audioBase=`${location.protocol}//${location.hostname}:82`;let state={},lastMove=0,torque=true,browserRecording=null,deviceAudioUrl='';
async function post(path,params={}){const q=new URLSearchParams(params);const r=await fetch(path+'?'+q,{method:'POST'});if(!r.ok)throw Error(await r.text());return r}
function setVideo(available){const box=$('videoBox');let img=$('video');if(available){if(!img){img=document.createElement('img');img.id='video';img.alt='Stack-Chan camera';box.replaceChildren(img)}if(!img.src){img.src=`http://${location.hostname}:81/stream`;img.onerror=()=>{box.innerHTML='<span class="error">video unavailable</span>'}}}else if(img){box.innerHTML='<span>camera unavailable</span>'}}
function render(s){state=s;$('connection').textContent=`${s.mode} · ${s.ip} · ${s.moving?'moving':'idle'}`;$('battery').textContent=s.battery_percent<0?'--':s.battery_percent+'%';$('yaw').textContent=s.yaw.toFixed(1)+'°';$('pitch').textContent=s.pitch.toFixed(1)+'°';for(let i=0;i<3;i++)$('t'+i).dataset.v=s.touch[i];document.querySelectorAll('[data-expression]').forEach(b=>b.classList.toggle('active',b.dataset.expression===s.expression));$('dot').style.left=((s.yaw+120)/240*100)+'%';$('dot').style.top=((85-s.pitch)/80*100)+'%';$('microphoneToggle').textContent=s.microphone_enabled?'Mute device mic':'Unmute device mic';$('speakerToggle').textContent=s.speaker_enabled?'Mute device speaker':'Unmute device speaker';$('microphoneToggle').classList.toggle('active',!s.microphone_enabled);$('speakerToggle').classList.toggle('active',!s.speaker_enabled);$('deviceRecord').disabled=!s.audio_available||!s.microphone_enabled||s.audio_recording;$('browserRecord').disabled=!s.audio_available||!s.speaker_enabled||(!browserRecording&&s.audio_playing);setVideo(s.camera)}
async function poll(){try{const r=await fetch('/api/state',{cache:'no-store'});render(await r.json())}catch(e){$('connection').textContent='disconnected'}setTimeout(poll,200)}
const ex=$('expressions');expressions.forEach(name=>{const b=document.createElement('button');b.textContent=name;b.dataset.expression=name;b.onclick=()=>post('/api/expression',{name});ex.appendChild(b)});
function drive(e){const now=Date.now();if(now-lastMove<80)return;lastMove=now;const r=$('pad').getBoundingClientRect(),x=Math.max(0,Math.min(r.width,e.clientX-r.left)),y=Math.max(0,Math.min(r.height,e.clientY-r.top));post('/api/look',{yaw:(x/r.width*240-120).toFixed(1),pitch:(85-y/r.height*80).toFixed(1)})}
$('pad').addEventListener('pointerdown',e=>{$('pad').setPointerCapture(e.pointerId);drive(e)});$('pad').addEventListener('pointermove',e=>{if(e.buttons)drive(e)});$('neutral').onclick=()=>post('/api/look',{yaw:0,pitch:45});$('stop').onclick=()=>post('/api/stop');$('torque').onclick=()=>{torque=!torque;post('/api/torque',{enabled:torque?1:0});$('torque').textContent=torque?'Torque off':'Torque on'};
$('setColor').onclick=()=>{const c=$('color').value;post('/api/rgb',{r:parseInt(c.slice(1,3),16),g:parseInt(c.slice(3,5),16),b:parseInt(c.slice(5,7),16)})};
$('microphoneToggle').onclick=()=>post('/api/audio/settings',{microphone:state.microphone_enabled?0:1}).catch(showDeviceError);$('speakerToggle').onclick=()=>post('/api/audio/settings',{speaker:state.speaker_enabled?0:1}).catch(showBrowserError);
function message(error){try{return JSON.parse(error.message).error||error.message}catch{return error.message||String(error)}}function showDeviceError(error){$('deviceAudioStatus').textContent=message(error);$('deviceAudioStatus').className='audio-status error'}function showBrowserError(error){$('browserAudioStatus').textContent=message(error);$('browserAudioStatus').className='audio-status error'}
$('deviceRecord').onclick=async()=>{const status=$('deviceAudioStatus');status.className='audio-status';status.textContent='Recording on Stack-Chan…';$('deviceRecord').disabled=true;try{const response=await fetch(audioBase+'/capture?seconds=5',{cache:'no-store'});if(!response.ok)throw Error(await response.text());const blob=await response.blob();if(deviceAudioUrl)URL.revokeObjectURL(deviceAudioUrl);deviceAudioUrl=URL.createObjectURL(blob);$('deviceAudio').src=deviceAudioUrl;$('deviceAudio').hidden=false;status.textContent='Recording ready · press play'}catch(error){showDeviceError(error)}};
function combine(chunks,length){const output=new Float32Array(length);let offset=0;for(const chunk of chunks){output.set(chunk,offset);offset+=chunk.length}return output}function resample(input,sourceRate){const length=Math.min(Math.round(input.length*16000/sourceRate),160000),output=new Float32Array(length),ratio=sourceRate/16000;for(let i=0;i<length;i++){const at=i*ratio,left=Math.floor(at),right=Math.min(left+1,input.length-1),mix=at-left;output[i]=input[left]*(1-mix)+input[right]*mix}return output}function pcmWav(samples){const buffer=new ArrayBuffer(44+samples.length*2),view=new DataView(buffer),word=(at,value)=>view.setUint16(at,value,true),dword=(at,value)=>view.setUint32(at,value,true),text=(at,value)=>{for(let i=0;i<value.length;i++)view.setUint8(at+i,value.charCodeAt(i))};text(0,'RIFF');dword(4,36+samples.length*2);text(8,'WAVE');text(12,'fmt ');dword(16,16);word(20,1);word(22,1);dword(24,16000);dword(28,32000);word(32,2);word(34,16);text(36,'data');dword(40,samples.length*2);for(let i=0;i<samples.length;i++){const value=Math.max(-1,Math.min(1,samples[i]));view.setInt16(44+i*2,value<0?value*32768:value*32767,true)}return buffer}
async function sendSamples(samples,sourceRate){const status=$('browserAudioStatus');status.className='audio-status';status.textContent='Converting and sending…';const wav=pcmWav(resample(samples,sourceRate)),response=await fetch(audioBase+'/play',{method:'POST',headers:{'Content-Type':'audio/wav'},body:wav});if(!response.ok)throw Error(await response.text());status.textContent='Playing on Stack-Chan'}
async function startBrowserRecording(){if(!navigator.mediaDevices?.getUserMedia){showBrowserError(Error('Direct microphone recording requires HTTPS or localhost. Use “Choose / record audio” instead.'));return}try{const stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:true,noiseSuppression:true}}),context=new (window.AudioContext||window.webkitAudioContext)(),source=context.createMediaStreamSource(stream),processor=context.createScriptProcessor(4096,1,1),silent=context.createGain(),recording={stream,context,source,processor,silent,chunks:[],length:0,timer:0};silent.gain.value=0;processor.onaudioprocess=event=>{const chunk=new Float32Array(event.inputBuffer.getChannelData(0));recording.chunks.push(chunk);recording.length+=chunk.length};source.connect(processor);processor.connect(silent);silent.connect(context.destination);browserRecording=recording;recording.timer=setTimeout(()=>stopBrowserRecording(),10000);$('browserRecord').textContent='Stop & send';$('browserAudioStatus').className='audio-status';$('browserAudioStatus').textContent='Recording this browser…'}catch(error){showBrowserError(error)}}
async function stopBrowserRecording(){const recording=browserRecording;if(!recording)return;browserRecording=null;clearTimeout(recording.timer);recording.processor.disconnect();recording.source.disconnect();recording.silent.disconnect();recording.stream.getTracks().forEach(track=>track.stop());$('browserRecord').textContent='Start recording';try{await sendSamples(combine(recording.chunks,recording.length),recording.context.sampleRate)}catch(error){showBrowserError(error)}finally{recording.context.close()}}
$('browserRecord').onclick=()=>browserRecording?stopBrowserRecording():startBrowserRecording();$('audioFile').onchange=async event=>{const file=event.target.files[0];if(!file)return;try{const context=new (window.AudioContext||window.webkitAudioContext)(),decoded=await context.decodeAudioData(await file.arrayBuffer());await sendSamples(decoded.getChannelData(0),decoded.sampleRate);context.close()}catch(error){showBrowserError(error)}finally{event.target.value=''}};
if(!navigator.mediaDevices?.getUserMedia){$('browserAudioStatus').textContent='Direct recording needs HTTPS; the choose / record option still works.'}poll();
</script></body></html>
)HTML";

void expressionColor(Expression expression, uint8_t &red, uint8_t &green,
                     uint8_t &blue) {
  red = green = blue = 0;
  if (expression == Expression::Happy) {
    red = 24;
    green = 10;
  } else if (expression == Expression::Listening) {
    green = 20;
  } else if (expression == Expression::Thinking) {
    red = 12;
    blue = 20;
  } else if (expression == Expression::Speaking) {
    blue = 24;
  } else if (expression == Expression::Surprised) {
    red = 20;
    green = 16;
    blue = 4;
  } else if (expression == Expression::Sleepy) {
    blue = 5;
  }
}
} // namespace

bool WebControlService::begin(StackChanBoard &board, FaceRenderer &face,
                              CameraService &camera, AudioService &audio) {
  _board = &board;
  _face = &face;
  _camera = &camera;
  _audio = &audio;
  if (!connectNetwork()) {
    return false;
  }
  _camera->startStream();
  _audio->startServer();
  configureRoutes();
  _server.begin(Config::kControlHttpPort);
  if (MDNS.begin(Config::kControlHostname)) {
    MDNS.addService("http", "tcp", Config::kControlHttpPort);
  }
  _available = true;
  Serial.printf("[Web] control panel: %s\n", address().c_str());
  return true;
}

void WebControlService::update() {
  if (_available) {
    _server.handleClient();
  }
}

void WebControlService::stop() {
  if (_available) {
    _server.stop();
    MDNS.end();
  }
  if (_audio) {
    _audio->stopServer();
  }
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  _available = false;
  _accessPointMode = false;
  Serial.println("[Web] control service stopped; reboot to restore");
}

String WebControlService::address() const {
  if (!_available) {
    return "off";
  }
  const IPAddress ip = _accessPointMode ? WiFi.softAPIP() : WiFi.localIP();
  return "http://" + ip.toString();
}

bool WebControlService::connectNetwork() {
#if STACK_CHAN_HAS_WIFI_CREDENTIALS
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(Config::kControlHostname);
  WiFi.setSleep(false);
#if defined(STACK_CHAN_REUSE_WIFI_NETWORKS)
  WiFiMulti networks;
  for (int index = 0; index < WIFI_NETWORK_COUNT; ++index) {
    networks.addAP(WIFI_NETWORKS[index].ssid, WIFI_NETWORKS[index].password);
  }
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < Config::kWifiConnectTimeoutMs) {
    networks.run(1000);
    delay(50);
  }
#else
  WiFi.begin(CONTROL_WIFI_SSID, CONTROL_WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < Config::kWifiConnectTimeoutMs) {
    delay(100);
  }
#endif
  if (WiFi.status() == WL_CONNECTED) {
    _accessPointMode = false;
    Serial.printf("[WiFi] connected to %s at %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WiFi] configured network unavailable");
#endif

  if (!Config::kEnableControlAccessPoint) {
    WiFi.mode(WIFI_OFF);
    _accessPointMode = false;
    Serial.println("[WiFi] AP fallback disabled; radio off");
    return false;
  }

  WiFi.mode(WIFI_AP);
  _accessPointMode = true;
  if (!WiFi.softAP(Config::kControlApSsid, Config::kControlApPassword)) {
    Serial.println("[WiFi] access point failed");
    return false;
  }
  Serial.printf("[WiFi] AP %s at %s\n", Config::kControlApSsid,
                WiFi.softAPIP().toString().c_str());
  return true;
}

void WebControlService::configureRoutes() {
  _server.on("/", HTTP_GET, [this]() {
    _server.send_P(200, "text/html; charset=utf-8", kControlPage);
  });
  _server.on("/api/state", HTTP_GET, [this]() { sendState(); });
  _server.on("/api/expression", HTTP_POST, [this]() {
    Expression expression;
    if (!_server.hasArg("name") ||
        !parseExpression(_server.arg("name"), expression)) {
      sendJsonError(400, "invalid expression");
      return;
    }
    applyExpression(expression);
    _server.send(204);
  });
  _server.on("/api/look", HTTP_POST, [this]() {
    if (!_server.hasArg("yaw") || !_server.hasArg("pitch")) {
      sendJsonError(400, "yaw and pitch required");
      return;
    }
    _board->lookAtDegrees(_server.arg("yaw").toFloat(),
                          _server.arg("pitch").toFloat());
    _server.send(204);
  });
  _server.on("/api/stop", HTTP_POST, [this]() {
    _board->stopMotion();
    _server.send(204);
  });
  _server.on("/api/torque", HTTP_POST, [this]() {
    _board->setMotionTorqueEnabled(_server.arg("enabled") != "0");
    _server.send(204);
  });
  _server.on("/api/rgb", HTTP_POST, [this]() {
    if (!_server.hasArg("r") || !_server.hasArg("g") ||
        !_server.hasArg("b")) {
      sendJsonError(400, "r, g and b required");
      return;
    }
    _board->setRgb(constrain(_server.arg("r").toInt(), 0, 255),
                   constrain(_server.arg("g").toInt(), 0, 255),
                   constrain(_server.arg("b").toInt(), 0, 255));
    _server.send(204);
  });
  _server.on("/api/audio/settings", HTTP_POST, [this]() {
    if (_server.hasArg("microphone")) {
      _audio->setMicrophoneEnabled(_server.arg("microphone") != "0");
    }
    if (_server.hasArg("speaker")) {
      _audio->setSpeakerEnabled(_server.arg("speaker") != "0");
    }
    if (!_server.hasArg("microphone") && !_server.hasArg("speaker")) {
      sendJsonError(400, "microphone or speaker required");
      return;
    }
    _server.send(204);
  });
  _server.onNotFound([this]() { sendJsonError(404, "not found"); });
}

void WebControlService::sendState() {
  const auto &touch = _board->topTouchIntensities();
  String json;
  json.reserve(440);
  json += "{\"battery_voltage\":" + String(_board->batteryVoltage(), 3);
  json += ",\"battery_current\":" + String(_board->batteryCurrent(), 3);
  json += ",\"battery_percent\":" + String(_board->batteryPercent());
  json += ",\"yaw\":" + String(_board->yawTenths() / 10.0f, 1);
  json += ",\"pitch\":" + String(_board->pitchTenths() / 10.0f, 1);
  json += ",\"moving\":" + String(_board->motionIsMoving() ? "true" : "false");
  json += ",\"expression\":\"" + String(expressionName(_face->expression())) + "\"";
  json += ",\"touch\":[" + String(touch[0]) + "," + String(touch[1]) + "," + String(touch[2]) + "]";
  json += ",\"camera\":" + String(_camera->available() ? "true" : "false");
  json += ",\"audio_available\":" +
          String(_audio->available() ? "true" : "false");
  json += ",\"microphone_enabled\":" +
          String(_audio->microphoneEnabled() ? "true" : "false");
  json += ",\"speaker_enabled\":" +
          String(_audio->speakerEnabled() ? "true" : "false");
  json += ",\"audio_recording\":" +
          String(_audio->recording() ? "true" : "false");
  json += ",\"audio_playing\":" +
          String(_audio->playing() ? "true" : "false");
  json += ",\"mode\":\"" + String(_accessPointMode ? "access point" : "wifi") + "\"";
  json += ",\"ip\":\"" + String(_accessPointMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
  json += ",\"uptime_ms\":" + String(millis()) + "}";
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", json);
}

void WebControlService::sendJsonError(int status, const char *message) {
  _server.send(status, "application/json",
               "{\"error\":\"" + String(message) + "\"}");
}

void WebControlService::applyExpression(Expression expression) {
  _face->setExpression(expression);
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  expressionColor(expression, red, green, blue);
  _board->setRgb(red, green, blue);
}

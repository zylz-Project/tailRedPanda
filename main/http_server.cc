#include "http_server.h"
#include "config.h"
#include "flash_upload_server.h"
#include "power.h"
#include "servo.h"
#include "wifi.h"
#include "wifi_config.h"
#include "chat.h"
#if ENABLE_AUTO_RUN
#include "auto_run.h"
#endif

#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

httpd_handle_t g_http_server = nullptr;
static const char *TAG = "tailpanda";

// ==================== Web Page ====================
static const char kHtml[] = R"raw(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>TailPanda</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#1a1a2e;color:#eee;padding:8px;max-width:520px;margin:auto}
h1{text-align:center;font-size:16px;color:#e94560;margin:6px 0}
.card{background:#16213e;border-radius:8px;padding:10px;margin-bottom:8px}
h2{font-size:13px;margin-bottom:5px;color:#4ecca3}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
.col{flex:1;min-width:60px}
.btn{padding:8px 14px;border:none;border-radius:4px;font-size:13px;cursor:pointer;color:#fff;margin:3px}
.btn-on{background:#2ecc71;color:#000}
.btn-off{background:#555}
.btn-act{background:#e67e22}
.btn-preset{background:#3498db}
input,select{width:100%;padding:6px;border:1px solid #333;border-radius:4px;background:#0f3460;color:#eee;font-size:13px;margin:2px 0}
input[type=range]{-webkit-appearance:none;width:100%;height:28px;background:linear-gradient(90deg,#0f3460,#e94560);border-radius:4px;margin:4px 0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;background:#e94560;border-radius:50%}
.grp-head{background:#1a2e3a;border-left:3px solid #4ea3cc}
.grp-tail{background:#1a3a2e;border-left:3px solid #4ecca3}
.lbl{display:flex;justify-content:space-between;font-size:11px;color:#aaa}
.val{font-size:20px;font-weight:bold;color:#4ecca3;text-align:center;min-width:50px}
.grp-head .val{color:#4ea3cc}
.joy-row{display:flex;gap:12px;align-items:center;justify-content:center}
.joy{background:#0a0f1e;border-radius:50%;cursor:pointer;touch-action:none;display:block;border:3px solid #4ecca3;box-shadow:0 0 12px rgba(78,204,163,0.25)}
.joy-info{display:flex;flex-direction:column;gap:6px;min-width:70px}
.joy-lbl{font-size:11px;color:#aaa;text-align:center}
.joy-lbl .val{font-size:22px;font-weight:bold;display:block}
.status{padding:4px;text-align:center;font-size:12px;color:#888;min-height:20px}
</style>
</head>
<body>
<h1>TailPanda</h1>
<div class="status" id="status">就绪</div>

<!-- WIFI -->
<div class="card" id="wifiCard">
<h2>WiFi</h2>
<!-- 已连接: 只显示状态 + 重新配网 -->
<div class="row" style="justify-content:space-between;display:none" id="wifiConnected">
<span style="font-size:12px;color:#aaa" id="wifiInfo">--</span>
<button type="button" class="btn btn-preset" onclick="enterPortal()">重新配网</button>
</div>
<!-- 未连接/配网: 显示完整表单 -->
<div id="wifiForm">
<div class="row" style="justify-content:space-between">
<span style="font-size:12px;color:#aaa" id="wifiInfo2">--</span>
</div>
<button type="button" class="btn btn-preset" style="width:100%;margin-top:6px" onclick="scanWifi()">扫描附近 WiFi</button>
<div id="wifiList" style="margin-top:6px;min-height:24px"></div>
<div class="row" style="margin-top:6px">
<div class="col" style="flex:1"><input id="wSsid" placeholder="WiFi SSID" style="width:100%"></div>
</div>
<div class="row">
<div class="col" style="flex:1"><input id="wPass" placeholder="WiFi 密码" type="text" style="width:100%"></div>
</div>
<div class="row">
<button type="button" class="btn btn-on" style="width:100%" onclick="saveWifi()">连接此 WiFi</button>
</div>
</div>
</div>

<!-- HEAD -->
<div class="card grp-head">
<h2>Head  头部 <span style="font-size:10px;color:#888">IO18 | 0°=右转 90°=正中 180°=左转</span></h2>
<div class="lbl"><span>头部角度</span><span class="val" id="v0">90°</span></div>
<input type="range" id="s0" min="0" max="180" value="90" oninput="onSlider()">
<div class="row" style="margin-top:4px">
<button class="btn btn-off" onclick="setServo(0,0)">右转 0°</button>
<button class="btn btn-on" onclick="setServo(0,90)">正中 90°</button>
<button class="btn btn-off" onclick="setServo(0,180)">左转 180°</button>
</div>
</div>

<!-- TAIL -->
<div class="card grp-tail">
<h2>Tail 尾巴 <span style="font-size:10px;color:#888">IO15=LR(左右) IO16=UD(上下) | 0°=最左/最上 180°=最右/最下</span></h2>

<div class="row">
<div class="col">
<div class="lbl"><span>左右 LR</span><span class="val" id="v1">90°</span></div>
<input type="range" id="s1" min="0" max="180" value="90" oninput="onSlider()">
</div>
<div class="col">
<div class="lbl"><span>上下 UD</span><span class="val" id="v2">180°</span></div>
<input type="range" id="s2" min="0" max="180" value="180" oninput="onSlider()">
</div>
</div>

<h3 style="font-size:11px;color:#f39c12;margin-top:8px">Tail Joystick  摇杆</h3>
<div class="joy-row">
<canvas class="joy" id="joyTail" width="180" height="180"></canvas>
<div class="joy-info">
<div class="joy-lbl"><span>LR</span><span class="val" id="v1j">90°</span></div>
<div class="joy-lbl"><span>UD</span><span class="val" id="v2j">180°</span></div>
</div>
</div>
<div class="row" style="margin-top:4px">
<button class="btn btn-preset" onclick="tailJoy.setAngles(0,180)">最左上</button>
<button class="btn btn-on" onclick="tailJoy.setAngles(90,180)">正中下</button>
<button class="btn btn-preset" onclick="tailJoy.setAngles(180,180)">最右下</button>
</div>
<div class="row" style="margin-top:3px">
<button class="btn btn-preset" onclick="tailJoy.setAngles(90,0)">正中上</button>
<button class="btn btn-preset" onclick="tailJoy.setAngles(0,90)">最左中</button>
<button class="btn btn-preset" onclick="tailJoy.setAngles(180,90)">最右中</button>
</div>
</div>

<!-- ACTION PRESETS -->
<div class="card">
<h2>动作预设</h2>
<div class="row">
<button class="btn btn-on" onclick="preset(90,90,180)">默认姿态</button>
<button class="btn btn-act" onclick="doBreath()">呼吸</button>
</div>
<div class="row" style="margin-top:3px">
<button class="btn btn-act" onclick="doTailWag()">摇尾巴</button>
<button class="btn btn-act" onclick="doTailCircle()">画圈</button>
<button class="btn btn-act" onclick="doLookAround()">环顾</button>
</div>
</div>

<!-- AUTO RUN -->
<div class="card">
<h2>自运行动画</h2>
<div class="row">
<button class="btn" id="btnAutoPlay" style="background:#2ecc71;color:#000" onclick="toggleAutoPlay()">自运行动画: ON</button>
<button class="btn" id="btnHardSwing" style="background:#333;color:#fff" onclick="toggleHardSwing()">曲线: sin²</button>
</div>
</div>

<!-- BATTERY -->
<div class="card">
<div style="display:flex;align-items:center;gap:8px;justify-content:center">
<span style="font-size:11px;color:#aaa">🔋 电量</span>
<span id="batteryLevel" style="font-size:16px;font-weight:bold;color:#4ecca3">--%</span>
<span id="batteryMv" style="font-size:10px;color:#888">--mV</span>
</div>
</div>

<script>
// --- API helper ---
async function api(url, body){
  try{
    let o = body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {method:'GET'};
    let r = await fetch(url, o);
    return await r.text();
  }catch(e){ setStatus('连接失败','#e94560'); return null; }
}

function $(id){return document.getElementById(id)}
function setStatus(m,c){ let s=$('status'); s.textContent=m; s.style.color=c||'#888'; }

// --- Slider → servo ---
function getAngles(){
  return [parseInt($('s0').value), parseInt($('s1').value), parseInt($('s2').value)];
}

async function sendAngles(){
  let a = getAngles();
  $('v0').textContent = a[0]+'°';
  $('v1').textContent = a[1]+'°';
  $('v2').textContent = a[2]+'°';
  $('v1j').textContent = a[1]+'°';
  $('v2j').textContent = a[2]+'°';
  await api('/api/servo', {angles:a});
}

function onSlider(){ sendAngles(); }

function setServo(idx, val){
  let ids = ['s0','s1','s2'];
  $(ids[idx]).value = val;
  sendAngles();
}

function preset(h, lr, ud){
  $('s0').value=h; $('s1').value=lr; $('s2').value=ud;
  tailJoy.setAngles(lr, ud);
  sendAngles();
}

// --- Tail Joystick ---
class TailJoystick {
  constructor(canvasId){
    this.canvas = $(canvasId);
    this.ctx = this.canvas.getContext('2d');
    this.cx=90; this.cy=90; this.r=78;
    this.kx=90; this.ky=90+78; this.kr=22; // default: center-bottom
    this.active=false; this.animId=0;
    this.canvas.addEventListener('pointerdown',e=>{cancelAnimationFrame(this.animId);this.active=true;this.move(e);});
    this.canvas.addEventListener('pointermove',e=>{if(this.active)this.move(e);});
    this.canvas.addEventListener('pointerup',()=>{this.active=false;this.springBack();});
    this.canvas.addEventListener('pointerleave',()=>{this.active=false;this.springBack();});
    this.canvas.addEventListener('pointercancel',()=>{this.active=false;this.springBack();});
    this.draw();
  }
  move(e){
    e.preventDefault();
    let rect=this.canvas.getBoundingClientRect();
    let sx=this.canvas.width/rect.width, sy=this.canvas.height/rect.height;
    let mx=(e.clientX-rect.left)*sx, my=(e.clientY-rect.top)*sy;
    let dx=mx-this.cx, dy=my-this.cy;
    let dist=Math.sqrt(dx*dx+dy*dy);
    if(dist>this.r){dx*=this.r/dist;dy*=this.r/dist;}
    this.kx=this.cx+dx; this.ky=this.cy+dy;
    this.updateAngles();
    this.draw();
  }
  springBack(){
    // Return to center-bottom (LR=90, UD=180)
    let tx=this.cx, ty=this.cy+this.r;
    let ease=t=>t<0.5?4*t*t*t:1-Math.pow(-2*t+2,3)/2;
    let sx=this.kx, sy=this.ky;
    let dx=tx-sx, dy=ty-sy;
    if(Math.abs(dx)<0.3&&Math.abs(dy)<0.3){
      this.kx=tx; this.ky=ty;
      this.updateAngles();
      this.draw();
      this.animId=0;
      return;
    }
    let t0=performance.now(), dur=250;
    let step=(now)=>{
      let t=Math.min(1,(now-t0)/dur);
      this.kx=sx+dx*ease(t); this.ky=sy+dy*ease(t);
      this.updateAngles(); this.draw();
      if(t<1)this.animId=requestAnimationFrame(step);
      else{this.kx=tx;this.ky=ty;this.updateAngles();this.draw();this.animId=0;}
    };
    this.animId=requestAnimationFrame(step);
  }
  updateAngles(){
    // Map joystick to servo angles
    // X axis: left→right  maps to LR 0→180
    // Y axis: top→bottom  maps to UD 0→180
    let dx=(this.kx-this.cx)/this.r;
    let dy=(this.ky-this.cy)/this.r;
    let lr=90+Math.round(dx*90);        // -1→0→+1 → 0°→90°→180°
    let ud=90+Math.round(dy*90);        // -1→0→+1 → 0°→90°→180°
    lr=Math.max(0,Math.min(180,lr));
    ud=Math.max(0,Math.min(180,ud));
    $('s1').value=lr; $('s2').value=ud;
    sendAngles();
  }
  draw(){
    let c=this.ctx, w=this.canvas.width, h=this.canvas.height;
    c.clearRect(0,0,w,h);
    // Outer ring
    c.beginPath(); c.arc(this.cx,this.cy,this.r,0,Math.PI*2);
    c.strokeStyle='rgba(78,204,163,0.3)'; c.lineWidth=2; c.stroke();
    // Crosshairs
    c.beginPath(); c.moveTo(this.cx-this.r,this.cy); c.lineTo(this.cx+this.r,this.cy);
    c.moveTo(this.cx,this.cy-this.r); c.lineTo(this.cx,this.cy+this.r);
    c.strokeStyle='rgba(255,255,255,0.08)'; c.lineWidth=1; c.stroke();
    // Labels
    c.font='10px monospace';
    c.fillStyle='#888'; c.fillText('左',this.cx-this.r+2,this.cy+4);
    c.fillText('右',this.cx+this.r-14,this.cy+4);
    c.fillText('上',this.cx-2,this.cy-this.r+12);
    c.fillText('下',this.cx-2,this.cy+this.r-4);
    // Default position marker (center-bottom)
    c.beginPath(); c.arc(this.cx,this.cy+this.r,3,0,Math.PI*2);
    c.fillStyle='rgba(255,255,255,0.2)'; c.fill();
    // Center dot
    c.beginPath(); c.arc(this.cx,this.cy,3,0,Math.PI*2);
    c.fillStyle='rgba(255,255,255,0.1)'; c.fill();
    // Knob
    c.beginPath(); c.arc(this.kx,this.ky,this.kr,0,Math.PI*2);
    let grad=c.createRadialGradient(this.kx-5,this.ky-5,3,this.kx,this.ky,this.kr);
    grad.addColorStop(0,'#4ecca3'); grad.addColorStop(1,'#2e8b57');
    c.fillStyle=grad; c.fill();
    c.strokeStyle='rgba(255,255,255,0.4)'; c.lineWidth=2; c.stroke();
  }
  setAngles(lr, ud){
    cancelAnimationFrame(this.animId);
    let dx=(lr-90)/90;
    let dy=(ud-90)/90;
    this.kx=this.cx+dx*this.r;
    this.ky=this.cy+dy*this.r;
    this.draw();
  }
}
let tailJoy = new TailJoystick('joyTail');

// --- Action animations (local, browser-side) ---
let actionRunning = false;

async function doBreath(){
  if(actionRunning) return;
  actionRunning = true;
  setStatus('呼吸中...', '#4ecca3');
  for(let i=0; i<45 && actionRunning; i++){
    let t = (i * 100) / 1000;
    let h = 90 + Math.round(15 * Math.sin(t * 2 * Math.PI / 4.5));
    let lr = 90 + Math.round(15 * Math.sin(t * 2 * Math.PI / 4.5));
    let ud = 155 + Math.round(22 * Math.sin((t + 1.1) * 2 * Math.PI / 4.5));
    $('s0').value=h; $('s1').value=lr; $('s2').value=ud;
    await api('/api/servo', {angles:[h,lr,ud]});
    await new Promise(r=>setTimeout(r,100));
  }
  if(actionRunning) preset(90,90,180);
  actionRunning = false;
  setStatus('就绪');
}

async function doTailWag(){
  if(actionRunning) return;
  actionRunning = true;
  setStatus('摇尾巴!', '#e67e22');
  for(let i=0; i<40 && actionRunning; i++){
    let lr = 90 + Math.round(50 * Math.sin(i * 0.4));
    $('s0').value=90; $('s1').value=lr; $('s2').value=180;
    await api('/api/servo', {angles:[90,lr,180]});
    await new Promise(r=>setTimeout(r,60));
  }
  if(actionRunning) preset(90,90,180);
  actionRunning = false;
  setStatus('就绪');
}

async function doTailCircle(){
  if(actionRunning) return;
  actionRunning = true;
  setStatus('画圈中...', '#f39c12');
  for(let i=0; i<60 && actionRunning; i++){
    let t = i * 0.12;
    let lr = 90 + Math.round(45 * Math.sin(t));
    let ud = 140 + Math.round(45 * Math.cos(t));
    $('s0').value=90; $('s1').value=lr; $('s2').value=ud;
    await api('/api/servo', {angles:[90,lr,ud]});
    await new Promise(r=>setTimeout(r,80));
  }
  if(actionRunning) preset(90,90,180);
  actionRunning = false;
  setStatus('就绪');
}

async function doLookAround(){
  if(actionRunning) return;
  actionRunning = true;
  setStatus('环顾四周...', '#3498db');
  for(let c=0; c<3 && actionRunning; c++){
    // Sweep right to left
    for(let i=0; i<30 && actionRunning; i++){
      let h = Math.round(90 + 60 * Math.sin(i/30 * Math.PI));
      $('s0').value=h;
      await api('/api/servo', {angles:[h,90,180]});
      await new Promise(r=>setTimeout(r,60));
    }
    // Sweep left to right
    for(let i=0; i<30 && actionRunning; i++){
      let h = Math.round(90 - 60 * Math.sin(i/30 * Math.PI));
      $('s0').value=h;
      await api('/api/servo', {angles:[h,90,180]});
      await new Promise(r=>setTimeout(r,60));
    }
  }
  if(actionRunning) preset(90,90,180);
  actionRunning = false;
  setStatus('就绪');
}

// --- WiFi ---
function wifiShowForm(show){
  $('wifiForm').style.display = show ? 'block' : 'none';
  $('wifiConnected').style.display = show ? 'none' : 'flex';
}
async function updateWifiInfo(){
  let r = await api('/api/wifi');
  if(!r) return;
  try{
    let w = JSON.parse(r);
    if(w.portal || !w.connected){
      // 未连接/配网模式: 显示完整配网表单
      $('wifiInfo2').textContent = w.portal ? '配网模式: 连接 Panda-XXXX → 192.168.4.1' : '未连接 WiFi, 请配置';
      wifiShowForm(true);
    } else {
      // 已连接: 只显示状态 + 重新配网
      $('wifiInfo').textContent = '已连接 ' + w.ssid + ' ' + w.ip;
      wifiShowForm(false);
    }
  }catch(e){}
}
async function enterPortal(){
  setStatus('正在进入配网模式...','#3498db');
  await api('/api/wifi/portal', {});
  setStatus('配网模式已开启：手机连接 Panda-XXXX 热点，访问 192.168.4.1', '#3498db');
}
async function scanWifi(){
  setStatus('正在扫描附近 WiFi...','#4ecca3');
  $('wifiList').innerHTML = '<div style="color:#888;font-size:12px;padding:4px">扫描中，请稍候...</div>';
  let r = await api('/api/wifi/scan');
  setStatus('扫描完成','#4ecca3');
  if(!r){ $('wifiList').innerHTML='<div style="color:#e94560;font-size:12px">扫描失败，请重试</div>'; return; }
  try{
    let list = JSON.parse(r);
    if(!list.length){ $('wifiList').innerHTML='<div style="color:#888;font-size:12px">未发现 WiFi</div>'; return; }
    let html='';
    for(let a of list){
      if(!a.ssid) continue;
      let pad = a.rssi>=-65?'#2ecc71':(a.rssi>=-80?'#f39c12':'#e94560');
      html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 4px;border-bottom:1px solid #1a2a4a;font-size:13px" onclick="pickWifi(\''+a.ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">'+
              '<span>'+a.ssid+'</span><span style="color:'+pad+'">'+a.rssi+'dBm</span></div>';
    }
    $('wifiList').innerHTML = html;
    setStatus('点击列表选择 WiFi','#4ecca3');
  }catch(e){ setStatus('扫描结果解析失败','#e94560'); }
}
function pickWifi(ssid){ $('wSsid').value = ssid; $('wPass').focus(); }
async function saveWifi(){
  let btn = document.querySelector('#wifiForm button[onclick="saveWifi()"]');
  let ssid = $('wSsid').value.trim();
  let pass = $('wPass').value.trim();
  if(!ssid){ setStatus('请输入 SSID','#e94560'); $('wSsid').focus(); return; }
  if(btn) btn.disabled = true;
  setStatus('正在保存并连接 '+ssid+'...','#4ecca3');
  let r = await api('/api/wifi/configure', {ssid:ssid, password:pass});
  setStatus('已保存！正在连接新网络，热点稍后自动关闭...', '#4ecca3');
  if(btn) btn.disabled = false;
}

(async function(){
  await updateWifiInfo();
})();

// --- Battery ---
async function updateBattery(){
  let r = await api('/api/battery');
  if(r){
    try{
      let b = JSON.parse(r);
      $('batteryLevel').textContent = b.level + '%';
      $('batteryMv').textContent = (b.voltage_mv/1000).toFixed(2) + 'V';
      $('batteryLevel').style.color = b.level > 20 ? '#4ecca3' : '#e94560';
    }catch(e){}
  }
}
setInterval(updateBattery, 5000);
updateBattery();

// --- Auto Play ---
async function toggleAutoPlay(){
  let r = await api('/api/autoplay', {});
  if(r){ try{ let s=JSON.parse(r); refreshAutoPlayUI(s); }catch(e){} }
}
async function toggleHardSwing(){
  let r0 = await api('/api/autoplay');
  let cur=false;
  if(r0){ try{cur=JSON.parse(r0).hard_swing;}catch(e){} }
  let r=await api('/api/autoplay', {hard_swing:!cur});
  if(r){ try{ let s=JSON.parse(r); refreshAutoPlayUI(s); }catch(e){} }
}
function refreshAutoPlayUI(s){
  let btn=$('btnAutoPlay');
  btn.textContent='自运行动画: '+(s.autoplay?'ON':'OFF');
  btn.style.background=s.autoplay?'#2ecc71':'#555';
  btn.style.color=s.autoplay?'#000':'#fff';
  let hbtn=$('btnHardSwing');
  hbtn.textContent='曲线: '+(s.hard_swing?'硬摆':'sin²');
  hbtn.style.background=s.hard_swing?'#e67e22':'#333';
}
(async function(){
  let r=await api('/api/autoplay');
  if(r){ try{let s=JSON.parse(r);refreshAutoPlayUI(s);}catch(e){} }
})();
</script>
</body>
</html>
)raw";

// ==================== HTTP Handlers ====================

static esp_err_t HandleRoot(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_sendstr(req, kHtml);
    return ESP_OK;
}

static esp_err_t HandleServo(httpd_req_t *req)
{
    char buf[256] = {};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[ret] = 0;

    int angles[3] = {90, 90, 180};
    const char *p = strstr(buf, "\"angles\":");
    if (p) {
        p += 9;
        for (int i = 0; i < kServoCount; i++) {
            while (*p == ' ' || *p == '[' || *p == ',') p++;
            angles[i] = atoi(p);
            while (*p && *p != ',' && *p != ']') p++;
        }
    }
    for (int i = 0; i < kServoCount; i++)
        SetServoAngle(i, angles[i]);

#if ENABLE_AUTO_RUN
    if (IsAutoRunRunning()) {
        SetAutoRunRunning(false);
        ESP_LOGI(TAG, "Auto-play paused by manual servo command");
    }
#endif

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t HandleBattery(httpd_req_t *req)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"voltage_mv\":%d,\"level\":%d}",
             GetBatteryVoltageMv(), GetBatteryLevel());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiStatus(httpd_req_t *req)
{
    char buf[160];
    const char *ssid = "unknown";
    {
        char s[33] = {}, p[65] = {};
        if (WifiConfigGetCredentials(s, sizeof(s), p, sizeof(p)) && s[0]) ssid = "saved";
#ifdef WIFI_STA_SSID
        else ssid = WIFI_STA_SSID;
#else
        else ssid = "none";
#endif
    }
    bool online = (strcmp(WiFiIP(), "0.0.0.0") != 0);
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"portal\":%s}",
             online ? "true" : "false", ssid, WiFiIP(),
             WifiConfigPortalRunning() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiPortal(httpd_req_t *req)
{
    httpd_req_recv(req, nullptr, 0); /* discard body */
    WifiConfigEnterFromWeb();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "portal started");
    return ESP_OK;
}

static esp_err_t HandleWifiScan(httpd_req_t *req)
{
    char buf[1200];
    WifiConfigScanAps(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t HandleWifiConfigure(httpd_req_t *req)
{
    char body[512] = {};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = 0;

    char ssid[33] = {}, pass[65] = {};
    const char *ps = strstr(body, "\"ssid\"");
    const char *pp = strstr(body, "\"password\"");
    if (ps) {
        ps = strchr(ps, ':'); ps = strchr(ps, '"');
        const char *e = strchr(ps + 1, '"');
        size_t n = (e && e > ps) ? (size_t)(e - ps - 1) : 0;
        if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
        memcpy(ssid, ps + 1, n);
    }
    if (pp) {
        pp = strchr(pp, ':'); pp = strchr(pp, '"');
        const char *e = strchr(pp + 1, '"');
        size_t n = (e && e > pp) ? (size_t)(e - pp - 1) : 0;
        if (n >= sizeof(pass)) n = sizeof(pass) - 1;
        memcpy(pass, pp + 1, n);
    }
    if (!ssid[0]) { httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "no ssid"); return ESP_OK; }

    WifiConfigSaveCredentials(ssid, pass);

    /* 先回响应, 让手机收到"已保存"再关热点, 避免页面加载中断乱跳 */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    vTaskDelay(pdMS_TO_TICKS(800));

    WifiConfigStopPortal();  /* 保存后切回 STA 连接 */
    return ESP_OK;
}

// ==================== Server startup ====================

void StartHttpServer()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    httpd_handle_t server = nullptr;
    httpd_start(&server, &cfg);
    g_http_server = server;

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &root);

    httpd_uri_t servo = {.uri = "/api/servo", .method = HTTP_POST, .handler = HandleServo, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &servo);

    httpd_uri_t battery = {.uri = "/api/battery", .method = HTTP_GET, .handler = HandleBattery, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &battery);

    httpd_uri_t wifi_status = {.uri = "/api/wifi", .method = HTTP_GET, .handler = HandleWifiStatus, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &wifi_status);
    httpd_uri_t wifi_portal = {.uri = "/api/wifi/portal", .method = HTTP_POST, .handler = HandleWifiPortal, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &wifi_portal);
    httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = HandleWifiScan, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &wifi_scan);
    httpd_uri_t wifi_cfg = {.uri = "/api/wifi/configure", .method = HTTP_POST, .handler = HandleWifiConfigure, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &wifi_cfg);

    /* 全局 404 → 重定向到首页: 配合 DNS 劫持实现 captive portal,
     * 手机连上 Panda-XXXX 后系统探测任何 URL 都会落到首页, 触发"登录网络"弹窗 */
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, [](httpd_req_t *req, httpd_err_code_t err) -> esp_err_t {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_sendstr(req, "<html><body>redirecting...</body></html>");
        return ESP_OK;
    });

#if ENABLE_AUTO_RUN
    httpd_uri_t autoplay_get  = {.uri = "/api/autoplay", .method = HTTP_GET,  .handler = HandleAutoPlay, .user_ctx = nullptr};
    httpd_uri_t autoplay_post = {.uri = "/api/autoplay", .method = HTTP_POST, .handler = HandleAutoPlay, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &autoplay_get);
    httpd_register_uri_handler(server, &autoplay_post);
#endif

    flash_upload_server_register();

    ESP_LOGI(TAG, "HTTP server started");
}

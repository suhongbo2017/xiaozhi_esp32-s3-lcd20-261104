#include "admin_server.h"

#include "audio_codec.h"
#include "application.h"
#include "device/gpio_control.h"
#include "mcp_server.h"
#include "asset_ota.h"
#include "ui/standby_service.h"
#include "ui/user_image.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "music/music_api_client.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <cstring>
#include <string>

#define TAG "AdminServer"

namespace {

httpd_handle_t g_server = nullptr;

constexpr const char* kHtml = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小智设备管理</title>
<style>
:root{color-scheme:dark}
body{margin:0;font:15px/1.6 -apple-system,Segoe UI,Roboto,sans-serif;background:#0e1e33;color:#e8f1ff}
.wrap{max-width:560px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:20px;margin:8px 0 4px}
.sub{color:#8fa6bf;font-size:13px;margin-bottom:18px}
.card{background:#13273f;border:1px solid #1e3a58;border-radius:14px;padding:16px;margin:14px 0}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:12px 0}
label{color:#b9cde4}
input[type=range]{width:100%}
input[type=text]{flex:1;background:#0b1a2a;border:1px solid #1e3a58;color:#e8f1ff;border-radius:10px;padding:10px}
select{background:#0b1a2a;border:1px solid #1e3a58;color:#e8f1ff;border-radius:10px;padding:8px 10px}
input[type=color]{width:46px;height:32px;border:none;background:none}
button{width:100%;padding:13px;border:none;border-radius:12px;background:#00a2ff;color:#04121f;font-weight:700;font-size:16px;margin-top:6px}
button:active{transform:scale(.99)}
.ok{color:#37e39b}.warn{color:#ffcf5c}
.tip{color:#8fa6bf;font-size:12px;margin-top:6px}
.val{color:#00e5ff;font-variant-numeric:tabular-nums}
pre{background:#0b1a2a;border-radius:10px;padding:10px;font-size:12px;overflow:auto;max-height:150px}
.tabs{display:flex;gap:6px;background:#0b1a2a;border:1px solid #1e3a58;border-radius:12px;padding:5px;margin:0 0 6px;position:sticky;top:0;z-index:9}
.tabs button{flex:1;margin:0;padding:9px 4px;background:transparent;color:#9fb6cf;font-size:13px;font-weight:600;border-radius:9px}
.tabs button.on{background:#00a2ff;color:#04121f}
.pane{display:none}.pane.on{display:block}
.imgbox{width:100%;aspect-ratio:3/4;background:#0b1a2a;border:1px dashed #2b4d70;border-radius:12px;
 display:flex;align-items:center;justify-content:center;overflow:hidden;margin:10px 0;color:#5c7490;font-size:13px}
.imgbox img{width:100%;height:100%;object-fit:cover}
.btnrow{display:flex;gap:8px}
.btnrow button{flex:1}
button.ghost{background:#1b3350;color:#cfe3f7}
.dev-card{background:#14263b;border:1px solid #22405e;border-radius:14px;padding:14px;margin:12px 0}
.dev-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.dev-name{font-size:17px;font-weight:700}
.dev-sub{color:#8fa6bf;font-size:12px;margin-top:4px}
.sw{width:58px;height:32px;border-radius:16px;background:#2b4d70;position:relative;cursor:pointer;
    transition:background .18s;flex:0 0 auto;border:none;padding:0}
.sw.on{background:#37e39b}
.sw::after{content:'';position:absolute;top:3px;left:3px;width:26px;height:26px;border-radius:50%;
    background:#fff;transition:left .18s}
.sw.on::after{left:29px}
.dev-actions{display:flex;justify-content:space-between;align-items:center;margin-top:12px}
.dev-state{font-size:13px}
.dev-state.on{color:#37e39b}.dev-state.off{color:#8fa6bf}
.btn-del{background:#3a2028;color:#ff9a9a;border:none;border-radius:10px;padding:8px 14px;font-size:13px;width:auto;margin:0}
.modal{position:fixed;inset:0;background:rgba(4,10,18,.86);display:none;align-items:center;justify-content:center;z-index:50;padding:14px}
.modal.on{display:flex}
.modal-box{background:#12253c;border:1px solid #24425f;border-radius:16px;padding:14px;width:100%;max-width:340px}
.modal-title{font-weight:700;margin-bottom:10px}
.crop-wrap{width:100%;display:flex;justify-content:center;margin-bottom:10px;touch-action:none}
#crop_canvas{width:210px;height:280px;border-radius:10px;background:#0b1a2a;box-shadow:0 0 0 1px #2b4d70 inset;cursor:grab}
#crop_canvas.dragging{cursor:grabbing}
}}
</style>
</style></head><body><div class="wrap">
<h1>小智设备管理</h1>
<div class="sub" id="dev">连接中…</div>

<div class="tabs">
  <button data-tab="display" class="on">显示</button>
  <button data-tab="look">外观</button>
  <button data-tab="sound">声音</button>
  <button data-tab="led">灯光</button>
  <button data-tab="gpio">开关</button>
  <button data-tab="sys">系统</button>
</div>

<!-- ============ 显示 ============ -->
<div class="pane on" id="pane-display">
  <div class="card">
    <div class="row"><label>主题</label>
      <select id="theme"><option value="dark">深色</option><option value="light">浅色</option></select>
    </div>
    <div class="row"><label>屏幕亮度 <span class="val" id="bv">-</span></label></div>
    <input type="range" id="brightness" min="0" max="100" step="1">
  </div>

  <div class="card">
    <div class="row"><label><b>待机页</b>（时钟 / 天气 / 指数）</label>
      <select id="standby"><option value="1">开启</option><option value="0">关闭</option></select>
    </div>
    <div class="row"><label>城市</label><input type="text" id="standby_city" placeholder="上海"></div>
    <div class="row"><label>指数 / 个股</label></div>
    <input type="text" id="standby_symbols" placeholder="usIXIC,usINX,sh000001">
    <div class="tip">默认 纳指 / 标普 / 上证；也支持 sh600519、hk00700、usNVDA，逗号分隔最多 4 个</div>
  </div>

  <div class="card">
    <div class="row"><label><b>待机背景图</b></label></div>
    <div class="imgbox" id="bg_box">未设置（使用深色渐变）</div>
    <div class="btnrow">
      <button onclick="document.getElementById('bg_file').click()">选择图片</button>
      <button class="ghost" onclick="clearImage('bg')">清除</button>
    </div>
    <input type="file" id="bg_file" accept="image/*" style="display:none">
    <div class="tip" id="bg_msg"></div>
    <div class="tip">上传后自动裁成 240×320、JPEG 压缩（约 20KB），显示在待机页文字下方</div>
  </div>
</div>

<!-- ============ 外观 ============ -->
<div class="pane" id="pane-look">
  <div class="card">
    <div class="row"><label><b>开机图片</b></label></div>
    <div class="imgbox" id="boot_box">未设置（开机直接进对话界面）</div>
    <div class="btnrow">
      <button onclick="document.getElementById('boot_file').click()">选择图片</button>
      <button class="ghost" onclick="clearImage('boot')">清除</button>
    </div>
    <input type="file" id="boot_file" accept="image/*" style="display:none">
    <div class="tip" id="boot_msg"></div>
    <div class="tip">开机显示约 2 秒，然后进入正常界面</div>
  </div>

  <div class="card">
    <div class="row"><label>字体 / 表情资源包</label>
      <select id="variant">
        <option value="a20_e64" selected>20px 字体 + 64px 表情（推荐）</option>
        <option value="a16_e32">16px 字体 + 32px 表情（省空间）</option>
        <option value="a30_e64">30px 字体 + 64px 表情（大字）</option>
      </select>
    </div>
    <button id="applyAssets" style="background:#ff9f43">应用并重启</button>
    <div class="tip" id="amsg"></div>
    <div class="tip">下载资源包写入 assets 分区并自动重启（约 5~20 秒），不影响上面两张图片</div>
  </div>
</div>

<!-- ============ 声音 ============ -->
<div class="pane" id="pane-sound">
  <div class="card">
    <div class="row"><label><b>唤醒词</b> <span class="val" id="ww_name"></span></label>
      <select id="wake_enabled"><option value="1">开启</option><option value="0">关闭</option></select>
    </div>
    <div class="row"><label>灵敏度 <span class="val" id="wtv">-</span></label></div>
    <input type="range" id="wake_threshold" min="40" max="99" step="1">
    <div class="tip">数值越小越容易唤醒（也更容易误唤醒）；0.40~0.99 生效</div>
    <div class="tip warn" id="wake_warn" style="display:none">关闭唤醒词后，只能用 BOOT 键开始/结束说话</div>
  </div>

  <div class="card">
    <div class="row"><label>音量 <span class="val" id="vv">-</span></label></div>
    <input type="range" id="volume" min="0" max="100" step="1">
    <div class="row"><label>音乐播放时音量比例 <span class="val" id="dv">-</span>%</label></div>
    <input type="range" id="duck" min="30" max="100" step="5">
    <div class="tip">播放音乐时自动压低音量，方便用唤醒词打断</div>
  </div>
</div>

<!-- ============ 灯光 ============ -->
<div class="pane" id="pane-led">
  <div class="card">
    <div class="row"><label>状态灯</label>
      <select id="led_mode"><option value="on">跟随状态</option><option value="off">常灭</option><option value="color">固定颜色</option></select>
    </div>
    <div class="row"><label>固定颜色</label><input type="color" id="led_color" value="#00b3ff"></div>
  </div>
</div>

<!-- ============ 用户开关（GPIO） ============ -->
<div class="pane" id="pane-gpio">
  <div class="card">
    <div class="row"><label><b>我的开关</b></label></div>
    <div id="gpio_list" class="tip">加载中…</div>
    <div class="tip">可以用语音控制，例如说「打开打印机灯光」</div>
  </div>

  <div class="card">
    <div class="row"><label><b>新增 / 编辑</b></label></div>
    <div class="row"><label>名称</label><input type="text" id="g_name" placeholder="打印机灯光"></div>
    <div class="row"><label>GPIO 号</label>
      <select id="g_pin"><option value="">（加载中…）</option></select>
    </div>
    <div class="row"><label>触发电平</label>
      <select id="g_active"><option value="1">高电平点亮</option><option value="0">低电平点亮（继电器）</option></select>
    </div>
    <div class="row"><label>上电初始状态</label>
      <select id="g_init"><option value="0">关闭</option><option value="1">打开</option></select>
    </div>
    <div class="card" style="background:#0f2033;margin:12px 0 0">
      <div class="tip">已被占用的引脚</div>
      <div class="tip" id="g_reserved" style="line-height:1.9"></div>
    </div>
    <button id="g_add">添加这一路</button>
    <div class="tip" id="g_msg"></div>
  </div>
</div>

<!-- ============ 系统 ============ -->
<div class="pane" id="pane-sys">
  <div class="card">
    <div class="row"><label>音乐服务地址</label></div>
    <input type="text" id="api" placeholder="http://47.116.199.150:2233">
    <div class="tip">改完保存立即生效（无需重启）</div>
  </div>
  <div class="card">
    <button class="ghost" onclick="chatTest()">预览聊天气泡样式</button>
    <button class="ghost" onclick="reboot()" style="margin-top:8px">重启设备</button>
    <div class="tip">重启约 8 秒；开机图片改动需要重启才能看到</div>
  </div>
  <div class="card" style="opacity:.9">
    <div class="tip">当前状态</div><pre id="status">-</pre>
  </div>
</div>

<button id="save">保存并应用</button>
<div class="tip" id="msg"></div>

<div class="modal" id="del_modal">
  <div class="modal-box">
    <div class="modal-title">删除开关</div>
    <div class="tip" id="del_text">确定要删除这个开关吗？</div>
    <div class="btnrow" style="margin-top:14px">
      <button class="ghost" onclick="delCancel()">取消</button>
      <button id="del_ok" style="background:#ff6b6b">确认删除</button>
    </div>
  </div>
</div>

<div class="modal" id="crop_modal">
  <div class="modal-box">
    <div class="modal-title">裁切图片 · <span id="crop_title">-</span></div>
    <div class="crop-wrap"><canvas id="crop_canvas" width="240" height="320"></canvas></div>
    <div class="row"><label>缩放 <span class="val" id="crop_zv">100</span>%</label></div>
    <input type="range" id="crop_zoom" min="100" max="400" step="5" value="100">
    <div class="tip">拖动图片调整位置；框内就是屏幕最终显示的样子（240×320）</div>
    <div class="btnrow" style="margin-top:10px">
      <button class="ghost" onclick="cropCancel()">取消</button>
      <button class="ghost" onclick="cropReset()">复位</button>
    </div>
    <button id="crop_ok">确认并上传</button>
    <div class="tip" id="crop_msg"></div>
  </div>
</div>
</div>
<script>
const $ = id => document.getElementById(id);
let cur = {};
function fill(s){
  cur = s;
  $('dev').textContent = (s.board||'设备') + ' · ' + location.host + ' · ' + (s.mac||'');
  $('theme').value = s.theme || 'dark';
  $('brightness').value = s.brightness ?? 80; $('bv').textContent = $('brightness').value;
  $('volume').value = s.volume ?? 70; $('vv').textContent = $('volume').value;
  $('duck').value = s.duck ?? 60; $('dv').textContent = $('duck').value;
  $('led_mode').value = s.led_mode || 'on';

  if (s.led_color) $('led_color').value = s.led_color;
  if (s.api) $('api').value = s.api;
  $('standby').value = String(s.standby ?? 1);
  $('standby_city').value = s.standby_city || '上海';
  $('standby_symbols').value = s.standby_symbols || 'usIXIC,usINX,sh000001';
  $('wake_enabled').value = String(s.wake_enabled ?? 1);
  $('wake_threshold').value = s.wake_threshold ?? 50;
  $('wtv').textContent = ((s.wake_threshold ?? 50)/100).toFixed(2);
  $('ww_name').textContent = s.wake_words ? ('· ' + s.wake_words) : '';
  $('wake_warn').style.display = (s.wake_enabled ?? 1) ? 'none' : 'block';
  $('status').textContent = JSON.stringify(s, null, 1);
}
async function load(){
  try{ const r = await fetch('/api/status'); fill(await r.json()); }
  catch(e){ $('msg').textContent='读取失败：'+e; }
}


/* ---------------- 用户 GPIO 开关 ---------------- */
let gpioDevices = [], gpioReserved = [], gpioUsed = [], gpioAvailable = [];

function renderGpio(){
  const box = $('gpio_list');
  box.innerHTML = '';
  if (!gpioDevices.length){
    const empty = document.createElement('div');
    empty.className = 'tip';
    empty.textContent = '还没有配置任何开关（下面可以添加）';
    box.appendChild(empty);
  } else {
    gpioDevices.forEach((d, i) => {
      const card = document.createElement('div');
      card.className = 'dev-card';

      const top = document.createElement('div');
      top.className = 'dev-top';

      const left = document.createElement('div');
      const name = document.createElement('div');
      name.className = 'dev-name';
      name.textContent = d.name;
      const sub = document.createElement('div');
      sub.className = 'dev-sub';
      sub.textContent = 'GPIO' + d.gpio + (d.active_high ? ' · 高电平触发' : ' · 低电平触发')
        + (d.initial_on ? ' · 上电默认开' : '');
      left.appendChild(name); left.appendChild(sub);

      const sw = document.createElement('button');
      sw.className = 'sw' + (d.on ? ' on' : '');
      sw.title = d.on ? '点击关闭' : '点击打开';
      sw.onclick = () => gpioControl(d.name, d.on ? 'off' : 'on');

      top.appendChild(left); top.appendChild(sw);

      const actions = document.createElement('div');
      actions.className = 'dev-actions';
      const st = document.createElement('span');
      st.className = 'dev-state ' + (d.on ? 'on' : 'off');
      st.textContent = d.on ? '● 已打开' : '○ 已关闭';
      const del = document.createElement('button');
      del.className = 'btn-del';
      del.textContent = '删除';
      del.onclick = () => delAsk(d.name);
      actions.appendChild(st); actions.appendChild(del);

      card.appendChild(top); card.appendChild(actions);
      box.appendChild(card);
    });
  }

  /* 下拉框：只放当前可用的引脚 */
  const sel = $('g_pin');
  const prev = sel.value;
  sel.innerHTML = '';
  if (!gpioAvailable.length) {
    const o = document.createElement('option'); o.value=''; o.textContent='没有可用引脚了'; sel.appendChild(o);
  } else {
    gpioAvailable.forEach(pin => {
      const o = document.createElement('option');
      o.value = pin; o.textContent = 'GPIO' + pin;
      sel.appendChild(o);
    });
    if (prev && gpioAvailable.includes(+prev)) sel.value = prev;
  }

  /* 说明栏：所有已占用引脚 + 原因 */
  const notes = gpioUsed.map(u => 'GPIO' + u.gpio + ' · ' + u.reason);
  $('g_reserved').innerHTML = notes.length ? notes.join('<br>') : '（暂无）';
}

/* 删除二次确认 */
let delTarget = null;
function delAsk(name){
  delTarget = name;
  $('del_text').textContent = '确定要删除「' + name + '」吗？删除后引脚会恢复，语音也不能再控制它。';
  $('del_modal').classList.add('on');
}
function delCancel(){
  delTarget = null;
  $('del_modal').classList.remove('on');
}
$('del_ok').onclick = () => {
  const name = delTarget;
  delCancel();
  if (!name) return;
  const devices = gpioDevices
    .filter(d => d.name !== name)
    .map(d => ({name:d.name, gpio:d.gpio, active_high:d.active_high, initial_on:d.initial_on}));
  saveGpio(devices, '已删除「' + name + '」✓');
};

async function loadGpio(){
  try{
    const r = await fetch('/api/gpio');
    const j = await r.json();
    gpioDevices = j.devices || [];
    gpioReserved = j.reserved || [];
    gpioUsed = j.used || [];
    gpioAvailable = j.available || [];
    renderGpio();
  }catch(e){ $('g_msg').textContent = '读取失败：'+e; }
}

async function gpioControl(name, state){
  try{
    const r = await fetch('/api/gpio',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({name: name, state: state})});
    const j = await r.json();
    $('g_msg').className = 'tip ' + (j.ok?'ok':'warn');
    $('g_msg').textContent = j.message || (j.ok?'已执行':'失败');
    loadGpio();
  }catch(e){ $('g_msg').textContent = '失败：'+e; }
}

async function saveGpio(devices, msg){
  try{
    const r = await fetch('/api/gpio',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({devices: devices})});
    const j = await r.json();
    $('g_msg').className = 'tip ' + (j.ok?'ok':'warn');
    $('g_msg').textContent = j.ok ? (msg||'已保存 ✓') : ('失败：'+(j.error||''));
    if (j.ok) loadGpio();
  }catch(e){ $('g_msg').textContent = '失败：'+e; }
}

$('g_add').onclick = () => {
  const name = $('g_name').value.trim();
  const pin = parseInt($('g_pin').value, 10);
  if (!name){ $('g_msg').textContent='请填写名称'; $('g_msg').className='tip warn'; return; }
  if (!(pin >= 0 && pin <= 48)){ $('g_msg').textContent='没有可用引脚'; $('g_msg').className='tip warn'; return; }
  const devices = gpioDevices.map(d => ({name:d.name, gpio:d.gpio, active_high:d.active_high, initial_on:d.initial_on}));
  devices.push({name: name, gpio: pin, active_high: +$('g_active').value === 1, initial_on: +$('g_init').value === 1});
  saveGpio(devices, '已添加 ✓（现在可以直接说「打开'+name+'」）');
  $('g_name').value='';
};

/* ---------------- 分类卡片：tab 切换 ---------------- */
document.querySelectorAll('.tabs button').forEach(btn=>{
  btn.onclick = () => {
    document.querySelectorAll('.tabs button').forEach(b=>b.classList.remove('on'));
    document.querySelectorAll('.pane').forEach(p=>p.classList.remove('on'));
    btn.classList.add('on');
    $('pane-'+btn.dataset.tab).classList.add('on');
    if (btn.dataset.tab === 'gpio') loadGpio();
    window.scrollTo({top:0,behavior:'smooth'});
  };
});

/* ---------------- 开机图 / 背景图 ---------------- */
const IMG_W = 240, IMG_H = 320;
function previewImage(kind){
  const box = $(kind+'_box');
  const url = '/api/image?kind='+kind+'&t='+Date.now();
  const im = new Image();
  im.onload = () => { box.innerHTML=''; box.appendChild(im); };
  im.onerror = () => { box.textContent = kind==='bg' ? '未设置（使用深色渐变）' : '未设置（开机直接进对话界面）'; };
  im.src = url;
}
/* ---------------- 裁切弹窗：WYSIWYG 240x320 ---------------- */
const crop = {kind:null, bmp:null, zoom:1, ox:0, oy:0, dragging:false, px:0, py:0};
const cv = document.getElementById('crop_canvas');
const ctx = cv.getContext('2d');

function cropTitle(kind){ return kind === 'bg' ? '待机背景图' : '开机图'; }

function drawCrop(){
  if (!crop.bmp) return;
  const iw = crop.bmp.width, ih = crop.bmp.height;
  const base = Math.max(IMG_W/iw, IMG_H/ih);       // cover：保证铺满
  const k = base * crop.zoom;
  const dw = iw*k, dh = ih*k;
  // 允许移动范围：图比画布大的部分
  const maxX = Math.max(0, (dw-IMG_W)/2), maxY = Math.max(0, (dh-IMG_H)/2);
  crop.ox = Math.min(maxX, Math.max(-maxX, crop.ox));
  crop.oy = Math.min(maxY, Math.max(-maxY, crop.oy));
  ctx.fillStyle = '#0b1a2a';
  ctx.fillRect(0,0,IMG_W,IMG_H);
  ctx.drawImage(crop.bmp, (IMG_W-dw)/2 + crop.ox, (IMG_H-dh)/2 + crop.oy, dw, dh);
}

function openCrop(kind, file){
  const msg = $(kind+'_msg');
  msg.textContent = '读取图片…';
  createImageBitmap(file).then(bmp => {
    crop.kind = kind; crop.bmp = bmp; crop.zoom = 1; crop.ox = 0; crop.oy = 0;
    $('crop_title').textContent = cropTitle(kind);
    $('crop_zoom').value = 100; $('crop_zv').textContent = '100';
    $('crop_msg').textContent = '原图 '+bmp.width+'×'+bmp.height+' → 输出 240×320';
    $('crop_modal').classList.add('on');
    msg.textContent = '';
    drawCrop();
  }).catch(e => { msg.textContent = '读取失败：'+e; msg.className='tip warn'; });
}
function cropCancel(){ $('crop_modal').classList.remove('on'); crop.bmp = null; }
function cropReset(){ crop.zoom = 1; crop.ox = 0; crop.oy = 0; $('crop_zoom').value = 100; $('crop_zv').textContent='100'; drawCrop(); }

$('crop_zoom').addEventListener('input', e => {
  crop.zoom = (+e.target.value)/100;
  $('crop_zv').textContent = e.target.value;
  drawCrop();
});

/* 拖动 / 触摸 */
cv.addEventListener('pointerdown', e => {
  if (!crop.bmp) return;
  crop.dragging = true; crop.px = e.clientX; crop.py = e.clientY;
  cv.classList.add('dragging'); cv.setPointerCapture(e.pointerId);
});
cv.addEventListener('pointermove', e => {
  if (!crop.dragging || !crop.bmp) return;
  const rect = cv.getBoundingClientRect();
  const sx = IMG_W / rect.width;      // 显示尺寸 -> 画布像素
  crop.ox += (e.clientX - crop.px) * sx;
  crop.oy += (e.clientY - crop.py) * sx;
  crop.px = e.clientX; crop.py = e.clientY;
  drawCrop();
});
['pointerup','pointercancel'].forEach(ev => cv.addEventListener(ev, () => {
  crop.dragging = false; cv.classList.remove('dragging');
}));

$('crop_ok').onclick = async () => {
  if (!crop.bmp) return;
  const kind = crop.kind;
  const msg = $(kind+'_msg');
  $('crop_msg').textContent = '生成并上传…';
  try{
    const blob = await new Promise(r => cv.toBlob(r, 'image/jpeg', 0.85));
    $('crop_msg').textContent = '上传中…（'+(blob.size/1024).toFixed(0)+'KB，240×320）';
    const r = await fetch('/api/image?kind='+kind, {method:'POST', headers:{'Content-Type':'image/jpeg'}, body:blob});
    const j = await r.json();
    if (j.ok) {
      cropCancel();
      msg.className = 'tip ok';
      msg.textContent = '已保存 ✓（240×320，'+(blob.size/1024).toFixed(0)+'KB）';
      previewImage(kind);
    } else {
      $('crop_msg').textContent = '失败：'+(j.error||'');
    }
  }catch(e){ $('crop_msg').textContent = '失败：'+e; }
};

async function clearImage(kind){
  if(!confirm('确定清除这张图片吗？')) return;
  const msg = $(kind+'_msg');
  try{
    const r = await fetch('/api/image?kind='+kind+'&clear=1',{method:'POST'});
    const j = await r.json();
    msg.textContent = j.ok ? '已清除 ✓' : ('失败：'+(j.error||''));
    msg.className = 'tip ' + (j.ok?'ok':'warn');
    previewImage(kind);
  }catch(e){ msg.textContent='失败：'+e; }
}
['boot','bg'].forEach(kind=>{
  $(kind+'_file').addEventListener('change', e=>{
    if (e.target.files && e.target.files[0]) openCrop(kind, e.target.files[0]);
    e.target.value = '';
  });
  previewImage(kind);
});

$('wake_threshold').addEventListener('input', e=>{
  $('wtv').textContent = ((+e.target.value)/100).toFixed(2);
});
$('wake_enabled').addEventListener('change', e=>{
  $('wake_warn').style.display = (+e.target.value) ? 'none' : 'block';
});
['brightness','volume','duck'].forEach(id=>{
  $(id).addEventListener('input',()=>{ $(id[0]+'v').textContent = $(id).value; });
});
$('save').onclick = async () => {
  $('msg').textContent = '保存中…';
  const body = {
    theme: $('theme').value,
    brightness: +$('brightness').value,
    volume: +$('volume').value,
    duck: +$('duck').value,
    led_mode: $('led_mode').value,
    led_color: $('led_color').value,
    api: $('api').value.trim(),
    standby: +$('standby').value,
    standby_city: $('standby_city').value.trim(),
    standby_symbols: $('standby_symbols').value.trim(),
    wake_enabled: +$('wake_enabled').value,
    wake_threshold: +$('wake_threshold').value
  };
  try{
    const r = await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const j = await r.json();
    $('msg').className = 'tip ' + (j.ok?'ok':'warn');
    $('msg').textContent = j.ok ? '已应用 ✓' : ('失败：'+(j.error||''));
    load();
  }catch(e){ $('msg').textContent='请求失败：'+e; }
};
async function chatTest(){
  try{
    await fetch('/api/chat_test',{method:'POST'});
    $('msg').textContent = '已发送样例消息，看一眼屏幕（说完话后气泡会被清空）';
  }catch(e){ $('msg').textContent='失败：'+e; }
}
async function reboot(){
  if(!confirm('确定重启设备吗？')) return;
  $('msg').textContent = '重启中…约 8 秒后刷新页面';
  try{ await fetch('/api/reboot',{method:'POST'}); }catch(e){}
  setTimeout(load, 9000);
}
$('applyAssets').onclick = async () => {
  const v = $('variant').value;
  if(!confirm('将把「'+v+'」资源包写入设备并重启，确定吗？')) return;
  $('amsg').textContent = '下载中，请稍候…（设备会自动重启）';
  try{
    const r = await fetch('/api/assets',{method:'POST',headers:{'Content-Type':'application/json'},
      body: JSON.stringify({url:'http://47.116.199.150:2233/assets/'+v+'.bin'})});
    const j = await r.json();
    $('amsg').textContent = j.ok ? '已开始写入，设备即将重启…' : ('失败：'+(j.error||''));
  }catch(e){ $('amsg').textContent = '请求失败：'+e; }
};
load();
loadGpio();
</script></body></html>)HTML";

const char* LedModeToString(int mode) {
    switch (mode) {
        case 1: return "off";
        case 2: return "color";
        default: return "on";
    }
}

int HexToRgb(const std::string& hex, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (hex.size() != 7 || hex[0] != '#') {
        return 0;
    }
    unsigned int v = 0;
    if (sscanf(hex.c_str() + 1, "%06x", &v) != 1) {
        return 0;
    }
    *r = (v >> 16) & 0xFF;
    *g = (v >> 8) & 0xFF;
    *b = v & 0xFF;
    return 1;
}

std::string BuildStatusJson() {
    auto& board = Board::GetInstance();
    Settings ui("ui", false);
    Settings music("music", false);
    Settings led_settings("led", false);

    auto* display = board.GetDisplay();
    std::string theme = "dark";
    if (display != nullptr && display->GetTheme() != nullptr) {
        theme = display->GetTheme()->name();
    }
    int brightness = 80;
    if (auto* bl = board.GetBacklight()) {
        brightness = bl->brightness();
    }
    int volume = 70;
    if (auto* codec = board.GetAudioCodec()) {
        volume = codec->output_volume();
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "board", BOARD_NAME);
    cJSON_AddStringToObject(root, "version", SystemInfo::GetUserAgent().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "theme", theme.c_str());
    cJSON_AddNumberToObject(root, "brightness", brightness);
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddNumberToObject(root, "duck", music.GetInt("duck_volume_percent", CONFIG_MUSIC_DUCK_VOLUME_PERCENT));
    cJSON_AddStringToObject(root, "api", music.GetString("api_url", CONFIG_MUSIC_API_URL).c_str());
    cJSON_AddStringToObject(root, "led_mode",
                             led_settings.GetString("mode",
                                                     led_settings.GetInt("status_enabled", 1)
                                                         ? "on" : "off").c_str());
    cJSON_AddStringToObject(root, "led_color", "#00b3ff");
    auto& app = Application::GetInstance();
    cJSON_AddNumberToObject(root, "wake_enabled", app.WakeWordEnabled() ? 1 : 0);
    cJSON_AddNumberToObject(root, "wake_threshold", (int)(app.WakeWordThreshold() * 100 + 0.5f));
    std::string wake_words;
    for (auto& w : app.GetWakeWords()) {
        if (!wake_words.empty()) {
            wake_words += " / ";
        }
        wake_words += w;
    }
    cJSON_AddStringToObject(root, "wake_words", wake_words.c_str());

    auto& standby = StandbyService::GetInstance();
    cJSON_AddNumberToObject(root, "standby", standby.enabled() ? 1 : 0);
    cJSON_AddStringToObject(root, "standby_city", standby.city().c_str());
    cJSON_AddStringToObject(root, "standby_symbols", standby.symbols().c_str());
    char* out = cJSON_PrintUnformatted(root);
    std::string json = out ? out : "{}";
    cJSON_free(out);
    cJSON_Delete(root);
    return json;
}


/* ---------------- 外观资源包（字体/表情）安全热更新 ----------------
 * 不在运行中直接改写 assets（会与 LVGL 读取冲突导致崩溃/变砖），
 * 改为：下载到 ota_1 暂存 -> 重启 -> 启动早期由 AssetOta::ApplyIfPending() 写入。
 */

esp_err_t HandleAssetsApply(httpd_req_t* req) {
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty\"}");
        return ESP_FAIL;
    }
    buf[len] = 0;
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }
    auto* url_item = cJSON_GetObjectItem(root, "url");
    std::string url = cJSON_IsString(url_item) ? url_item->valuestring : "";
    cJSON_Delete(root);
    if (url.size() < 10) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad url\"}");
        return ESP_FAIL;
    }
    if (!AssetOta::StageFromUrl(url)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"stage_failed\"}");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    /* 让响应先发出去，再重启 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}


/* ---------------- 用户图片（开机图 / 待机背景） ----------------
 * GET  /api/image?kind=boot|bg        回读原始 JPEG（网页预览用）
 * POST /api/image?kind=boot|bg        直接上传 JPEG 字节（网页用 canvas 缩放到 240x320）
 * POST /api/image?kind=boot|bg&clear=1 清除
 */
/* 调试用：往聊天区塞几条示例消息，方便在不动嘴的情况下检查气泡样式 */
esp_err_t HandleChatTest(httpd_req_t* req) {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no display\"}");
        return ESP_FAIL;
    }
    display->SetChatMessage("system", "样式预览 · 微信风格气泡");
    display->SetChatMessage("user", "今天天气怎么样？帮我看看纳指涨了没有");
    display->SetChatMessage("assistant", "上海今天多云，12 度，湿度 54%。纳指 24089，涨 6.51%，标普 7237，涨 4.73%。");
    display->SetChatMessage("user", "好的谢谢");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t HandleReboot(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t HandleImageGet(httpd_req_t* req) {
    char query[64] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    char kind_str[16] = {0};
    if (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no kind\"}");
        return ESP_FAIL;
    }
    auto kind = strcmp(kind_str, "bg") == 0 ? UserImage::kBackground : UserImage::kBoot;
    auto& img = UserImage::GetInstance();
    const uint32_t len = img.length(kind);
    if (len == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(malloc(len));
    }
    if (buf == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const size_t got = img.Read(kind, buf, len);
    if (got == 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buf), got);
    free(buf);
    return err;
}

esp_err_t HandleImagePost(httpd_req_t* req) {
    char query[128] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }
    char kind_str[16] = {0};
    char clear_str[8] = {0};
    if (httpd_query_key_value(query, "kind", kind_str, sizeof(kind_str)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no kind\"}");
        return ESP_FAIL;
    }
    auto kind = strcmp(kind_str, "bg") == 0 ? UserImage::kBackground : UserImage::kBoot;
    auto& img = UserImage::GetInstance();

    if (httpd_query_key_value(query, "clear", clear_str, sizeof(clear_str)) == ESP_OK &&
        clear_str[0] == '1') {
        img.Clear(kind);
        StandbyService::GetInstance().ReloadBackground();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true,\"cleared\":true}");
        return ESP_OK;
    }

    const int total = req->content_len;
    if (total < 64 || total > 400 * 1024) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad size\"}");
        return ESP_FAIL;
    }
    if (!img.BeginWrite(kind)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"erase failed\"}");
        return ESP_FAIL;
    }
    auto* chunk = static_cast<uint8_t*>(heap_caps_malloc(8192, MALLOC_CAP_SPIRAM));
    if (chunk == nullptr) {
        chunk = static_cast<uint8_t*>(malloc(8192));
    }
    if (chunk == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = 0;
    bool ok = true;
    while (received < total) {
        int want = total - received;
        if (want > 8192) {
            want = 8192;
        }
        int n = httpd_req_recv(req, reinterpret_cast<char*>(chunk), want);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (!img.WriteChunk(chunk, n)) {
            ok = false;
            break;
        }
        received += n;
    }
    free(chunk);

    if (!ok || !img.EndWrite(0, 0)) {
        img.Clear(kind);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"image_too_large_or_invalid\"}");
        return ESP_FAIL;
    }
    if (kind == UserImage::kBackground) {
        StandbyService::GetInstance().ReloadBackground();
    }
    ESP_LOGI(TAG, "image uploaded: kind=%s %d bytes", kind_str, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}


/* ---------------- 用户自定义 GPIO 开关 ----------------
 * GET  /api/gpio  -> {"devices":[{name,gpio,active_high,initial_on,on}], "reserved":[...]}
 * POST /api/gpio  {"devices":[{name,gpio,active_high,initial_on}]}  保存配置
 * POST /api/gpio  {"name":"打印机灯光","state":"on|off|toggle"}      即时控制
 */
/* 诊断：当前注册给大模型的 MCP 工具列表 */
esp_err_t HandleMcpTools(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "tools");
    for (const auto& name : McpServer::GetInstance().GetToolNames()) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(name.c_str()));
    }
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
    char* out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out != nullptr ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t HandleGpio(httpd_req_t* req) {
    auto& gpio = GpioControl::GetInstance();
    httpd_resp_set_type(req, "application/json");

    if (req->method == HTTP_GET) {
        cJSON* root = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(root, "devices");
        for (const auto& d : gpio.Devices()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", d.name.c_str());
            cJSON_AddNumberToObject(item, "gpio", d.gpio);
            cJSON_AddBoolToObject(item, "active_high", d.active_high);
            cJSON_AddBoolToObject(item, "initial_on", d.initial_on);
            bool on = false;
            gpio.GetState(d.name, &on);
            cJSON_AddBoolToObject(item, "on", on);
            cJSON_AddItemToArray(arr, item);
        }
        auto* reserved = cJSON_AddArrayToObject(root, "reserved");
        for (const auto& entry : Board::GetInstance().ReservedGpios()) {
            cJSON_AddItemToArray(reserved, cJSON_CreateNumber(entry.first));
        }
        /* 说明栏：所有已占用引脚 + 原因 */
        auto* used = cJSON_AddArrayToObject(root, "used");
        for (const auto& u : gpio.UsedPins()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "gpio", u.gpio);
            cJSON_AddStringToObject(item, "reason", u.reason.c_str());
            cJSON_AddItemToArray(used, item);
        }
        /* 下拉框：当前还能选的引脚 */
        auto* avail = cJSON_AddArrayToObject(root, "available");
        for (int pin : gpio.AvailablePins()) {
            cJSON_AddItemToArray(avail, cJSON_CreateNumber(pin));
        }
        char* out = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, out != nullptr ? out : "{}");
        cJSON_free(out);
        cJSON_Delete(root);
        return ESP_OK;
    }

    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty\"}");
        return ESP_FAIL;
    }
    buf[len] = 0;
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }

    /* 即时控制：{"name":..,"state":..} */
    auto* ctl_name = cJSON_GetObjectItem(root, "name");
    auto* ctl_state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(ctl_name) && cJSON_IsString(ctl_state)) {
        std::string msg;
        bool ok = gpio.SetByState(ctl_name->valuestring, ctl_state->valuestring, &msg);
        cJSON_Delete(root);
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", ok);
        cJSON_AddStringToObject(resp, "message", msg.c_str());
        char* out = cJSON_PrintUnformatted(resp);
        httpd_resp_sendstr(req, out != nullptr ? out : "{\"ok\":false}");
        cJSON_free(out);
        cJSON_Delete(resp);
        return ESP_OK;
    }

    /* 保存配置 */
    auto* devices = cJSON_GetObjectItem(root, "devices");
    std::vector<GpioSwitchConfig> cfgs;
    if (cJSON_IsArray(devices)) {
        const int n = cJSON_GetArraySize(devices);
        for (int i = 0; i < n; i++) {
            const cJSON* it = cJSON_GetArrayItem(devices, i);
            const cJSON* name = cJSON_GetObjectItem(it, "name");
            const cJSON* pin = cJSON_GetObjectItem(it, "gpio");
            if (!cJSON_IsString(name) || !cJSON_IsNumber(pin)) {
                continue;
            }
            if (name->valuestring[0] == 0) {
                continue;
            }
            GpioSwitchConfig cfg;
            cfg.name = name->valuestring;
            cfg.gpio = pin->valueint;
            const cJSON* ah = cJSON_GetObjectItem(it, "active_high");
            const cJSON* io = cJSON_GetObjectItem(it, "initial_on");
            cfg.active_high = cJSON_IsBool(ah) ? cJSON_IsTrue(ah) : true;
            cfg.initial_on = cJSON_IsBool(io) ? cJSON_IsTrue(io) : false;
            cfgs.push_back(cfg);
        }
    }
    cJSON_Delete(root);

    std::string error;
    bool ok = gpio.Save(cfgs, &error);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(resp, "error", error.c_str());
    }
    char* out = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, out != nullptr ? out : "{\"ok\":false}");
    cJSON_free(out);
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandleStatus(httpd_req_t* req) {
    auto json = BuildStatusJson();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), json.size());
}

void ApplyTheme(const std::string& name) {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
    if (theme != nullptr) {
        display->SetTheme(theme);
        Settings("ui", true).SetString("theme", name);
        ESP_LOGI(TAG, "theme -> %s", name.c_str());
    }
}

esp_err_t HandleSettings(httpd_req_t* req) {
    char buf[1024];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty body\"}");
        return ESP_FAIL;
    }
    buf[len] = '\0';
    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad json\"}");
        return ESP_FAIL;
    }

    auto& board = Board::GetInstance();

    auto* theme = cJSON_GetObjectItem(root, "theme");
    if (cJSON_IsString(theme)) {
        ApplyTheme(theme->valuestring);
    }

    auto* brightness = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness)) {
        if (auto* bl = board.GetBacklight()) {
            bl->SetBrightness(static_cast<uint8_t>(brightness->valueint), true);
            ESP_LOGI(TAG, "brightness -> %d", brightness->valueint);
        }
    }

    auto* volume = cJSON_GetObjectItem(root, "volume");
    if (cJSON_IsNumber(volume)) {
        if (auto* codec = board.GetAudioCodec()) {
            codec->SetOutputVolume(volume->valueint);
            Settings("audio", true).SetInt("output_volume", volume->valueint);
            ESP_LOGI(TAG, "volume -> %d", volume->valueint);
        }
    }

    auto* duck = cJSON_GetObjectItem(root, "duck");
    if (cJSON_IsNumber(duck)) {
        Settings("music", true).SetInt("duck_volume_percent", duck->valueint);
        ESP_LOGI(TAG, "duck -> %d%%", duck->valueint);
    }

    auto* api = cJSON_GetObjectItem(root, "api");
    if (cJSON_IsString(api) && strlen(api->valuestring) > 7) {
        Settings("music", true).SetString("api_url", api->valuestring);
        MusicApiClient::GetInstance().Reload();
        ESP_LOGI(TAG, "music api -> %s", api->valuestring);
    }

    auto* led_mode = cJSON_GetObjectItem(root, "led_mode");
    auto* wake_enabled = cJSON_GetObjectItem(root, "wake_enabled");
    auto* wake_threshold = cJSON_GetObjectItem(root, "wake_threshold");
    if (cJSON_IsNumber(wake_enabled) || cJSON_IsNumber(wake_threshold)) {
        auto& app = Application::GetInstance();
        if (cJSON_IsNumber(wake_threshold)) {
            app.SetWakeWordThreshold(wake_threshold->valueint / 100.0f);
        }
        if (cJSON_IsNumber(wake_enabled)) {
            app.SetWakeWordEnabled(wake_enabled->valueint != 0);
        }
        ESP_LOGI(TAG, "wake -> %d threshold=%d", (int)app.WakeWordEnabled(),
                 (int)(app.WakeWordThreshold() * 100 + 0.5f));
    }

    auto* standby = cJSON_GetObjectItem(root, "standby");
    auto* standby_city = cJSON_GetObjectItem(root, "standby_city");
    auto* standby_symbols = cJSON_GetObjectItem(root, "standby_symbols");
    if (cJSON_IsNumber(standby) || cJSON_IsString(standby_city) || cJSON_IsString(standby_symbols)) {
        auto& svc = StandbyService::GetInstance();
        if (cJSON_IsNumber(standby)) {
            svc.SetEnabled(standby->valueint != 0);
        }
        if (cJSON_IsString(standby_city) && standby_city->valuestring[0]) {
            svc.SetCity(standby_city->valuestring);
        }
        if (cJSON_IsString(standby_symbols) && standby_symbols->valuestring[0]) {
            svc.SetSymbols(standby_symbols->valuestring);
        }
        svc.ReloadConfig();
        ESP_LOGI(TAG, "standby -> %d city=%s symbols=%s", (int)svc.enabled(), svc.city().c_str(),
                 svc.symbols().c_str());
    }

    auto* led_color = cJSON_GetObjectItem(root, "led_color");
    if (auto* led = board.GetLed()) {
        if (cJSON_IsString(led_mode)) {
            std::string mode = led_mode->valuestring;
            if (mode != "on" && mode != "off" && mode != "color") {
                mode = "on";
            }
            Settings("led", true).SetString("mode", mode);
            if (mode == "off") {
                led->SetStatusEnabled(false);
                led->TurnOff();
            } else if (mode == "color") {
                uint8_t r = 0, g = 0, b = 255;
                if (cJSON_IsString(led_color)) {
                    HexToRgb(led_color->valuestring, &r, &g, &b);
                }
                led->SetStatusEnabled(false);
                led->SetColor(r, g, b);
                led->TurnOn();
            } else {
                led->SetStatusEnabled(true);
            }
            ESP_LOGI(TAG, "led mode -> %s", mode.c_str());
        }
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

}  // namespace

bool AdminServer::Start() {
    if (g_server != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 20;   /* 现在有 11 个 URI（GET/POST 分开算），默认 8 不够 */
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start admin server");
        g_server = nullptr;
        return false;
    }
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = nullptr};
    httpd_uri_t uri_settings = {.uri = "/api/settings", .method = HTTP_POST, .handler = HandleSettings, .user_ctx = nullptr};
    httpd_uri_t uri_assets = {.uri = "/api/assets", .method = HTTP_POST, .handler = HandleAssetsApply, .user_ctx = nullptr};
    httpd_uri_t uri_reboot = {.uri = "/api/reboot", .method = HTTP_POST, .handler = HandleReboot, .user_ctx = nullptr};
    httpd_uri_t uri_chat_test = {.uri = "/api/chat_test", .method = HTTP_POST, .handler = HandleChatTest, .user_ctx = nullptr};
    httpd_uri_t uri_mcp_tools = {.uri = "/api/mcp_tools", .method = HTTP_GET, .handler = HandleMcpTools, .user_ctx = nullptr};
    httpd_uri_t uri_gpio_get = {.uri = "/api/gpio", .method = HTTP_GET, .handler = HandleGpio, .user_ctx = nullptr};
    httpd_uri_t uri_gpio_post = {.uri = "/api/gpio", .method = HTTP_POST, .handler = HandleGpio, .user_ctx = nullptr};
    httpd_uri_t uri_image_get = {.uri = "/api/image", .method = HTTP_GET, .handler = HandleImageGet, .user_ctx = nullptr};
    httpd_uri_t uri_image_post = {.uri = "/api/image", .method = HTTP_POST, .handler = HandleImagePost, .user_ctx = nullptr};
    auto reg = [](httpd_uri_t* uri) {
        esp_err_t err = httpd_register_uri_handler(g_server, uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uri->uri, esp_err_to_name(err));
        }
    };
    reg(&uri_root);
    reg(&uri_status);
    reg(&uri_settings);
    reg(&uri_assets);
    reg(&uri_reboot);
    reg(&uri_chat_test);
    reg(&uri_mcp_tools);
    reg(&uri_gpio_get);
    reg(&uri_gpio_post);
    reg(&uri_image_get);
    reg(&uri_image_post);
    ESP_LOGI(TAG, "admin web: http://<device-ip>:8080/");
    return true;
}

void AdminServer::ApplyPersisted() {
    Settings ui("ui", false);
    auto theme = ui.GetString("theme", "");
    if (!theme.empty()) {
        ApplyTheme(theme);
    }
}

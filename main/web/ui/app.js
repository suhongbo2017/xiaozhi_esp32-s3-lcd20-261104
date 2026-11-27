/* 小智设备管理 — 前端逻辑（自动保存 + 屏幕模拟 + 图片裁切） */
'use strict';

const $ = (id) => document.getElementById(id);
const IMG_W = 240, IMG_H = 320;

let S = {};              /* 设备状态 */
let deviceOnline = false;

/* ---------------- 预设聊天样式（与固件 kStyles 一一对应） ---------------- */
const STYLES = [
  { id: 0, name: '微信浅色', userBg: '#95EC69', userFg: '#111111', aiBg: '#FFFFFF', aiFg: '#111111',
    bg: '#EDEDED', radius: 10, avatar: true, tail: true },
  { id: 1, name: '微信深色', userBg: '#3EB575', userFg: '#F2FFF6', aiBg: '#2C2C2E', aiFg: '#F2FFF6',
    bg: '#111111', radius: 10, avatar: true, tail: true },
  { id: 2, name: 'iMessage', userBg: '#0A84FF', userFg: '#FFFFFF', aiBg: '#E9E9EB', aiFg: '#111111',
    bg: '#000000', radius: 18, avatar: false, tail: false },
  { id: 3, name: '极简', userBg: 'transparent', userFg: '#95EC69', aiBg: 'transparent',
    aiFg: '#D8DEE9', bg: '#EDEDED', radius: 0, avatar: false, tail: false },
];

/* ---------------- toast ---------------- */
let toastTimer = null;
function toast(msg, isErr) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast on' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = 'toast'; }, 1800);
}

/* ---------------- 自动保存（防抖合并） ---------------- */
let pending = {}, saveTimer = null, saving = false;
function queueSave(patch, opts) {
  Object.assign(pending, patch);
  clearTimeout(saveTimer);
  const delay = (opts && opts.delay != null) ? opts.delay : 350;
  saveTimer = setTimeout(flushSave, delay);
}
async function flushSave() {
  if (!Object.keys(pending).length || saving) {
    if (saving) { clearTimeout(saveTimer); saveTimer = setTimeout(flushSave, 200); }
    return;
  }
  const body = pending;
  pending = {};
  saving = true;
  try {
    const r = await fetch('/api/settings', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    });
    const j = await r.json();
    if (!j.ok) { toast('保存失败：' + (j.error || ''), true); } else { toast('已保存 ✓'); }
    setOnline(true);
  } catch (e) {
    toast('设备无响应', true);
    setOnline(false);
  } finally {
    saving = false;
  }
}

/* ---------------- 在线状态 ---------------- */
function setOnline(on) {
  deviceOnline = on;
  const d = $('online');
  d.className = 'dot ' + (on ? 'on' : 'off');
}

/* ---------------- 加载状态 ---------------- */
async function load() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    const s = await r.json();
    S = s;
    setOnline(true);
    render(s);
  } catch (e) {
    setOnline(false);
    $('devline').textContent = '设备离线（检查是否同一 WiFi）';
  }
}

function render(s) {
  $('devline').textContent = (s.board || '设备') + ' · ' + location.host + ' · ' + (s.mac || '');

  /* 屏幕 */
  $('theme').value = s.theme || 'dark';
  $('brightness').value = s.brightness ?? 80;
  $('bv').textContent = $('brightness').value;

  /* 待机页 */
  $('standby_on').checked = (s.standby ?? 1) ? true : false;
  $('standby_city').value = s.standby_city || '上海';
  $('standby_symbols').value = s.standby_symbols || 'usIXIC,usINX,sh000001';

  /* 声音 */
  $('volume').value = s.volume ?? 70;
  $('vv').textContent = $('volume').value;
  $('duck').value = s.duck ?? 60;
  $('dv').textContent = $('duck').value;
  $('wake_on').checked = (s.wake_enabled ?? 1) ? true : false;
  $('wake_threshold').value = s.wake_threshold ?? 50;
  $('wtv').textContent = ((s.wake_threshold ?? 50) / 100).toFixed(2);
  $('ww_name').textContent = s.wake_words || '';

  /* 系统 */
  if (s.api) $('api').value = s.api;
  renderStatusKV(s);
  renderStyles(s.chat_style ?? 0);
  renderMock(s);
}

function renderStatusKV(s) {
  const rows = [
    ['板型', s.board], ['固件', s.version], ['MAC', s.mac],
    ['主题', s.theme], ['亮度', s.brightness], ['音量', s.volume],
    ['唤醒词', s.wake_words || '-'], ['聊天样式', STYLES[s.chat_style ?? 0]?.name || '-'],
    ['指数', s.standby_symbols], ['服务', s.api], ['上次重启', s.last_reset],
  ];
  $('status_kv').innerHTML = rows
    .map(([k, v]) => `<div class="k">${k}</div><div class="v">${v ?? '-'}</div>`).join('');
}

/* ---------------- 聊天样式 ---------------- */
function renderStyles(active) {
  const wrap = $('styles');
  wrap.innerHTML = '';
  STYLES.forEach((st) => {
    const tile = document.createElement('div');
    tile.className = 'style-tile' + (st.id === active ? ' on' : '');
    const mini = document.createElement('div');
    mini.className = 'mini';
    mini.style.background = st.bg;
    const mk = (cls, text, bg, fg) => {
      const b = document.createElement('div');
      b.className = 'b ' + cls;
      b.textContent = text;
      if (bg !== 'transparent') b.style.background = bg;
      b.style.color = fg;
      b.style.borderRadius = st.radius + 'px';
      return b;
    };
    mini.appendChild(mk('sys', '样式预览', 'rgba(255,255,255,.12)', '#c9d6e4'));
    mini.appendChild(mk('ai', '你好，有什么可以帮你？', st.aiBg, st.aiFg));
    mini.appendChild(mk('me', '打开打印机灯光', st.userBg, st.userFg));
    tile.appendChild(mini);
    const nm = document.createElement('div');
    nm.className = 'st-name';
    nm.textContent = st.name;
    tile.appendChild(nm);
    tile.onclick = () => {
      renderStyles(st.id);
      queueSave({ chat_style: st.id }, { delay: 0 });
      S.chat_style = st.id;
      renderMock(S);
    };
    wrap.appendChild(tile);
  });
}

/* ---------------- 设备屏幕模拟 ---------------- */
function renderMock(s) {
  const st = STYLES[s.chat_style ?? 0] || STYLES[0];
  const light = (s.theme || 'dark') === 'light';
  const shell = $('mock');
  const statusBar = `<div class="m-status"><span>${s.board || 'device'}</span><span>${
    s.volume ?? 0}%</span></div>`;

  if (s.standby ?? 1) {
    /* 待机页：时钟 + 天气 + 指数 */
    const syms = (s.standby_symbols || '').split(',').filter(Boolean).slice(0, 3);
    const demo = [['纳指', '24089', '+6.51%'], ['标普', '7237', '+4.73%'], ['上证', '3561', '+0.63%']];
    const rowsHtml = syms.map((code, i) => {
      const d = demo[i] || ['—', '—', '—'];
      return `<div style="display:flex;justify-content:space-between;font-size:9px;color:${light ? '#233' : '#cfe3f7'}">
        <span>${d[0]}</span><span style="color:#ff6b6b">${d[1]}</span><span style="color:#ff6b6b">${d[2]}</span></div>`;
    }).join('');
    shell.innerHTML = statusBar + `
      <div class="m-clock" style="background:${light ? '#EDEDED' : 'linear-gradient(#0B1622,#04070C)'}">
        <div class="t">10:42</div>
        <div class="d">11月24日 周一</div>
        <div class="w">多云 12°C  湿度54%</div>
        <div class="s" style="width:150px;color:${light ? '#233' : '#cfe3f7'}">${rowsHtml}</div>
        <div style="font-size:8px;margin-top:10px;color:#00e5ff">说“你好小智”开始对话</div>
      </div>`;
    return;
  }

  /* 对话页 */
  const bg = st.bg;
  shell.innerHTML = statusBar + `
    <div class="m-body" style="background:${bg}">
      <div class="b sys" style="background:rgba(140,150,160,.25);color:${light ? '#333' : '#ddd'};border-radius:${st.radius}px">样式预览</div>
      <div class="b ai" style="background:${st.aiBg === 'transparent' ? 'transparent' : st.aiBg};color:${st.aiFg};border-radius:${st.radius}px">上海今天多云，12 度</div>
      <div class="b me" style="background:${st.userBg === 'transparent' ? 'transparent' : st.userBg};color:${st.userFg};border-radius:${st.radius}px">打开打印机灯光</div>
      <div class="b ai" style="background:${st.aiBg === 'transparent' ? 'transparent' : st.aiBg};color:${st.aiFg};border-radius:${st.radius}px">打印机灯光 已打开</div>
    </div>`;
}

/* ---------------- Tab ---------------- */
document.querySelectorAll('.tabs button').forEach((btn) => {
  btn.onclick = () => {
    document.querySelectorAll('.tabs button').forEach((b) => b.classList.remove('on'));
    document.querySelectorAll('.pane').forEach((p) => p.classList.remove('on'));
    btn.classList.add('on');
    $('pane-' + btn.dataset.tab).classList.add('on');
    if (btn.dataset.tab === 'ctrl') loadGpio();
    window.scrollTo({ top: 0 });
  };
});

/* ---------------- 表单绑定 ---------------- */
$('theme').onchange = (e) => { queueSave({ theme: e.target.value }, { delay: 0 }); S.theme = e.target.value; renderMock(S); };
$('brightness').oninput = (e) => { $('bv').textContent = e.target.value; queueSave({ brightness: +e.target.value }); };
$('volume').oninput = (e) => { $('vv').textContent = e.target.value; queueSave({ volume: +e.target.value }); };
$('duck').oninput = (e) => { $('dv').textContent = e.target.value; queueSave({ duck: +e.target.value }); };
$('standby_on').onchange = (e) => { queueSave({ standby: e.target.checked ? 1 : 0 }, { delay: 0 }); S.standby = e.target.checked ? 1 : 0; renderMock(S); };
$('standby_city').onchange = (e) => queueSave({ standby_city: e.target.value.trim() });
$('standby_symbols').onchange = (e) => { queueSave({ standby_symbols: e.target.value.trim() }); S.standby_symbols = e.target.value; renderMock(S); };
$('wake_on').onchange = (e) => queueSave({ wake_enabled: e.target.checked ? 1 : 0 }, { delay: 0 });
$('wake_threshold').oninput = (e) => { $('wtv').textContent = ((+e.target.value) / 100).toFixed(2); queueSave({ wake_threshold: +e.target.value }); };
$('api').onchange = (e) => queueSave({ api: e.target.value.trim() }, { delay: 0 });

/* ---------------- 图片 ---------------- */
const IMG_NAME = { boot: '开机图片', bg: '待机背景图', chat: '聊天背景图' };
const IMG_EMPTY = { boot: '未设置', bg: '未设置', chat: '未设置' };

function previewImage(kind) {
  const box = $(kind + '_box');
  const im = new Image();
  im.onload = () => { box.innerHTML = ''; box.appendChild(im); };
  im.onerror = () => { box.textContent = IMG_EMPTY[kind]; };
  im.src = '/api/image?kind=' + kind + '&t=' + Date.now();
}

async function clearImage(kind) {
  if (!confirm(`确定清除「${IMG_NAME[kind]}」吗？`)) return;
  const msg = $(kind + '_msg');
  try {
    const r = await fetch('/api/image?kind=' + kind + '&clear=1', { method: 'POST' });
    const j = await r.json();
    msg.textContent = j.ok ? '已清除' : ('失败：' + (j.error || ''));
    msg.className = 'tip ' + (j.ok ? 'ok' : 'warn');
    previewImage(kind);
    if (kind === 'chat') renderMock(S);
  } catch (e) { msg.textContent = '失败：' + e; msg.className = 'tip warn'; }
}

function pickImage(kind) { $(kind + '_file').click(); }
['boot', 'bg', 'chat'].forEach((kind) => {
  $(kind + '_file').addEventListener('change', (e) => {
    if (e.target.files && e.target.files[0]) openCrop(kind, e.target.files[0]);
    e.target.value = '';
  });
  previewImage(kind);
});

/* 点击缩略图看大图 */
['boot', 'bg', 'chat'].forEach((kind) => {
  $(kind + '_box').onclick = () => {
    const img = $(kind + '_box').querySelector('img');
    if (!img) return;
    const w = window.open('', '_blank');
    if (w) w.document.write(`<title>${IMG_NAME[kind]}</title>
      <body style="margin:0;background:#000;display:flex;align-items:center;justify-content:center;height:100vh">
      <img src="${img.src}" style="width:240px;height:320px;image-rendering:pixelated"></body>`);
  };
});

/* ---------------- 裁切弹窗 ---------------- */
const crop = { kind: null, bmp: null, zoom: 1, ox: 0, oy: 0, dragging: false, px: 0, py: 0 };
const canvas = $('crop_canvas');
const ctx = canvas.getContext('2d');

function drawCrop() {
  if (!crop.bmp) return;
  const iw = crop.bmp.width, ih = crop.bmp.height;
  const base = Math.max(IMG_W / iw, IMG_H / ih);
  const k = base * crop.zoom;
  const dw = iw * k, dh = ih * k;
  const maxX = Math.max(0, (dw - IMG_W) / 2), maxY = Math.max(0, (dh - IMG_H) / 2);
  crop.ox = Math.min(maxX, Math.max(-maxX, crop.ox));
  crop.oy = Math.min(maxY, Math.max(-maxY, crop.oy));
  ctx.fillStyle = '#0b1a2a';
  ctx.fillRect(0, 0, IMG_W, IMG_H);
  ctx.drawImage(crop.bmp, (IMG_W - dw) / 2 + crop.ox, (IMG_H - dh) / 2 + crop.oy, dw, dh);
}

function openCrop(kind, file) {
  const msg = $(kind + '_msg');
  msg.textContent = '读取图片…';
  createImageBitmap(file).then((bmp) => {
    crop.kind = kind; crop.bmp = bmp; crop.zoom = 1; crop.ox = 0; crop.oy = 0;
    $('crop_title').textContent = IMG_NAME[kind];
    $('crop_zoom').value = 100; $('crop_zv').textContent = '100';
    $('crop_msg').textContent = `原图 ${bmp.width}×${bmp.height} → 输出 240×320`;
    $('crop_modal').classList.add('on');
    msg.textContent = '';
    drawCrop();
  }).catch((e) => { msg.textContent = '读取失败：' + e; msg.className = 'tip warn'; });
}
function cropCancel() { $('crop_modal').classList.remove('on'); crop.bmp = null; }
function cropReset() {
  crop.zoom = 1; crop.ox = 0; crop.oy = 0;
  $('crop_zoom').value = 100; $('crop_zv').textContent = '100';
  drawCrop();
}

$('crop_zoom').addEventListener('input', (e) => {
  crop.zoom = (+e.target.value) / 100;
  $('crop_zv').textContent = e.target.value;
  drawCrop();
});
canvas.addEventListener('pointerdown', (e) => {
  if (!crop.bmp) return;
  crop.dragging = true; crop.px = e.clientX; crop.py = e.clientY;
  canvas.classList.add('dragging');
  canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener('pointermove', (e) => {
  if (!crop.dragging || !crop.bmp) return;
  const rect = canvas.getBoundingClientRect();
  const sx = IMG_W / rect.width;
  crop.ox += (e.clientX - crop.px) * sx;
  crop.oy += (e.clientY - crop.py) * sx;
  crop.px = e.clientX; crop.py = e.clientY;
  drawCrop();
});
['pointerup', 'pointercancel'].forEach((ev) => canvas.addEventListener(ev, () => {
  crop.dragging = false; canvas.classList.remove('dragging');
}));

$('crop_ok').onclick = async () => {
  if (!crop.bmp) return;
  const kind = crop.kind;
  $('crop_msg').textContent = '生成中…';
  try {
    const blob = await new Promise((r) => canvas.toBlob(r, 'image/jpeg', 0.85));
    $('crop_msg').textContent = `上传中…（${(blob.size / 1024).toFixed(0)}KB）`;
    const r = await fetch('/api/image?kind=' + kind, {
      method: 'POST', headers: { 'Content-Type': 'image/jpeg' }, body: blob,
    });
    const j = await r.json();
    if (j.ok) {
      cropCancel();
      const msg = $(kind + '_msg');
      msg.className = 'tip ok';
      msg.textContent = `已保存（240×320，${(blob.size / 1024).toFixed(0)}KB）`;
      previewImage(kind);
      if (kind === 'chat') renderMock(S);
      toast('图片已保存 ✓');
    } else {
      $('crop_msg').textContent = '失败：' + (j.error || '');
    }
  } catch (e) { $('crop_msg').textContent = '失败：' + e; }
};

/* ---------------- 资源包 ---------------- */
$('applyAssets').onclick = async () => {
  const v = $('variant').value;
  if (!confirm(`应用「${v}」资源包并重启设备？`)) return;
  const msg = $('amsg');
  msg.textContent = '下载中…（设备将自动重启）';
  try {
    const r = await fetch('/api/assets', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ url: 'http://47.116.199.150:2233/assets/' + v + '.bin' }),
    });
    const j = await r.json();
    msg.textContent = j.ok ? '已开始写入，设备即将重启…' : ('失败：' + (j.error || ''));
    msg.className = 'tip ' + (j.ok ? 'ok' : 'warn');
  } catch (e) { msg.textContent = '失败：' + e; msg.className = 'tip warn'; }
};

/* ---------------- GPIO 开关 ---------------- */
let gpioDevices = [], gpioAvailable = [], gpioUsed = [], delTarget = null;

async function loadGpio() {
  try {
    const r = await fetch('/api/gpio', { cache: 'no-store' });
    const j = await r.json();
    gpioDevices = j.devices || [];
    gpioAvailable = j.available || [];
    gpioUsed = j.used || [];
    renderGpio();
  } catch (e) { $('g_msg').textContent = '读取失败：' + e; }
}

function renderGpio() {
  const box = $('gpio_list');
  $('gpio_count').textContent = gpioDevices.length ? gpioDevices.length + ' 路' : '';
  box.innerHTML = '';
  if (!gpioDevices.length) {
    box.innerHTML = '<div class="tip">还没有配置开关（下面可以添加）</div>';
  } else {
    gpioDevices.forEach((d) => {
      const row = document.createElement('div');
      row.className = 'dev';
      row.innerHTML = `
        <div class="dev-l">
          <div class="dev-name">${d.name}</div>
          <div class="dev-sub">GPIO${d.gpio} · ${d.active_high ? '高电平' : '低电平'}${
            d.initial_on ? ' · 上电开' : ''} · ${d.on ? '已打开' : '已关闭'}</div>
        </div>
        <div class="dev-r">
          <label class="sw"><input type="checkbox" ${d.on ? 'checked' : ''}><i></i></label>
          <button class="dev-del">删除</button>
        </div>`;
      const cb = row.querySelector('input[type=checkbox]');
      cb.onchange = () => gpioControl(d.name, cb.checked ? 'on' : 'off');
      row.querySelector('.dev-del').onclick = () => delAsk(d.name);
      box.appendChild(row);
    });
  }

  /* 下拉：只列可用引脚 */
  const sel = $('g_pin');
  const prev = sel.value;
  sel.innerHTML = '';
  if (!gpioAvailable.length) {
    sel.innerHTML = '<option value="">没有可用引脚</option>';
  } else {
    gpioAvailable.forEach((pin) => {
      const o = document.createElement('option');
      o.value = pin; o.textContent = 'GPIO' + pin;
      sel.appendChild(o);
    });
    if (prev && gpioAvailable.includes(+prev)) sel.value = prev;
  }

  /* 已占用引脚说明 */
  $('g_reserved').innerHTML = gpioUsed.length
    ? gpioUsed.map((u) => `GPIO${u.gpio} · ${u.reason}`).join('<br>')
    : '（暂无）';
}

async function gpioControl(name, state) {
  try {
    const r = await fetch('/api/gpio', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, state }),
    });
    const j = await r.json();
    toast(j.message || (j.ok ? '已执行' : '失败'), !j.ok);
    loadGpio();
  } catch (e) { toast('失败：' + e, true); }
}

async function saveGpio(devices, okMsg) {
  try {
    const r = await fetch('/api/gpio', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ devices }),
    });
    const j = await r.json();
    $('g_msg').className = 'tip ' + (j.ok ? 'ok' : 'warn');
    $('g_msg').textContent = j.ok ? (okMsg || '已保存') : ('失败：' + (j.error || ''));
    if (j.ok) loadGpio();
  } catch (e) { $('g_msg').textContent = '失败：' + e; }
}

$('g_add').onclick = () => {
  const name = $('g_name').value.trim();
  const pin = parseInt($('g_pin').value, 10);
  if (!name) { $('g_msg').textContent = '请填写名称'; $('g_msg').className = 'tip warn'; return; }
  if (!(pin >= 0 && pin <= 48)) { $('g_msg').textContent = '没有可用引脚'; $('g_msg').className = 'tip warn'; return; }
  const list = gpioDevices.map((d) => ({ name: d.name, gpio: d.gpio, active_high: d.active_high, initial_on: d.initial_on }));
  list.push({ name, gpio: pin, active_high: +$('g_active').value === 1, initial_on: +$('g_init').value === 1 });
  saveGpio(list, `已添加「${name}」，现在可以说“打开${name}”`);
  $('g_name').value = '';
};

function delAsk(name) {
  delTarget = name;
  $('del_text').textContent = `确定删除「${name}」吗？删除后引脚会释放，语音也不能再控制它。`;
  $('del_modal').classList.add('on');
}
function delCancel() { delTarget = null; $('del_modal').classList.remove('on'); }
function delConfirm() {
  const name = delTarget;
  delCancel();
  if (!name) return;
  const list = gpioDevices.filter((d) => d.name !== name)
    .map((d) => ({ name: d.name, gpio: d.gpio, active_high: d.active_high, initial_on: d.initial_on }));
  saveGpio(list, `已删除「${name}」`);
}

/* ---------------- 诊断 & 系统 ---------------- */
async function showQr() {
  try {
    const r = await fetch('/api/qr?seconds=120', { method: 'POST' });
    const j = await r.json();
    toast(j.ok ? '二维码已显示在设备屏幕上（2 分钟后自动消失）' : '失败：设备还没拿到 IP', !j.ok);
  } catch (e) { toast('失败：' + e, true); }
}

async function chatTest() {
  try {
    await fetch('/api/chat_test', { method: 'POST' });
    toast('已发送样例消息，看设备屏幕');
  } catch (e) { toast('失败：' + e, true); }
}

async function showTools() {
  const box = $('diag');
  box.textContent = '读取中…';
  try {
    const r = await fetch('/api/mcp_tools', { cache: 'no-store' });
    const j = await r.json();
    box.innerHTML = `<b>共 ${j.count} 个工具</b><br>` + (j.tools || []).join('<br>');
  } catch (e) { box.textContent = '失败：' + e; }
}

async function showLog() {
  const box = $('diag');
  box.textContent = '读取中…';
  try {
    const r = await fetch('/api/log?n=120', { cache: 'no-store' });
    const text = await r.text();
    box.innerHTML = '';
    const pre = document.createElement('div');
    pre.style.whiteSpace = 'pre-wrap';
    pre.style.fontSize = '10px';
    pre.style.lineHeight = '1.35';
    pre.textContent = text || '（日志为空）';
    box.appendChild(pre);
  } catch (e) { box.textContent = '失败：' + e; }
}

async function reboot() {
  if (!confirm('确定重启设备吗？')) return;
  toast('重启中…约 8 秒');
  try { await fetch('/api/reboot', { method: 'POST' }); } catch (e) {}
  setTimeout(load, 9000);
}

/* ---------------- 启动 ---------------- */
load();
setInterval(() => { if (!document.hidden) load(); }, 30000);
setInterval(() => { flushSave(); }, 5000);

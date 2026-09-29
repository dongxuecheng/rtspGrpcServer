'use strict';

/* ==================== 小工具 ==================== */

const $ = (sel) => document.querySelector(sel);

const STATUS_CLASS = {
  '已连接': 'ok',
  '连接中': 'warn',
  '无法连接': 'err',
  '不存在': '',
};

const state = {
  streams: [],
  lastSig: '',
  previewId: null,
  previewRetry: null,
  statsTimer: null,
  autoTimer: null,
  decoders: [],
};

function toast(message, type = 'info', timeout = 4000) {
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  el.textContent = message;
  $('#toasts').appendChild(el);
  setTimeout(() => el.remove(), timeout);
}

async function api(path, options = {}) {
  const resp = await fetch(path, { headers: { 'Content-Type': 'application/json' }, ...options });
  const text = await resp.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (_) { /* 非 JSON 响应 */ }
  if (!resp.ok) {
    const detail = (data && (data.detail || data.message)) || text || resp.statusText;
    throw new Error(typeof detail === 'string' ? detail : JSON.stringify(detail));
  }
  return data;
}

function tag(text, title, cls) {
  const span = document.createElement('span');
  span.className = cls ? `tag ${cls}` : 'tag';
  span.textContent = text;
  if (title) span.title = title;
  return span;
}

// Material 风格的极简图标路径
const ICONS = {
  play: 'M8 5v14l11-7z',
  stop: 'M6 6h12v12H6z',
  camera: 'M9 3h6l1 2h3a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h3l1-2zm3 5a5 5 0 1 0 0 10 5 5 0 0 0 0-10z',
  edit: 'M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z',
  copy: 'M16 1H4a2 2 0 0 0-2 2v14h2V3h12V1zm3 4H8a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2z',
  refresh: 'M17.65 6.35A8 8 0 1 0 19.73 14h-2.08A6 6 0 1 1 12 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z',
  close: 'M19 6.41 17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z',
  plus: 'M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z',
};

function icon(name, size = 14) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('width', size);
  svg.setAttribute('height', size);
  svg.setAttribute('fill', 'currentColor');
  svg.setAttribute('aria-hidden', 'true');
  svg.innerHTML = ICONS[name] || '';
  return svg;
}

function button(text, cls, onClick, iconName) {
  const b = document.createElement('button');
  b.className = cls;
  if (iconName) b.appendChild(icon(iconName));
  if (text) {
    const span = document.createElement('span');
    span.textContent = text;
    b.appendChild(span);
  }
  b.onclick = onClick;
  return b;
}

/* ==================== 复制（兼容非安全上下文）==================== */

async function copyText(text) {
  // 安全上下文（https / localhost）优先用 Clipboard API
  if (window.isSecureContext && navigator.clipboard && navigator.clipboard.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (_) { /* 落到下面的兜底 */ }
  }
  // 非安全上下文（如 http://局域网IP）浏览器不提供 Clipboard API，用 execCommand 兜底
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.top = '-1000px';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, text.length);
    const ok = document.execCommand('copy');
    ta.remove();
    return ok;
  } catch (_) {
    return false;
  }
}

async function copyStreamId(streamId) {
  if (await copyText(streamId)) {
    toast('已复制 stream_id', 'ok', 1500);
    return;
  }
  // 浏览器完全禁止自动复制时，给一个可手动复制的对话框（已全选，Ctrl/Cmd+C 即可）
  await openDialog({
    type: 'prompt',
    title: '手动复制',
    desc: '浏览器不允许自动复制（通常因为页面不是 HTTPS），请按 Ctrl/Cmd + C：',
    value: streamId,
    readonly: true,
    multiline: true,
    okText: '完成',
  });
}

/* ==================== 模态对话框 ==================== */

function openDialog(opts = {}) {
  return new Promise((resolve) => {
    const mask = $('#modal');
    const input = $('#modal-input');
    const err = $('#modal-error');
    const okBtn = $('#modal-ok');
    const cancelBtn = $('#modal-cancel');
    const isPrompt = opts.type !== 'confirm';

    $('#modal-title').textContent = opts.title || '';
    const descEl = $('#modal-desc');
    descEl.textContent = opts.desc || '';
    descEl.hidden = !opts.desc;

    $('#modal-field').hidden = !isPrompt;
    input.value = opts.value || '';
    input.placeholder = opts.placeholder || '';
    input.readOnly = !!opts.readonly;
    input.rows = opts.multiline ? 4 : 1;
    err.textContent = '';
    err.hidden = true;

    okBtn.textContent = opts.okText || '确定';
    okBtn.className = opts.danger ? 'danger' : 'primary';
    okBtn.disabled = false;

    mask.hidden = false;

    const close = (result) => {
      mask.hidden = true;
      okBtn.onclick = cancelBtn.onclick = mask.onclick = document.onkeydown = null;
      resolve(result);
    };
    const cancel = () => close(isPrompt ? null : false);

    const accept = async () => {
      if (!isPrompt) { close(true); return; }

      const value = input.value.trim();
      if (opts.validate) {
        const msg = opts.validate(value);
        if (msg) {
          err.textContent = msg;
          err.hidden = false;
          input.focus();
          return;
        }
      }

      if (opts.submit) {
        const label = okBtn.textContent;
        okBtn.disabled = true;
        okBtn.textContent = '处理中…';
        try {
          await opts.submit(value);
        } catch (e) {
          err.textContent = e.message || String(e);
          err.hidden = false;
          okBtn.disabled = false;
          okBtn.textContent = label;
          input.focus();
          return;
        }
        okBtn.disabled = false;
        okBtn.textContent = label;
      }
      close(value);
    };

    okBtn.onclick = accept;
    cancelBtn.onclick = cancel;
    mask.onclick = (e) => { if (e.target === mask) cancel(); };
    document.onkeydown = (e) => {
      if (e.key === 'Escape') { e.preventDefault(); cancel(); }
      else if (e.key === 'Enter' && (!opts.multiline || e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        accept();
      }
    };

    setTimeout(() => {
      if (isPrompt) {
        input.focus();
        input.select();   // 便于直接粘贴新 URL，或 Ctrl+C 手工复制
      } else {
        okBtn.focus();
      }
    }, 0);
  });
}

/* ==================== 顶部状态 ==================== */

async function loadMeta() {
  try {
    const meta = await api('/api/meta');
    $('#server-addr').textContent = `服务端: ${meta.server}`;
    const sel = $('#f-decoder');
    sel.textContent = '';
    meta.decoders.forEach((d) => {
      const o = document.createElement('option');
      o.value = String(d.value);
      o.textContent = d.label;
      sel.appendChild(o);
    });

    // 原始帧格式（仅 SHM 模式生效）；旧服务端可能不返回 pixel_formats，此时隐藏该项
    const psel = $('#f-pixel');
    if (psel) {
      psel.textContent = '';
      (meta.pixel_formats || []).forEach((p) => {
        const o = document.createElement('option');
        o.value = String(p.value);
        o.textContent = p.label;
        psel.appendChild(o);
      });
      const wrap = psel.closest('label');
      if (wrap) wrap.style.display = (meta.pixel_formats && meta.pixel_formats.length) ? '' : 'none';
    }
  } catch (e) {
    toast(`读取配置失败：${e.message}`, 'err');
  }
}

async function loadHealth() {
  const badge = $('#health');
  try {
    const h = await api('/api/health');
    if (h.ok) {
      badge.className = 'badge ok';
      badge.textContent = `在线 · ${h.stream_count} 路流`;
    } else {
      badge.className = 'badge err';
      badge.textContent = '服务端不可达';
      badge.title = h.error || '';
    }
  } catch (e) {
    badge.className = 'badge err';
    badge.textContent = '后端异常';
    badge.title = e.message;
  }
}

/* ==================== 流列表 ==================== */

function renderStreams(streams) {
  const tbody = $('#stream-rows');
  tbody.textContent = '';

  streams.forEach((s) => {
    const tr = document.createElement('tr');

    // --- ID（缩短显示 + 复制） ---
    const tdId = document.createElement('td');
    const idSpan = document.createElement('span');
    idSpan.className = 'mono';
    idSpan.textContent = s.stream_id.length > 22 ? `${s.stream_id.slice(0, 22)}…` : s.stream_id;
    idSpan.title = s.stream_id;
    tdId.append(idSpan, button('', 'mini ghost', () => copyStreamId(s.stream_id), 'copy'));

    // --- URL ---
    const tdUrl = document.createElement('td');
    tdUrl.className = 'wrap mono';
    tdUrl.textContent = s.rtsp_url;
    tdUrl.title = s.rtsp_url;

    // --- 状态 ---
    const tdStatus = document.createElement('td');
    const pill = document.createElement('span');
    pill.className = `pill ${STATUS_CLASS[s.status_name] || ''}`;
    pill.textContent = s.status_name;
    tdStatus.appendChild(pill);

    // --- 解码器 ---
    const tdDecoder = document.createElement('td');
    tdDecoder.textContent = s.decoder_type;

    // --- 分辨率 ---
    const tdRes = document.createElement('td');
    tdRes.className = 'mono';
    tdRes.textContent = s.width && s.height ? `${s.width}×${s.height}` : '—';

    // --- 帧率（服务端真实出帧率，由 C++ 侧统计） ---
    const tdFps = document.createElement('td');
    tdFps.className = 'mono';
    const fps = Number(s.fps || 0);
    const lagMs = Number(s.media_lag_ms || 0);
    tdFps.textContent = fps > 0 ? fps.toFixed(1) : '—';
    if (fps > 0) {
      // 与配置的抽帧间隔对比，方便一眼看出是否被限速
      const cap = s.decode_interval_ms > 0 ? 1000 / s.decode_interval_ms : 0;
      const lagTxt = lagMs > 0 ? `，落后源 ${lagMs}ms` : '，未落后于源';
      tdFps.title = (cap > 0
        ? `服务端出帧 ${fps.toFixed(2)} FPS（抽帧间隔 ${s.decode_interval_ms}ms 理论上限 ${cap.toFixed(1)} FPS）`
        : `服务端出帧 ${fps.toFixed(2)} FPS（未限制抽帧间隔）`) + lagTxt;
    } else {
      tdFps.title = '没有新帧（未连接 / 首帧未到 / 已停流）';
    }

    // --- 选项 ---
    const tdOpt = document.createElement('td');
    tdOpt.appendChild(tag(s.use_shared_mem ? 'SHM' : 'JPEG', s.use_shared_mem
      ? '共享内存零拷贝（需客户端与服务端同机）' : 'gRPC JPEG 传输'));
    // SHM 流额外显示原始帧像素格式（NV12/I420/YUYV422；BGR24 是默认值不显示）
    if (s.use_shared_mem && s.pixel_format_name && s.pixel_format_name !== 'BGR24') {
      tdOpt.appendChild(tag(s.pixel_format_name,
        `共享内存里的原始帧是 ${s.pixel_format_name}，客户端无需再从 BGR 转换（如 Ascend DVPP）。`, 'warn'));
    }
    if (s.only_key_frames) tdOpt.appendChild(tag('仅关键帧'));
    if (s.keep_on_failure) tdOpt.appendChild(tag('失败保持'));
    if (s.decode_interval_ms > 0) tdOpt.appendChild(tag(`间隔 ${s.decode_interval_ms}ms`));
    tdOpt.appendChild(tag(`心跳 ${Math.round(s.heartbeat_timeout_ms / 1000)}s`));
    // 花屏（解码器自报的错误帧）：
    //   glitch_ratio > 0 = 最近 1 秒正在花屏；corrupted_frames 是累计值
    const glitchRatio = Number(s.glitch_ratio || 0);
    const corrupted = Number(s.corrupted_frames || 0);
    if (glitchRatio > 0) {
      tdOpt.appendChild(tag(`花屏 ${(glitchRatio * 100).toFixed(0)}%`,
        `最近 1 秒约 ${(glitchRatio * 100).toFixed(1)}% 的帧解码出错（缺参考帧/错误掩盖），`
        + '画面上就是灰块/绿块/马赛克。累计 ' + corrupted + ' 帧。'
        + '走 TCP 时通常是相机侧码流异常，可考虑重连或改用关键帧模式。', 'warn'));
    } else if (corrupted > 0) {
      tdOpt.appendChild(tag(`花屏累计 ${corrupted}`,
        `曾经出现 ${corrupted} 帧解码错误（当前已恢复正常）`, 'muted'));
    } else if (s.status_name === '已连接') {
      // 已连接且从无花屏：也明确显示一次，否则“无花屏”与“功能未生效”看起来一样
      tdOpt.appendChild(tag('无花屏',
        '解码器从未上报花屏（缺参考帧 / 错误掩盖 / 码流非法），画面正常', 'ok'));
    }
    // 落后源超过 1 秒：说明接收缓冲在堆积（延迟会继续变大），醒目提示
    if (lagMs > 1000) {
      tdOpt.appendChild(tag(`落后 ${(lagMs / 1000).toFixed(1)}s`, '落后于源：接收缓冲堆积，延迟持续增大'));
    }

    // --- 操作 ---
    const tdAct = document.createElement('td');
    const actions = document.createElement('div');
    actions.className = 'actions';
    actions.append(
      button('预览', 'mini primary', () => openPreview(s.stream_id), 'play'),
      button('截图', 'mini ghost', () => snapshot(s.stream_id), 'camera'),
      button('改 URL', 'mini ghost', () => changeUrl(s), 'edit'),
      button('停止', 'mini danger-ghost', () => stopStream(s.stream_id), 'stop'),
    );
    tdAct.appendChild(actions);

    tr.append(tdId, tdUrl, tdStatus, tdDecoder, tdRes, tdFps, tdOpt, tdAct);
    tbody.appendChild(tr);
  });

  $('#count').textContent = streams.length ? `共 ${streams.length} 路` : '';
  $('#empty').hidden = streams.length > 0;
  renderStats(streams);
}

function renderStats(streams) {
  const count = (name) => streams.filter((s) => s.status_name === name).length;
  $('#stat-total').textContent = streams.length;
  $('#stat-connected').textContent = count('已连接');
  $('#stat-connecting').textContent = count('连接中');
  $('#stat-error').textContent = count('无法连接') + count('不存在');
}

async function loadStreams() {
  try {
    const data = await api('/api/streams');
    const streams = data.streams || [];
    const sig = JSON.stringify(streams);
    if (sig === state.lastSig) return;   // 无变化则跳过重绘，避免打断点击
    state.lastSig = sig;
    state.streams = streams;
    renderStreams(streams);
  } catch (e) {
    toast(`刷新流列表失败：${e.message}`, 'err');
  }
}

/* ==================== 操作 ==================== */

async function createStream(event) {
  event.preventDefault();
  const btn = $('#btn-create');
  const label = state.createLabel || btn;
  const body = {
    rtsp_url: $('#f-url').value.trim(),
    decoder_type: Number($('#f-decoder').value || 0),
    gpu_id: Number($('#f-gpu').value || 0),
    heartbeat_timeout_ms: Number($('#f-hb').value || 100000),
    decode_interval_ms: Number($('#f-interval').value || 0),
    only_key_frames: $('#f-keyframe').checked,
    use_shared_mem: $('#f-shm').checked,
    pixel_format: Number($('#f-pixel') ? $('#f-pixel').value : 0),
    keep_on_failure: $('#f-keep').checked,
  };
  btn.disabled = true;
  label.textContent = '创建中…';
  try {
    const res = await api('/api/streams', { method: 'POST', body: JSON.stringify(body) });
    toast(`已创建：${res.stream_id}`, 'ok');
    // 创建成功不代表配置生效：服务端可能降级解码器（CPU-only 构建 / GPU 不可用）
    // 或复用了同 URL 的已有流（配置沿用原有流）。这类差异必须显式提示。
    (res.warnings || []).forEach((w) => toast(`注意：${w}`, 'warn', 10000));
    await loadStreams();
    await loadHealth();
  } catch (e) {
    toast(`创建失败：${e.message}`, 'err');
  } finally {
    btn.disabled = false;
    label.textContent = '创建任务';
  }
}

async function stopStream(streamId) {
  const ok = await openDialog({
    type: 'confirm',
    title: '停止流',
    desc: `确定停止该任务吗？\n${streamId}`,
    okText: '停止',
    danger: true,
  });
  if (!ok) return;
  try {
    await api(`/api/streams/${encodeURIComponent(streamId)}/stop`, { method: 'POST' });
    toast('已停止', 'ok');
    if (state.previewId === streamId) closePreview();
    await loadStreams();
    await loadHealth();
  } catch (e) {
    toast(`停止失败：${e.message}`, 'err');
  }
}

async function stopAll() {
  if (!state.streams.length) { toast('当前没有流', 'warn'); return; }
  const ok = await openDialog({
    type: 'confirm',
    title: '全部停止',
    desc: `确定停止全部 ${state.streams.length} 路流吗？`,
    okText: '全部停止',
    danger: true,
  });
  if (!ok) return;
  try {
    const res = await api('/api/streams/stop-all', { method: 'POST' });
    toast(`已停止 ${res.stopped.length} 路${res.failed.length ? `，失败 ${res.failed.length} 路` : ''}`,
      res.failed.length ? 'warn' : 'ok');
    closePreview();
    await loadStreams();
    await loadHealth();
  } catch (e) {
    toast(`批量停止失败：${e.message}`, 'err');
  }
}

async function changeUrl(stream) {
  const value = await openDialog({
    type: 'prompt',
    title: '修改 RTSP URL',
    desc: `任务 ${stream.stream_id}\n当前：${stream.rtsp_url}`,
    value: stream.rtsp_url,
    multiline: true,
    okText: '保存',
    placeholder: 'rtsp://admin:密码@172.16.22.16:554/Streaming/Channels/101',
    validate: (v) => {
      if (!v) return 'URL 不能为空';
      if (!/^(rtsp|hik):\/\//i.test(v)) return 'URL 需以 rtsp:// 或 hik:// 开头';
      if (v === stream.rtsp_url) return '与当前 URL 相同，无需修改';
      return null;
    },
    submit: async (v) => {
      await api(`/api/streams/${encodeURIComponent(stream.stream_id)}`,
        { method: 'PATCH', body: JSON.stringify({ new_rtsp_url: v }) });
    },
  });
  if (value === null) return;
  toast('URL 已更新，任务将重连', 'ok');
  await loadStreams();
}

async function snapshot(streamId) {
  try {
    const resp = await fetch(`/api/streams/${encodeURIComponent(streamId)}/snapshot.jpg?quality=90&t=${Date.now()}`);
    if (!resp.ok) {
      const text = await resp.text();
      let detail = text;
      try { detail = JSON.parse(text).detail || text; } catch (_) { /* 忽略 */ }
      throw new Error(detail || `HTTP ${resp.status}`);
    }
    const blob = await resp.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `${streamId}_${new Date().toISOString().replace(/[:.]/g, '-')}.jpg`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 5000);
    toast('已保存截图', 'ok');
  } catch (e) {
    toast(`截图失败：${e.message}`, 'err');
  }
}

/* ==================== 预览（canvas + MJPEG 解析，可测延迟并自动重新同步）==================== */

// 端到端延迟超过该值且持续 2s → 重新连接，丢弃隧道/浏览器侧积压
const PREVIEW_LAG_RESYNC_MS = 1500;
const PREVIEW_RESYNC_MIN_GAP_MS = 3000;

const viewer = {
  ctrl: null,          // AbortController
  running: false,
  lagBase: Infinity,   // 校准基准：min(本地时间 - 帧时间戳)，抵消两端时钟偏差
  lagMs: 0,
  frames: 0,           // 客户端已渲染帧数
  fpsT0: 0,
  fpsCount: 0,
  fps: 0,
  laggingSince: 0,
  lastResync: 0,
  quality: '960|75|10',  // max_width|quality|fps
};

function concatBytes(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

function indexOfSeq(buf, seq, from = 0) {
  outer:
  for (let i = from; i <= buf.length - seq.length; i++) {
    for (let j = 0; j < seq.length; j++) {
      if (buf[i + j] !== seq[j]) continue outer;
    }
    return i;
  }
  return -1;
}

// 从缓冲区里取出一个完整的 MJPEG 分片；数据不够则返回 null
function nextJpegPart(buf) {
  const he = indexOfSeq(buf, [13, 10, 13, 10]);
  if (he < 0) return null;
  const header = new TextDecoder('latin1').decode(buf.subarray(0, he));
  const lenMatch = /content-length:\s*(\d+)/i.exec(header);
  if (!lenMatch) {
    // 不是带长度头的分片（例如结束边界），丢弃这段头继续
    return { jpeg: null, ts: 0, rest: buf.subarray(he + 4) };
  }
  const len = parseInt(lenMatch[1], 10);
  const start = he + 4;
  if (buf.length < start + len) return null;  // 等更多数据
  const tsMatch = /x-frame-ts:\s*(\d+)/i.exec(header);
  return {
    jpeg: buf.slice(start, start + len),
    ts: tsMatch ? parseInt(tsMatch[1], 10) : 0,
    // 服务端逐帧花屏标记（解码器自报）
    glitch: /x-frame-glitch:\s*1/i.test(header),
    rest: buf.subarray(start + len),
  };
}

async function drawJpeg(ctx, canvas, jpeg) {
  const bmp = await createImageBitmap(new Blob([jpeg], { type: 'image/jpeg' }));
  if (canvas.width !== bmp.width || canvas.height !== bmp.height) {
    canvas.width = bmp.width;
    canvas.height = bmp.height;
  }
  ctx.drawImage(bmp, 0, 0);
  bmp.close();
}

// 当前这一帧是否花屏（服务端在 MJPEG 分片头里带 X-Frame-Glitch）
let glitchTimer = 0;
function showFrameGlitch(on) {
  const el = $('#preview-glitch');
  if (!el) return;
  if (on) {
    el.hidden = false;
    clearTimeout(glitchTimer);
    glitchTimer = setTimeout(() => { el.hidden = true; }, 1500);
  }
}

function previewUrl(streamId) {  const [w, q, fps] = viewer.quality.split('|');
  return `/api/streams/${encodeURIComponent(streamId)}/mjpeg`
    + `?max_width=${w}&quality=${q}&fps=${fps}&_=${Date.now()}`;
}

function stopPreviewStream() {
  viewer.running = false;
  if (viewer.ctrl) {
    try { viewer.ctrl.abort(); } catch (_) { /* 忽略 */ }
    viewer.ctrl = null;
  }
  clearTimeout(state.previewRetry);
}

function schedulePreviewRetry(streamId) {
  clearTimeout(state.previewRetry);
  state.previewRetry = setTimeout(() => {
    if (state.previewId === streamId) startPreviewStream();
  }, 3000);
}

async function startPreviewStream() {
  const id = state.previewId;
  if (!id) return;
  stopPreviewStream();

  const ctrl = new AbortController();
  viewer.ctrl = ctrl;
  viewer.running = true;
  viewer.lagBase = Infinity;
  viewer.lagMs = 0;
  viewer.laggingSince = 0;
  viewer.fpsT0 = performance.now();
  viewer.fpsCount = 0;
  viewer.fps = 0;

  const canvas = $('#preview-canvas');
  const ctx = canvas.getContext('2d', { alpha: false });
  const hint = $('#preview-hint');
  hint.hidden = true;
  canvas.hidden = false;

  let buf = new Uint8Array(0);
  try {
    const resp = await fetch(previewUrl(id), { signal: ctrl.signal, cache: 'no-store' });
    if (!resp.ok || !resp.body) throw new Error(`HTTP ${resp.status}`);
    const reader = resp.body.getReader();

    while (viewer.running && viewer.ctrl === ctrl) {
      const { value, done } = await reader.read();
      if (done) break;
      buf = concatBytes(buf, value);

      // 尽量把本次收到的分片都取出来，只渲染最后一帧（积压时自动丢中间帧）
      let newest = null;
      let newestTs = 0;
      let newestGlitch = false;
      let got = 0;
      for (;;) {
        const part = nextJpegPart(buf);
        if (!part) break;
        buf = part.rest;
        if (part.jpeg) { newest = part.jpeg; newestTs = part.ts; newestGlitch = part.glitch; got += 1; }
      }
      if (!newest) continue;

      await drawJpeg(ctx, canvas, newest);
      viewer.frames += 1;
      viewer.fpsCount += got;
      viewer.lastSize = `${canvas.width}×${canvas.height}`;
      $('#preview-live').hidden = false;
      $('#preview-hint').hidden = true;
      // 当前这一帧是否花屏（服务端解码器自报）：亮 1.5 秒后自动隐藏
      showFrameGlitch(newestGlitch);

      const now = performance.now();
      if (now - viewer.fpsT0 >= 1000) {
        viewer.fps = viewer.fpsCount * 1000 / (now - viewer.fpsT0);
        viewer.fpsCount = 0;
        viewer.fpsT0 = now;
      }

      if (newestTs) {
        // 用会话内最小值做基准：同时抵消两端时钟偏差与初始缓冲
        const raw = Date.now() - newestTs;
        if (raw < viewer.lagBase) viewer.lagBase = raw;
        viewer.lagMs = Math.max(0, raw - viewer.lagBase);
        maybeResync(id, viewer.lagMs);
      }
      renderPreviewStats();
    }

    if (viewer.running && viewer.ctrl === ctrl) {
      hint.hidden = false;
      hint.textContent = '暂无新帧（流未连接或服务端长时间无帧），3 秒后重试…';
      schedulePreviewRetry(id);
    }
  } catch (e) {
    if (viewer.running && viewer.ctrl === ctrl && e.name !== 'AbortError') {
      hint.hidden = false;
      hint.textContent = `预览中断：${e.message}（3 秒后重试）`;
      schedulePreviewRetry(id);
    }
  }
}

function maybeResync(streamId, lagMs) {
  const now = Date.now();
  if (lagMs <= PREVIEW_LAG_RESYNC_MS) {
    viewer.laggingSince = 0;
    return;
  }
  if (!viewer.laggingSince) {
    viewer.laggingSince = now;
    return;
  }
  if (now - viewer.laggingSince < 2000 || now - viewer.lastResync < PREVIEW_RESYNC_MIN_GAP_MS) {
    return;
  }
  viewer.lastResync = now;
  viewer.laggingSince = 0;

  // 30 秒内反复积压 → 判定带宽不足，自动降一档画质
  viewer.resyncs = (viewer.resyncs || []).filter((t) => now - t < 30000);
  viewer.resyncs.push(now);
  if (viewer.resyncs.length >= 3) {
    viewer.resyncs = [];
    if (downgradeQuality()) {
      toast('网络带宽不足，已自动降低预览画质', 'warn');
    } else {
      toast('网络带宽不足：已是流畅画质，建议改用 gRPC 模式或降低分辨率', 'warn');
    }
  } else {
    toast(`检测到预览积压 ${(lagMs / 1000).toFixed(1)}s，已重新同步（丢弃缓冲）`, 'warn');
  }
  startPreviewStream();
}

function downgradeQuality() {
  const order = ['480|60|8', '960|75|10', '1600|85|10'];
  const i = order.indexOf(viewer.quality);
  if (i <= 0) return false;
  viewer.quality = order[i - 1];
  const sel = $('#preview-quality');
  if (sel) sel.value = viewer.quality;
  return true;
}

function renderPreviewStats() {
  const el = $('#preview-stats');
  const server = state.serverStats || {};
  const lagTxt = viewer.lagBase === Infinity
    ? '—'
    : (viewer.lagMs >= 1000 ? `${(viewer.lagMs / 1000).toFixed(1)}s` : `${Math.round(viewer.lagMs)}ms`);
  const stale = viewer.lagMs > PREVIEW_LAG_RESYNC_MS;
  const srcTxt = server.server_fps ? `服务端出帧 ${server.server_fps.toFixed(1)} FPS` : '服务端出帧 —';
  const sendTxt = server.send_fps ? `预览发送 ${server.send_fps.toFixed(1)} FPS` : '预览发送 —';
  const sizeTxt = viewer.lastSize ? ` · ${viewer.lastSize}` : '';
  // 花屏状态常显：无花屏时也要能看到（否则“正常”与“功能没生效”看起来一样）
  const gRatio = Number(server.server_glitch_ratio || 0);
  const gTotal = Number(server.server_corrupted_frames || 0);
  const glitchTxt = gRatio > 0
    ? ` · 花屏 ${(gRatio * 100).toFixed(0)}%`
    : (gTotal > 0 ? ` · 花屏累计 ${gTotal}` : ' · 无花屏');

  el.textContent = `${srcTxt} → ${sendTxt} → 客户端 ${viewer.fps.toFixed(1)} FPS · 端到端延迟 ${lagTxt}${sizeTxt}${glitchTxt}`
    + (stale ? ' ⚠' : '');
  el.className = stale || gRatio > 0 ? 'muted warn-text' : 'muted';
  el.title = '服务端出帧：服务端每秒发布的新帧数（与预览限速无关）'
    + '；预览发送：本进程向浏览器实际发出的帧率（受当前画质档位的 fps 限制）'
    + '；客户端：浏览器实际渲染帧率。三者差距定位瓶颈，端到端延迟含网络/隧道积压';
}

async function refreshServerStats() {
  const id = state.previewId;
  if (!id) return;
  try {
    state.serverStats = await api(`/api/streams/${encodeURIComponent(id)}/stats`);
  } catch (_) {
    state.serverStats = null;
  }
  renderPreviewStats();
}

function openPreview(streamId) {
  state.previewId = streamId;
  state.serverStats = null;
  $('#preview-card').hidden = false;
  $('#preview-id').textContent = streamId;
  startPreviewStream();
  clearInterval(state.statsTimer);
  state.statsTimer = setInterval(refreshServerStats, 1000);
  refreshServerStats();
  $('#preview-card').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function closePreview() {
  state.previewId = null;
  state.serverStats = null;
  stopPreviewStream();
  clearInterval(state.statsTimer);
  $('#preview-card').hidden = true;
  $('#preview-canvas').hidden = true;
  $('#preview-live').hidden = true;
  $('#preview-glitch').hidden = true;
  clearTimeout(glitchTimer);
  $('#preview-hint').hidden = true;
  $('#preview-stats').textContent = '';
}


/* ==================== 初始化 ==================== */

function bindAutoRefresh() {
  const tick = () => {
    if ($('#auto-refresh').checked) { loadStreams(); loadHealth(); }
  };
  clearInterval(state.autoTimer);
  state.autoTimer = setInterval(tick, 2000);
  $('#auto-refresh').addEventListener('change', tick);
}

function tickClock() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  $('#clock').textContent = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function init() {
  $('#create-form').addEventListener('submit', createStream);
  $('#btn-refresh').addEventListener('click', () => { loadStreams(); loadHealth(); toast('已刷新', 'ok', 1200); });
  $('#btn-stop-all').addEventListener('click', stopAll);
  $('#btn-snapshot').addEventListener('click', () => state.previewId && snapshot(state.previewId));
  $('#btn-close-preview').addEventListener('click', closePreview);
  $('#btn-preview-resync').addEventListener('click', () => {
    if (!state.previewId) return;
    viewer.lastResync = Date.now();
    viewer.laggingSince = 0;
    toast('已重新同步预览（丢弃积压缓冲）', 'ok', 1500);
    startPreviewStream();
  });
  $('#preview-quality').addEventListener('change', (e) => {
    viewer.quality = e.target.value;
    if (state.previewId) {
      toast('已切换预览画质', 'ok', 1200);
      startPreviewStream();
    }
  });

  // 创建按钮加图标（保留可更新的文字节点）
  const createBtn = $('#btn-create');
  createBtn.textContent = '';
  const label = document.createElement('span');
  label.textContent = '创建任务';
  createBtn.append(icon('plus', 15), label);
  state.createLabel = label;

  // 其余带文字的按钮统一补图标
  const withIcon = (sel, name) => {
    const el = $(sel);
    if (el) el.prepend(icon(name, 14));
  };
  withIcon('#btn-refresh', 'refresh');
  withIcon('#btn-stop-all', 'stop');
  withIcon('#btn-preview-resync', 'refresh');
  withIcon('#btn-snapshot', 'camera');
  withIcon('#btn-close-preview', 'close');

  loadMeta();
  loadHealth();
  loadStreams();
  bindAutoRefresh();
  tickClock();
  setInterval(tickClock, 1000);
}

document.addEventListener('DOMContentLoaded', init);

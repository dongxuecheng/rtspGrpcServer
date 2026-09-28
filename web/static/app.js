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

function tag(text, title) {
  const span = document.createElement('span');
  span.className = 'tag';
  span.textContent = text;
  if (title) span.title = title;
  return span;
}

function button(text, cls, onClick) {
  const b = document.createElement('button');
  b.className = cls;
  b.textContent = text;
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
    tdId.append(idSpan, button('复制', 'mini', () => copyStreamId(s.stream_id)));

    // --- URL ---
    const tdUrl = document.createElement('td');
    tdUrl.className = 'wrap mono';
    tdUrl.textContent = s.rtsp_url;
    tdUrl.title = s.rtsp_url;

    // --- 状态 ---
    const tdStatus = document.createElement('td');
    const badge = document.createElement('span');
    badge.className = `badge ${STATUS_CLASS[s.status_name] || ''}`;
    badge.textContent = s.status_name;
    tdStatus.appendChild(badge);

    // --- 解码器 ---
    const tdDecoder = document.createElement('td');
    tdDecoder.textContent = s.decoder_type;

    // --- 分辨率 ---
    const tdRes = document.createElement('td');
    tdRes.className = 'mono';
    tdRes.textContent = s.width && s.height ? `${s.width}×${s.height}` : '—';

    // --- 选项 ---
    const tdOpt = document.createElement('td');
    tdOpt.appendChild(tag(s.use_shared_mem ? 'SHM' : 'JPEG', s.use_shared_mem
      ? '共享内存零拷贝（需客户端与服务端同机）' : 'gRPC JPEG 传输'));
    if (s.only_key_frames) tdOpt.appendChild(tag('仅关键帧'));
    if (s.keep_on_failure) tdOpt.appendChild(tag('失败保持'));
    if (s.decode_interval_ms > 0) tdOpt.appendChild(tag(`间隔 ${s.decode_interval_ms}ms`));
    tdOpt.appendChild(tag(`心跳 ${Math.round(s.heartbeat_timeout_ms / 1000)}s`));

    // --- 操作 ---
    const tdAct = document.createElement('td');
    const actions = document.createElement('div');
    actions.className = 'actions';
    actions.append(
      button('预览', 'mini', () => openPreview(s.stream_id)),
      button('截图', 'mini', () => snapshot(s.stream_id)),
      button('改 URL', 'mini', () => changeUrl(s)),
      button('停止', 'mini danger', () => stopStream(s.stream_id)),
    );
    tdAct.appendChild(actions);

    tr.append(tdId, tdUrl, tdStatus, tdDecoder, tdRes, tdOpt, tdAct);
    tbody.appendChild(tr);
  });

  $('#count').textContent = streams.length ? `共 ${streams.length} 路` : '';
  $('#empty').hidden = streams.length > 0;
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
  const body = {
    rtsp_url: $('#f-url').value.trim(),
    decoder_type: Number($('#f-decoder').value || 0),
    gpu_id: Number($('#f-gpu').value || 0),
    heartbeat_timeout_ms: Number($('#f-hb').value || 100000),
    decode_interval_ms: Number($('#f-interval').value || 0),
    only_key_frames: $('#f-keyframe').checked,
    use_shared_mem: $('#f-shm').checked,
    keep_on_failure: $('#f-keep').checked,
  };
  btn.disabled = true;
  btn.textContent = '创建中…';
  try {
    const res = await api('/api/streams', { method: 'POST', body: JSON.stringify(body) });
    toast(`已创建：${res.stream_id}`, 'ok');
    await loadStreams();
    await loadHealth();
  } catch (e) {
    toast(`创建失败：${e.message}`, 'err');
  } finally {
    btn.disabled = false;
    btn.textContent = '创建';
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

/* ==================== 预览 ==================== */

function openPreview(streamId) {
  state.previewId = streamId;
  $('#preview-card').hidden = false;
  $('#preview-id').textContent = streamId;
  loadPreviewFrame();
  refreshStats();
  clearInterval(state.statsTimer);
  state.statsTimer = setInterval(refreshStats, 1000);
  $('#preview-card').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function loadPreviewFrame() {
  const id = state.previewId;
  if (!id) return;
  const img = $('#preview-img');
  const hint = $('#preview-hint');
  hint.hidden = true;
  img.hidden = false;
  img.onload = () => { hint.hidden = true; img.hidden = false; };
  img.onerror = () => {
    // 服务端长时间无新帧时后端会结束该 MJPEG 流，这里提示并自动重试
    img.hidden = true;
    hint.hidden = false;
    hint.textContent = '等待帧…（流未连接，或该流暂无新帧；3 秒后自动重试）';
    clearTimeout(state.previewRetry);
    state.previewRetry = setTimeout(() => { if (state.previewId === id) loadPreviewFrame(); }, 3000);
  };
  img.src = `/api/streams/${encodeURIComponent(id)}/mjpeg?fps=10&max_width=1280&quality=80&t=${Date.now()}`;
}

async function refreshStats() {
  const id = state.previewId;
  if (!id) return;
  try {
    const st = await api(`/api/streams/${encodeURIComponent(id)}/stats`);
    const ageMs = st.last_ts ? Math.max(0, Date.now() - st.last_ts) : null;
    const ageTxt = ageMs === null
      ? '—'
      : (ageMs >= 1000 ? `${(ageMs / 1000).toFixed(1)}s` : `${ageMs}ms`);
    const stale = ageMs !== null && ageMs > 2000;

    const el = $('#preview-stats');
    el.textContent = `预览 ${st.fps.toFixed(1)} FPS · 已发送 ${st.sent} 帧 · 帧龄 ${ageTxt}`
      + (stale ? ' ⚠' : '');
    el.className = stale ? 'muted warn-text' : 'muted';
    el.title = stale
      ? '帧龄偏大：服务端解码管线落后于采集（常见于刚起流、CPU 解码跟不上或源帧率过高）'
      : '显示画面相对当前时间的新鲜度';
  } catch (_) {
    $('#preview-stats').textContent = '';
  }
}

function closePreview() {
  state.previewId = null;
  clearTimeout(state.previewRetry);
  clearInterval(state.statsTimer);
  $('#preview-card').hidden = true;
  $('#preview-img').removeAttribute('src');
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

function init() {
  $('#create-form').addEventListener('submit', createStream);
  $('#btn-refresh').addEventListener('click', () => { loadStreams(); loadHealth(); toast('已刷新', 'ok', 1200); });
  $('#btn-stop-all').addEventListener('click', stopAll);
  $('#btn-snapshot').addEventListener('click', () => state.previewId && snapshot(state.previewId));
  $('#btn-close-preview').addEventListener('click', closePreview);

  loadMeta();
  loadHealth();
  loadStreams();
  bindAutoRefresh();
}

document.addEventListener('DOMContentLoaded', init);

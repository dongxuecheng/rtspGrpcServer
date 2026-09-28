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
    tdId.append(idSpan, button('复制', 'mini', async () => {
      try {
        await navigator.clipboard.writeText(s.stream_id);
        toast('已复制 stream_id', 'ok', 1500);
      } catch (_) {
        toast('复制失败（浏览器限制）', 'err');
      }
    }));

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
  if (!window.confirm(`确定停止 ${streamId} ？`)) return;
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
  if (!window.confirm(`确定停止全部 ${state.streams.length} 路流？`)) return;
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
  const input = window.prompt('输入新的 RTSP URL：', stream.rtsp_url);
  if (!input || input === stream.rtsp_url) return;
  try {
    await api(`/api/streams/${encodeURIComponent(stream.stream_id)}`,
      { method: 'PATCH', body: JSON.stringify({ new_rtsp_url: input.trim() }) });
    toast('URL 已更新，任务将重连', 'ok');
    await loadStreams();
  } catch (e) {
    toast(`更新失败：${e.message}`, 'err');
  }
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
    const age = st.last_ts ? `${Math.max(0, Date.now() - st.last_ts)}ms` : '—';
    $('#preview-stats').textContent =
      `预览 ${st.fps.toFixed(1)} FPS · 已发送 ${st.sent} 帧 · 帧延迟 ${age}`;
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

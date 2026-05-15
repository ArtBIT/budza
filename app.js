import { convertFile, isVideo, isImage } from './media.js';
import { uploadFile } from './upload.js';

// ── State ─────────────────────────────────────────────────────────
const files = [];  // { id, file, status, blob, outName, thumbUrl }
let nextId = 0;

// ── Tab routing ───────────────────────────────────────────────────
document.querySelectorAll('.tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById(`panel-${btn.dataset.tab}`).classList.add('active');
  });
});

// ── Drop zone ─────────────────────────────────────────────────────
const dropZone   = document.getElementById('drop-zone');
const fileInput  = document.getElementById('file-input');
const browseBtn  = document.getElementById('browse-btn');

browseBtn.addEventListener('click', e => { e.stopPropagation(); fileInput.click(); });
dropZone.addEventListener('click', () => fileInput.click());

dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('drag-over'); });
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag-over'));
dropZone.addEventListener('drop', e => {
  e.preventDefault();
  dropZone.classList.remove('drag-over');
  addFiles([...e.dataTransfer.files]);
});

fileInput.addEventListener('change', () => {
  addFiles([...fileInput.files]);
  fileInput.value = '';
});

// ── File management ───────────────────────────────────────────────
function addFiles(incoming) {
  const valid = incoming.filter(f => isVideo(f) || isImage(f));
  if (!valid.length) return;

  valid.forEach(file => {
    const entry = { id: nextId++, file, status: 'pending', blob: null, outName: null, thumbUrl: null };
    files.push(entry);
    if (isImage(file)) generateThumb(entry);
  });

  renderList();
  document.getElementById('action-bar').hidden = false;
}

function generateThumb(entry) {
  const url = URL.createObjectURL(entry.file);
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = c.height = 48;
    const ctx = c.getContext('2d');
    const s = Math.min(img.width, img.height);
    ctx.drawImage(img, (img.width-s)/2, (img.height-s)/2, s, s, 0, 0, 48, 48);
    URL.revokeObjectURL(url);
    entry.thumbUrl = c.toDataURL('image/jpeg', 0.8);
    updateItemThumb(entry);
  };
  img.src = url;
}

// ── Rendering ─────────────────────────────────────────────────────
function renderList() {
  const list = document.getElementById('file-list');
  list.innerHTML = '';
  files.forEach(entry => list.appendChild(makeItem(entry)));
}

function makeItem(entry) {
  const el = document.createElement('div');
  el.className = 'file-item';
  el.dataset.id = entry.id;
  el.innerHTML = `
    <div class="file-thumb" id="thumb-${entry.id}">${isVideo(entry.file) ? '▶' : '🖼'}</div>
    <div class="file-info">
      <div class="file-name">${entry.file.name}</div>
      <div class="file-meta" id="meta-${entry.id}">${fmtSize(entry.file.size)} · ${isVideo(entry.file) ? 'video' : 'image'}</div>
      <div class="file-progress" id="prog-${entry.id}"><div class="file-progress-bar" style="width:0%"></div></div>
    </div>
    <div class="file-actions">
      <span class="badge badge-pending" id="badge-${entry.id}">Pending</span>
      <button class="btn-danger" data-remove="${entry.id}">✕</button>
    </div>`;

  el.querySelector(`[data-remove]`).addEventListener('click', () => removeFile(entry.id));
  if (entry.thumbUrl) applyThumb(el.querySelector(`#thumb-${entry.id}`), entry.thumbUrl);
  return el;
}

function applyThumb(el, url) {
  el.innerHTML = `<img src="${url}" alt="">`;
}

function updateItemThumb(entry) {
  const el = document.getElementById(`thumb-${entry.id}`);
  if (el && entry.thumbUrl) applyThumb(el, entry.thumbUrl);
}

function updateItemStatus(entry, label, cls, metaExtra) {
  const badge = document.getElementById(`badge-${entry.id}`);
  const meta  = document.getElementById(`meta-${entry.id}`);
  const prog  = document.getElementById(`prog-${entry.id}`);
  if (badge) { badge.textContent = label; badge.className = `badge ${cls}`; }
  if (meta && metaExtra) meta.textContent = metaExtra;
  if (prog) prog.classList.toggle('visible', cls === 'badge-working');
}

function setProgress(entry, pct) {
  const bar = document.querySelector(`#prog-${entry.id} .file-progress-bar`);
  if (bar) bar.style.width = pct + '%';
}

function removeFile(id) {
  const idx = files.findIndex(e => e.id === id);
  if (idx === -1) return;
  files.splice(idx, 1);
  document.querySelector(`.file-item[data-id="${id}"]`)?.remove();
  if (!files.length) document.getElementById('action-bar').hidden = true;
}

// ── Upload all ────────────────────────────────────────────────────
const uploadBtn = document.getElementById('upload-btn');
const ffmpegStatus = document.getElementById('ffmpeg-status');

uploadBtn.addEventListener('click', runAll);

async function runAll() {
  uploadBtn.disabled = true;
  const ip = document.getElementById('badge-ip').value.trim();

  for (const entry of files) {
    if (entry.status === 'uploaded') continue;

    // Convert
    entry.status = 'converting';
    updateItemStatus(entry, 'Converting…', 'badge-working');
    if (isVideo(entry.file)) ffmpegStatus.textContent = 'Loading ffmpeg.wasm…';

    try {
      const { blob, filename } = await convertFile(entry.file, ({ progress }) => {
        ffmpegStatus.textContent = `ffmpeg ${Math.round(progress * 100)}%`;
        setProgress(entry, Math.round(progress * 100));
      });
      entry.blob = blob;
      entry.outName = filename;
      ffmpegStatus.textContent = '';
      updateItemStatus(entry, 'Uploading…', 'badge-working',
        `${fmtSize(entry.file.size)} → ${fmtSize(blob.size)} · ${isVideo(entry.file) ? 'video' : 'image'}`);
    } catch (err) {
      entry.status = 'error';
      updateItemStatus(entry, 'Error', 'badge-error', String(err));
      continue;
    }

    // Upload
    const result = await uploadFile(entry.blob, entry.outName, ip);
    if (result.ok) {
      entry.status = 'uploaded';
      updateItemStatus(entry, 'Uploaded', 'badge-uploaded');
    } else {
      entry.status = 'error';
      updateItemStatus(entry, 'Failed', 'badge-error');
      showCurlFallback(entry, result.curl, result.error);
    }
  }

  uploadBtn.disabled = false;
}

function showCurlFallback(entry, curl, reason) {
  const existing = document.getElementById(`curl-${entry.id}`);
  if (existing) return;

  const box = document.createElement('div');
  box.className = 'curl-box';
  box.id = `curl-${entry.id}`;
  box.innerHTML = `<p>Upload failed: ${reason}. Run manually:</p><pre>${curl}</pre>`;

  const item = document.querySelector(`.file-item[data-id="${entry.id}"]`);
  item?.insertAdjacentElement('afterend', box);
}

// ── Utilities ─────────────────────────────────────────────────────
function fmtSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

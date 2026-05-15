/**
 * Convert a File to the format expected by the badge:
 *   Image (JPEG/PNG) → baseline JPEG 240×240
 *   Video (MP4/MOV/AVI) → MJPEG AVI 240×240, no audio
 *
 * Returns { blob: Blob, filename: string }.
 */

const TARGET = 240;

// ── Image pipeline ────────────────────────────────────────────────
export async function convertImage(file) {
  const bitmap = await createImageBitmap(file);
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = TARGET;
  const ctx = canvas.getContext('2d');

  // cover-fill: crop to square, then scale
  const { width: w, height: h } = bitmap;
  const side = Math.min(w, h);
  const sx = (w - side) / 2;
  const sy = (h - side) / 2;
  ctx.drawImage(bitmap, sx, sy, side, side, 0, 0, TARGET, TARGET);
  bitmap.close();

  const blob = await new Promise(res => canvas.toBlob(res, 'image/jpeg', 0.9));
  const outName = file.name.replace(/\.[^.]+$/, '.jpg');
  return { blob, filename: outName };
}

// ── Video pipeline (ffmpeg.wasm) ──────────────────────────────────
let _ffmpeg = null;

async function getFFmpeg(onProgress) {
  if (_ffmpeg) return _ffmpeg;

  // FFmpeg class loaded locally so import.meta.url is same-origin,
  // making the Worker it spawns (./worker.js) same-origin too.
  const { FFmpeg } = await import('./vendor/ffmpeg/index.js');
  const { fetchFile, toBlobURL } = await import(
    'https://unpkg.com/@ffmpeg/util@0.12.1/dist/esm/index.js'
  );

  const baseURL = 'https://unpkg.com/@ffmpeg/core@0.12.6/dist/esm';
  const ff = new FFmpeg();
  if (onProgress) ff.on('progress', onProgress);

  await ff.load({
    coreURL: await toBlobURL(`${baseURL}/ffmpeg-core.js`,   'text/javascript'),
    wasmURL: await toBlobURL(`${baseURL}/ffmpeg-core.wasm`, 'application/wasm'),
  });

  _ffmpeg = ff;
  return ff;
}

export async function convertVideo(file, onProgress) {
  const { fetchFile } = await import(
    'https://unpkg.com/@ffmpeg/util@0.12.1/dist/esm/index.js'
  );
  const ff = await getFFmpeg(onProgress);

  const inName  = 'input' + file.name.slice(file.name.lastIndexOf('.'));
  const outName = file.name.replace(/\.[^.]+$/, '.avi');

  await ff.writeFile(inName, await fetchFile(file));

  await ff.exec([
    '-i', inName,
    '-c:v', 'mjpeg',
    '-q:v', '7',
    '-pix_fmt', 'yuvj420p',
    '-vf', `scale=${TARGET}:${TARGET}:force_original_aspect_ratio=decrease,pad=${TARGET}:${TARGET}:(ow-iw)/2:(oh-ih)/2`,
    '-an',
    outName,
  ]);

  const data = await ff.readFile(outName);
  await ff.deleteFile(inName);
  await ff.deleteFile(outName);

  return { blob: new Blob([data], { type: 'video/avi' }), filename: outName };
}

// ── Dispatch ──────────────────────────────────────────────────────
export function isVideo(file) {
  return /\.(mp4|mov|avi)$/i.test(file.name);
}

export function isImage(file) {
  return /\.(jpe?g|png)$/i.test(file.name);
}

export async function convertFile(file, onProgress) {
  if (isVideo(file)) return convertVideo(file, onProgress);
  if (isImage(file)) return convertImage(file);
  throw new Error(`Unsupported file type: ${file.name}`);
}

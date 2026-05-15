/**
 * Upload a Blob to the badge over WiFi.
 * Returns { ok: true } or { ok: false, curl: string, error: string }.
 *
 * Mixed-content note: GitHub Pages is HTTPS; badge AP is HTTP.
 * Browsers block that fetch. We catch the failure and provide a curl fallback.
 */
export async function uploadFile(blob, filename, ip) {
  const url = `http://${ip}/upload`;
  const fd = new FormData();
  fd.append('file', blob, filename);

  try {
    const res = await fetch(url, { method: 'POST', body: fd });
    if (!res.ok) {
      return { ok: false, error: `HTTP ${res.status}`, curl: makeCurl(ip, filename) };
    }
    return { ok: true };
  } catch (err) {
    const isMixed = err instanceof TypeError &&
      (location.protocol === 'https:' || err.message?.toLowerCase().includes('mixed'));
    return {
      ok: false,
      error: isMixed ? 'Mixed-content blocked (HTTPS→HTTP)' : String(err),
      curl: makeCurl(ip, filename),
    };
  }
}

function makeCurl(ip, filename) {
  return `curl -X POST http://${ip}/upload -F "file=@${filename}"`;
}

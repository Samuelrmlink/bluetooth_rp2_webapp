// The device's filesystem, over its HTTP API (src/net/http.c, /api/fs) --
// and, on a build with an SD card, the card as a top-level "sd" folder: any
// path under sd/ goes to /api/sd/files instead (docs/sd.md), with the same
// {ok, ...} answers, so the editor, the tree, search and upload work on
// either without knowing which.
//
// HTTP stays for every backend (LittleFS, the SD card, curl, the dev
// server). A websocket fs.read / fs.write moves the same bytes one small
// chunk at a time and is slower; the editor lists that path separately.
// The editor, the sidebar and upload/download all go through here for HTTP.
//
// Two rules carried over from the km003c app's api.js:
//
//  * The same ?host=/?port= overrides the WS URL honours, so a page served by
//    dev/devserver.py on one origin still reaches a device on another.
//    Same-origin stays relative, which keeps the normal deployment free of
//    any absolute URL whichever address it was reached by.
//  * "Could not reach the device" is a value, not an exception. Every call
//    resolves to {ok:true, ...} or {ok:false, error, status?, offline?}, and
//    the status rides along so "no such file" (404) can be told from "the
//    device said no" without matching on prose.

const params = new URLSearchParams(location.search);
const HOST = params.get('host') || location.hostname || 'localhost';
const PORT = params.get('port') || location.port || (location.protocol === 'https:' ? 443 : 80);
const PEER = params.get('peer');
const SAME_ORIGIN =
  HOST === location.hostname && String(PORT) === String(location.port || (location.protocol === 'https:' ? 443 : 80));

/* ?peer= names a board found by mDNS. Filesystem calls go through this
 * page's /api/peer/<name>/ proxy. Static paths (an extension script) stay
 * on the page that is hosting the UI. */
function withPeer(path) {
  const p = String(path);
  // fetchBody runs apiUrl again on a URL tarUrl and removePostUrl already
  // prefixed, so a second pass must leave /api/peer/ alone.
  if (!PEER || !p.startsWith('/api/') || p.startsWith('/api/peer/')) return path;
  return `/api/peer/${encodeURIComponent(PEER)}${p}`;
}

// The board proxies one websocket and one HTTP request at a time. The editor
// asks for the file tree and for js/ext together, so peer calls take turns
// and a "proxy busy" is tried once more.
let peerTail = Promise.resolve();
function throughPeer(run) {
  if (!PEER) return run();
  const job = peerTail.then(async () => {
    const out = await run();
    if (out && out.error === 'proxy busy') {
      await new Promise((resolve) => setTimeout(resolve, 200));
      return run();
    }
    return out;
  });
  peerTail = job.then(() => {}, () => {});
  return job;
}

export function apiUrl(path) {
  const p = withPeer(path);
  return SAME_ORIGIN ? p : `${location.protocol}//${HOST}:${PORT}${p}`;
}

// The device serves at most 1 MiB per file (FS_FILE_MAX in src/fs/fs.h) and
// refuses a larger PUT with 413. Checked here too, so the editor can say so
// before sending rather than after.
export const FILE_MAX = 1024 * 1024;

// --- the SD card's place in the tree -------------------------------------
// Off until the app has seen "sd" in sys.info's modules (setSd), so a build
// without a card shows no sd folder at all.
export const SD_ROOT = 'sd';
let sdOn = false;
export function setSd(on) { sdOn = !!on; }
export function sdAvailable() { return sdOn; }
export function isSd(path) {
  const p = String(path || '');
  return sdOn && (p === SD_ROOT || p.startsWith(SD_ROOT + '/'));
}
const sdRel = (path) => String(path).slice(SD_ROOT.length + 1);   // 'sd/a/b' -> 'a/b', 'sd' -> ''
// The largest file each side takes: LittleFS's 1 MiB; FAT32's 4 GiB - 1.
export const SD_FILE_MAX = 4 * 1024 * 1024 * 1024 - 1;
export const fileMax = (path) => (isSd(path) ? SD_FILE_MAX : FILE_MAX);

const enc = (path) => String(path).split('/').map(encodeURIComponent).join('/');

// Encode each path segment, never the separators: the device matches on the
// literal path, and an encoded '/' would name a different file.
function fsPath(path) {
  if (isSd(path)) {
    const rel = sdRel(path);
    return rel ? '/api/sd/files/' + enc(rel) : '/api/sd/files';
  }
  return '/api/fs/' + enc(path);
}

async function send(method, url, body, as) {
  return throughPeer(() => sendNow(method, url, body, as));
}

async function sendNow(method, url, body, as) {
  let res;
  try {
    res = await fetch(apiUrl(url), { method, cache: 'no-store', body });
  } catch {
    return { ok: false, error: 'unreachable', offline: true };
  }
  if (as === 'text' && res.ok) return { ok: true, text: await res.text() };
  if (as === 'blob' && res.ok) return { ok: true, blob: await res.blob() };
  let data = null;
  try {
    data = await res.json();
  } catch {
    return res.ok ? { ok: true } : { ok: false, error: `HTTP ${res.status}`, status: res.status };
  }
  if (data && data.ok === false) return { ...data, status: res.status };
  if (!res.ok) return { ok: false, error: data?.error || `HTTP ${res.status}`, status: res.status };
  return { ok: true, ...data };
}

// One level, as the device lists it: [{name, dir:true} | {name, size, alloc}],
// alloc being the flash a file takes (fw 1.94+; 0 = inline in its folder).
// Directories first, then files, each alphabetical -- the order a file tree
// is read in.
export async function list(dir = '') {
  const sd = isSd(dir);
  const url = sd ? fsPath(dir) : dir ? fsPath(dir) + '/' : '/api/fs/';
  const r = await send('GET', url, undefined, 'json');
  if (!r.ok) return r;
  let files = (r.files || []).slice();
  // The card hangs off the root, in place of any flash folder of that name.
  if (!dir && sdOn) files = files.filter((f) => f.name !== SD_ROOT).concat([{ name: SD_ROOT, dir: true, sd: true }]);
  files.sort((a, b) => (b.dir ? 1 : 0) - (a.dir ? 1 : 0) || a.name.localeCompare(b.name));
  return { ok: true, files, block: sd ? undefined : r.block, sd };
}

// A file's URL (for a link or a drag out of the page).
export const fileUrl = (path) => apiUrl(fsPath(path));

export const readText = (path) => send('GET', fsPath(path), undefined, 'text');
export const readBlob = (path) => send('GET', fsPath(path), undefined, 'blob');

// A body read as it arrives, so a long open or download can say how far it
// has got. `got` is capped at Content-Length when the browser has already
// unpacked a gzip (the length is the stored size, the bytes are the text).
//
// Chunks already sitting in the browser resume as microtasks, so a click
// never runs until the whole body has been pulled. A macrotask at least
// every 32 ms (and once more before the bytes are kept) lets Abort land
// while the job is still running. Yielding on every chunk would add the
// browser's timer clamp to each packet of a large file.
const READ_YIELD_MS = 32;
const yieldToInput = () => new Promise((resolve) => setTimeout(resolve, 0));
const abortedBody = () => ({ ok: false, aborted: true, error: 'aborted' });

export async function fetchBody(url, opts) {
  return throughPeer(() => fetchBodyNow(url, opts));
}

async function fetchBodyNow(url, { method = 'GET', body = undefined, signal, onProgress } = {}) {
  let res;
  const target = /^https?:\/\//i.test(url) ? url : apiUrl(url);
  try {
    res = await fetch(target, {
      method,
      cache: 'no-store',
      body,
      signal,
      headers: body != null ? { 'Content-Type': 'text/plain' } : undefined,
    });
  } catch (e) {
    if (e?.name === 'AbortError') return abortedBody();
    return { ok: false, error: 'unreachable', offline: true };
  }
  if (!res.ok) {
    let error = `HTTP ${res.status}`;
    try {
      const j = await res.json();
      if (typeof j?.error === 'string') error = j.error;
      else if (j?.error?.message) error = j.error.message;
    } catch { /* a tar or a file, not JSON */ }
    return { ok: false, error, status: res.status };
  }
  const total = Number(res.headers.get('Content-Length')) || 0;
  const reader = res.body?.getReader?.();
  const chunks = [];
  let raw = 0;
  let yieldedAt = performance.now();
  const report = () => onProgress?.({ got: total && raw > total ? total : raw, total, raw });
  const stop = async () => {
    try { await reader?.cancel(); } catch { /* already closed */ }
    return abortedBody();
  };
  try {
    if (signal?.aborted) return stop();
    if (!reader) {
      const buf = new Uint8Array(await res.arrayBuffer());
      if (signal?.aborted) return abortedBody();
      raw = buf.length;
      chunks.push(buf);
      report();
    } else {
      for (;;) {
        if (signal?.aborted) return stop();
        const step = await reader.read();
        if (signal?.aborted) return stop();
        if (step.done) break;
        chunks.push(step.value);
        raw += step.value.length;
        report();
        if (signal?.aborted) return stop();
        if (performance.now() - yieldedAt >= READ_YIELD_MS) {
          await yieldToInput();
          yieldedAt = performance.now();
          if (signal?.aborted) return stop();
        }
      }
    }
  } catch (e) {
    try { await reader?.cancel(); } catch { /* already closed */ }
    if (e?.name === 'AbortError' || signal?.aborted) return abortedBody();
    return { ok: false, error: e?.message || 'read failed' };
  }
  await yieldToInput();
  if (signal?.aborted) return stop();
  const bytes = new Uint8Array(chunks.reduce((n, c) => n + c.length, 0));
  let o = 0;
  for (const c of chunks) { bytes.set(c, o); o += c.length; }
  await yieldToInput();
  if (signal?.aborted) return abortedBody();
  return { ok: true, bytes, total, size: bytes.length };
}

export const readFileStream = (path, opts) => fetchBody(fsPath(path), opts);
export async function readTextStream(path, opts) {
  const r = await readFileStream(path, opts);
  if (!r.ok) return r;
  if (opts?.signal?.aborted) return abortedBody();
  // The copy above is synchronous. One more turn so a click that landed
  // during it is applied before the text is accepted.
  await yieldToInput();
  if (opts?.signal?.aborted) return abortedBody();
  return { ...r, text: new TextDecoder().decode(r.bytes) };
}

export function write(path, data) {
  const size = typeof data === 'string' ? new TextEncoder().encode(data).length : data.size;
  if (!isSd(path) && size > FILE_MAX) {
    return Promise.resolve({ ok: false, error: `larger than the device's ${FILE_MAX / 1024} KB file limit`, status: 413 });
  }
  if (isSd(path) && !sdRel(path)) return Promise.resolve({ ok: false, error: 'that is the card itself', status: 400 });
  return send('PUT', fsPath(path), data, 'json');
}

export const remove = (path) => send('DELETE', fsPath(path), undefined, 'json');

// A folder, parents and all (fw 1.95: PUT with a trailing slash). Files need
// none -- a PUT makes its parents -- so this is for empty folders.
export const mkdir = (path) => send('PUT', fsPath(path) + '/', '', 'json');

// A folder ('' = everything) as a tar archive, streamed by the device
// (fw 1.95). The editor fetches it so the download can be watched and
// aborted; a drag out of the browser still uses the URL. id= names fs.job
// notes on a newer image. The board has no clock, so the file times are
// the time asked for.
export function tarUrl(folder = '', id = 0) {
  if (isSd(folder)) return null;           // the device streams tar from flash only
  const p = folder ? '/api/tar/' + String(folder).split('/').map(encodeURIComponent).join('/') : '/api/tar/';
  const idq = id ? `&id=${id}` : '';
  return apiUrl(`${p}?mtime=${Math.floor(Date.now() / 1000)}${idq}`);
}

// POST /api/tar {"paths":[...],"mtime":N,"id":N}. Older firmware answers 404.
export const tarPostUrl = () => apiUrl('/api/tar');
export const removePostUrl = () => apiUrl('/api/fs/remove');

// The device takes a JSON body here, as text/plain so a cross-origin dev page
// does not provoke a CORS preflight it would not answer.
export function rename(from, to) {
  if (isSd(from) !== isSd(to)) {
    return Promise.resolve({ ok: false, error: 'moving between the SD card and flash is not supported (download, then upload)', status: 400 });
  }
  if (isSd(from)) {
    if (!sdRel(from) || !sdRel(to)) return Promise.resolve({ ok: false, error: 'the card itself cannot be renamed', status: 400 });
    return send('POST', '/api/sd/rename', JSON.stringify({ from: sdRel(from), to: sdRel(to) }), 'json');
  }
  return send('POST', '/api/fs/rename', JSON.stringify({ from, to }), 'json');
}

// GET /api/sd: the card's status (docs/sd.md).
export const sdStatus = () => send('GET', '/api/sd', undefined, 'json');
export const sdFormat = (fs, cluster = 0) => send('POST', '/api/sd/format', JSON.stringify({ fs, cluster }), 'json');
export const sdBench = (mb = 10) => send('POST', '/api/sd/bench', JSON.stringify({ mb }), 'json');

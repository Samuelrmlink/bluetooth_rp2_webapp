// The device's filesystem, over its HTTP API (src/net/http.c, /api/fs).
//
// HTTP rather than the WebSocket because files are the one thing too big for
// it: a WS frame caps at 4 KB and an op reply at less, while a file may be
// 128 KB. The editor, the sidebar and upload/download all go through here.
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
const SAME_ORIGIN =
  HOST === location.hostname && String(PORT) === String(location.port || (location.protocol === 'https:' ? 443 : 80));

export function apiUrl(path) {
  return SAME_ORIGIN ? path : `${location.protocol}//${HOST}:${PORT}${path}`;
}

// The device serves at most 128 KB per file (FS_FILE_MAX in src/fs/fs.h) and
// refuses a larger PUT with 413. Checked here too, so the editor can say so
// before sending rather than after.
export const FILE_MAX = 128 * 1024;

// Encode each path segment, never the separators: the device matches on the
// literal path, and an encoded '/' would name a different file.
function fsPath(path) {
  return '/api/fs/' + String(path).split('/').map(encodeURIComponent).join('/');
}

async function send(method, url, body, as) {
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
  const r = await send('GET', dir ? fsPath(dir) + '/' : '/api/fs/', undefined, 'json');
  if (!r.ok) return r;
  const files = (r.files || []).slice().sort((a, b) =>
    (b.dir ? 1 : 0) - (a.dir ? 1 : 0) || a.name.localeCompare(b.name));
  return { ok: true, files, block: r.block };
}

export const readText = (path) => send('GET', fsPath(path), undefined, 'text');
export const readBlob = (path) => send('GET', fsPath(path), undefined, 'blob');

export function write(path, data) {
  const size = typeof data === 'string' ? new TextEncoder().encode(data).length : data.size;
  if (size > FILE_MAX) {
    return Promise.resolve({ ok: false, error: `larger than the device's ${FILE_MAX / 1024} KB file limit`, status: 413 });
  }
  return send('PUT', fsPath(path), data, 'json');
}

export const remove = (path) => send('DELETE', fsPath(path), undefined, 'json');

// A folder, parents and all (fw 1.95: PUT with a trailing slash). Files need
// none -- a PUT makes its parents -- so this is for empty folders.
export const mkdir = (path) => send('PUT', fsPath(path) + '/', '', 'json');

// A folder ('' = everything) as a tar archive, streamed by the device
// (fw 1.95). A URL rather than a fetch: the browser saves it straight to
// disk, with its progress, and nothing is held in the page. The board has no
// clock, so the archive's file times are the time asked for.
export function tarUrl(folder = '') {
  const p = folder ? '/api/tar/' + String(folder).split('/').map(encodeURIComponent).join('/') : '/api/tar/';
  return apiUrl(`${p}?mtime=${Math.floor(Date.now() / 1000)}`);
}

// The device takes a JSON body here, as text/plain so a cross-origin dev page
// does not provoke a CORS preflight it would not answer.
export function rename(from, to) {
  return send('POST', '/api/fs/rename', JSON.stringify({ from, to }), 'json');
}

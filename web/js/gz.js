// Compressed files on the device: a name ending in .gz holds gzip bytes, as
// the web assets always have and Lua sources may (fw 1.95: lua/x.lua.gz runs
// like lua/x.lua). The board never compresses -- there is no deflate on it --
// so the browser does, with the native CompressionStream. Reading needs
// nothing: the device answers GET /api/fs/<x>.gz with Content-Encoding: gzip,
// so fetch hands back the text already.

export const isGz = (path) => /\.gz$/i.test(path);
export const plainName = (path) => String(path).replace(/\.gz$/i, '');

// Pictures the editor shows rather than edits. SVG is text as well, so it
// gets a Preview / Text toggle.
export function imageKind(path) {
  const m = /\.(png|jpe?g|gif|webp|ico|svg)$/i.exec(plainName(path));
  if (!m) return null;
  return m[1].toLowerCase() === 'svg' ? 'svg' : 'bitmap';
}

export const IMAGE_TYPES = { png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
                             webp: 'image/webp', ico: 'image/x-icon', svg: 'image/svg+xml' };
export function imageType(path) {
  const ext = plainName(path).split('.').pop().toLowerCase();
  return IMAGE_TYPES[ext] || 'application/octet-stream';
}

// The folders that are the gateway's own state, not the web UI's files.
const DEVICE_DIRS = /^(lua|staged|cfg|var|cache|ota)\//;

// A file of the web UI itself (js/, css/, index.html, a webapp's folder...).
// Saving one keeps the previous version as <file>.bak: a broken page is the
// one mistake that also takes away the editor to fix it with.
export function isWebAsset(path) {
  if (/^sd(\/|$)/.test(path)) return false;      // the SD card is never served as the UI
  return !DEVICE_DIRS.test(path) && /\.(js|mjs|css|html?|svg|json|webmanifest)$/i.test(plainName(path));
}

// May this file be stored compressed? The board interprets a gzipped file
// only for the web UI (served as static bytes) and for a top-level Lua
// script (lua/x.lua.gz runs as lua/x.lua). Anything else — a PEX image, a
// nested module, a note, a C source — would be unreadable once gzipped, so
// it stays plain. cfg/ is read at boot and is too small to gain (the 1.95
// notes); staged/ is what the editor is writing and the firmware reads it
// plain. A .gz of a type refused here can still be stored uncompressed:
// the file menu offers that for any gzip that is not a bitmap.
export function canCompress(path) {
  if (/^sd(\/|$)/.test(path)) return false;      // the card serves nothing: no point
  if (isGz(path) || /^(staged|cfg|var|cache|ota)\//.test(path) || /\.bak$/i.test(path)) return false;
  if (/\.(png|jpe?g|gif|webp|gz|zip|bin|luac|pex)$/i.test(plainName(path))) return false;
  if (isWebAsset(path)) return true;
  return /^lua\/[^/]+\.lua$/i.test(plainName(path));
}

// The plain/compressed twin a file would be shadowed by, given its folder's
// names: lua/x.lua runs over lua/x.lua.gz (plain wins when both exist).
export function shadowInfo(path, siblings) {
  const name = path.split('/').pop();
  if (isGz(name) && siblings.includes(plainName(name))) return { shadowed: true, by: plainName(name) };
  if (!isGz(name) && siblings.includes(`${name}.gz`)) return { shadowed: false, by: `${name}.gz` };
  return null;
}

// text -> a Blob of gzip bytes.
export async function gzipText(text) {
  const stream = new Blob([text]).stream().pipeThrough(new CompressionStream('gzip'));
  return new Response(stream).blob();
}

// gzip bytes -> text (for anything that holds the stored bytes, such as a
// file taken out of a tar archive).
export async function gunzipText(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Response(stream).text();
}

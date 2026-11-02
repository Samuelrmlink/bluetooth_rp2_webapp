/* Chunked fs.read / fs.write on the board websocket. One chunk is in flight.
 * `busy` retries the same offset (the link was kept for telemetry). HTTP
 * transfers stay in fs.js. `node`, when set, is the mDNS name of the other
 * board and is the first field the socket's board looks at. Omit it when
 * this socket is already that board. */

import { FILE_RAW, b64encode, b64decode } from './link.js';

const BUSY_CAP = 200;

const yieldTurn = () => new Promise((resolve) => setTimeout(resolve, 0));

function concat(parts) {
  const n = parts.reduce((s, p) => s + p.length, 0);
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

async function callOp(call, op, params, node) {
  return node ? call(op, params, node) : call(op, params);
}

export async function readChunks(call, path, { signal, onProgress, node, raw = FILE_RAW, busyCap = BUSY_CAP } = {}) {
  let offset = 0;
  let total = 0;
  let busy = 0;
  const parts = [];
  for (;;) {
    if (signal?.aborted) return { ok: false, aborted: true, bytes: concat(parts), got: offset };
    let result;
    try {
      result = await callOp(call, 'fs.read', { path, offset, len: raw }, node);
      busy = 0;
    } catch (e) {
      if (e?.code === 'busy') {
        if (++busy > busyCap) return { ok: false, error: e.message || 'busy', code: 'busy', bytes: concat(parts), got: offset };
        await yieldTurn();
        continue;
      }
      return { ok: false, error: e?.message || 'read failed', code: e?.code, bytes: concat(parts), got: offset };
    }
    const chunk = result?.data ? b64decode(result.data) : new Uint8Array(0);
    if (chunk.length) parts.push(chunk);
    offset += chunk.length;
    total = Number(result?.size ?? offset);
    onProgress?.({ got: offset, total });
    const eof = result?.eof === true || chunk.length === 0;
    if (eof) break;
    await yieldTurn();
  }
  return { ok: true, bytes: concat(parts), size: total, got: offset };
}

export async function writeChunks(call, path, bytes, { signal, onProgress, node, raw = FILE_RAW, busyCap = BUSY_CAP } = {}) {
  const src = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let offset = 0;
  let busy = 0;
  const total = src.length;
  while (offset < total || (offset === 0 && total === 0)) {
    if (signal?.aborted) return { ok: false, aborted: true, got: offset, total };
    const n = Math.min(raw, total - offset);
    const slice = src.subarray(offset, offset + n);
    try {
      await callOp(call, 'fs.write', {
        path,
        offset,
        data: b64encode(slice),
        truncate: offset === 0,
      }, node);
      busy = 0;
    } catch (e) {
      if (e?.code === 'busy') {
        if (++busy > busyCap) return { ok: false, error: e.message || 'busy', code: 'busy', got: offset, total };
        await yieldTurn();
        continue;
      }
      return { ok: false, error: e?.message || 'write failed', code: e?.code, got: offset, total };
    }
    offset += n;
    onProgress?.({ got: offset, total });
    if (total === 0) break;
    await yieldTurn();
  }
  return { ok: true, got: offset, total };
}

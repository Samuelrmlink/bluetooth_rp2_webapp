/* The same send-buffer rule as src/net/ws_link.h. A file frame is allowed
 * only when what remains after it still covers the telemetry reserve.
 * pumpLink is the simulator the unit test runs; the firmware is the real
 * sender. Do not export a name the editor already uses for archive copies. */

export const FILE_RAW = 480;
export const FILE_FRAME = 900;
export const RESERVE_MAX = 4096;
export const LINK_SND_BUF = 5840;

export function allowBulk(sndBuf, sndAvail, frameLen) {
  if (!frameLen || !sndBuf) return false;
  let reserve = sndBuf > 1024 ? Math.floor((sndBuf * 2) / 3) : Math.floor(sndBuf / 2);
  if (reserve > RESERVE_MAX) reserve = RESERVE_MAX;
  if (reserve >= sndBuf) reserve = Math.floor(sndBuf / 2);
  if (sndAvail < frameLen) return false;
  return (sndAvail - frameLen) >= reserve;
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

export function b64encode(bytes) {
  const src = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let out = '';
  for (let i = 0; i < src.length; i += 3) {
    const b0 = src[i];
    const b1 = i + 1 < src.length ? src[i + 1] : 0;
    const b2 = i + 2 < src.length ? src[i + 2] : 0;
    const v = (b0 << 16) | (b1 << 8) | b2;
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += i + 1 < src.length ? B64[(v >> 6) & 63] : '=';
    out += i + 2 < src.length ? B64[v & 63] : '=';
  }
  return out;
}

export function b64decode(text) {
  const s = String(text || '');
  if (s.length % 4) throw Object.assign(new Error('bad base64'), { code: 'bad_request' });
  const out = [];
  for (let i = 0; i < s.length; i += 4) {
    const c = [0, 1, 2, 3].map((k) => s[i + k]);
    let pad = 0;
    const v = c.map((ch, k) => {
      if (ch === '=') {
        if (k < 2) throw Object.assign(new Error('bad base64'), { code: 'bad_request' });
        pad++;
        return 0;
      }
      const n = B64.indexOf(ch);
      if (n < 0 || pad) throw Object.assign(new Error('bad base64'), { code: 'bad_request' });
      return n;
    });
    out.push((v[0] << 2) | (v[1] >> 4));
    if (pad < 2) out.push(((v[1] << 4) | (v[2] >> 2)) & 255);
    if (pad < 1) out.push(((v[2] << 6) | v[3]) & 255);
  }
  return Uint8Array.from(out);
}

function drain(avail, sndBuf, mss) {
  const used = sndBuf - avail;
  return avail + Math.min(mss, used);
}

/* Fair: telemetry first, then at most one file frame if the gate allows.
 * Greedy: fill the buffer with file frames, then telemetry. That is the
 * sender the gate exists to reject. */
function turn(link, greedy) {
  if (greedy) {
    while (link.left > 0 && link.avail >= link.frameLen) {
      const n = Math.min(link.raw, link.left);
      link.avail -= link.frameLen;
      link.left -= n;
      link.rawSent += n;
      link.fileFrames++;
    }
    if (link.avail >= link.telemetrySize) {
      link.avail -= link.telemetrySize;
      link.telSent++;
    } else {
      link.telDrop++;
    }
  } else {
    let telOut = false;
    if (link.avail >= link.telemetrySize) {
      link.avail -= link.telemetrySize;
      link.telSent++;
      telOut = true;
    } else {
      link.telDrop++;
    }
    if (link.left > 0 && allowBulk(link.sndBuf, link.avail, link.frameLen)) {
      if (!telOut) link.late++;
      const n = Math.min(link.raw, link.left);
      link.avail -= link.frameLen;
      link.left -= n;
      link.rawSent += n;
      link.fileFrames++;
    }
  }
  link.avail = drain(link.avail, link.sndBuf, link.mss);
}

/* Logical file of `size` bytes (default 2 MiB). Nothing that large is
 * allocated: each frame carries `raw` bytes of credit. */
export function pumpLink({
  size = 2 * 1024 * 1024,
  telemetrySize = 64,
  mss = 1460,
  sndBuf = LINK_SND_BUF,
  frameLen = FILE_FRAME,
  raw = FILE_RAW,
  stopAt = 0,
  greedy = false,
  signal,
} = {}) {
  const link = {
    avail: sndBuf,
    sndBuf,
    telemetrySize,
    mss,
    frameLen,
    raw,
    left: size,
    telSent: 0,
    telDrop: 0,
    rawSent: 0,
    fileFrames: 0,
    late: 0,
  };
  let guard = 0;
  while (link.left > 0 && guard < 2000000) {
    if (signal?.aborted) break;
    if (stopAt && link.rawSent >= stopAt) break;
    turn(link, greedy);
    guard++;
  }
  return {
    telDrop: link.telDrop,
    telSent: link.telSent,
    rawSent: link.rawSent,
    left: link.left,
    late: link.late,
    fileFrames: link.fileFrames,
    aborted: Boolean(signal?.aborted) || (stopAt > 0 && link.left > 0),
  };
}

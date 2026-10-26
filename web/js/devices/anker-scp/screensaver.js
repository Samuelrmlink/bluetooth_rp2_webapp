// A2687 screensaver upload -- the browser's half, DOM-free and tested.
//
// The gateway's driver only frames one step at a time and reports what the
// charger answered (the anker.screensaver op). Everything else is
// here: the CRC32, the image id, cutting the JPEG into packets, the
// window-of-10 loop, resuming from what the charger says it has, retries.
//
// The exchange (decrypted from the app's own upload, every frame verified):
//   select  id, crc            -> status 0x11: not cached, send it; 0: has it
//   begin   id, crc, size, count, packet length, window   -> status 0
//   data    packet n (0-based), padded to the packet length
//           -> after each window and the last packet: status 0 + packets
//              received so far, or status 0x10 when the image is complete

export const STATUS_NOT_CACHED = 0x11;
export const STATUS_COMPLETE = 0x10;
export const WINDOW = 10;
export const MAX_PACKET = 156;       // what the app uses
export const SIDE = 240;             // the charger's screen, in pixels

let table;
export function crc32(bytes) {
  if (!table) {
    table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = table[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// The packet length a link can carry: one data frame must fit one ATT write.
// Frame = 10 B header/checksum + 11 B of TLV around the data + 16 B GCM tag,
// and an ATT write carries MTU - 3.
export function packetLength(mtu) {
  return Math.max(0, Math.min(MAX_PACKET, (mtu || 23) - 3 - 37));
}

// The JPEG cut into packets of `plen`, the last one zero-padded as the app does.
export function packets(bytes, plen) {
  const out = [];
  for (let off = 0; off < bytes.length; off += plen) {
    const p = new Uint8Array(plen);
    p.set(bytes.subarray(off, off + plen));
    out.push(p);
  }
  return out;
}

// The charger keys its cache by id (the app uses ids from Anker's cloud, e.g.
// 44718, 47984). Derived from the CRC so the same picture gets the same id
// and a second upload of it is a cache hit; editable in the window.
export function defaultId(crc) {
  return 100000 + (crc % 900000);
}

const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');

export class UploadError extends Error {}

/**
 * Run one upload. io:
 *   call(params)            -> the anker.screensaver op (device already filled in)
 *   wait(match, ms)         -> resolves with the next profile.anker.screensaver
 *                              event for which match(ev) is true, rejects on timeout
 * Returns 'cached' when the charger already had the image, else 'sent'.
 */
export async function upload({ jpeg, id, mode = 3, plen = MAX_PACKET, window = WINDOW },
                             io, { onProgress = () => {}, signal } = {}) {
  const crc = crc32(jpeg);
  const pk = packets(jpeg, plen);
  const count = pk.length;
  const stop = () => { if (signal?.aborted) throw new UploadError('cancelled'); };

  // Each request is armed BEFORE it is sent: the reply event can beat the
  // op's own answer back to the browser.
  async function step(params, match, ms) {
    stop();
    const got = io.wait(match, ms);
    for (let tries = 0; ; tries++) {
      const r = await io.call(params);
      if (!r?.error) break;
      if (r.error !== 'busy' || tries > 50) throw new UploadError(`${params.step}: ${r.message || r.error}`);
      await new Promise((res) => setTimeout(res, 100));
    }
    const ev = await got;
    if (ev.error) throw new UploadError(`${params.step}: ${ev.error}`);
    return ev;
  }

  onProgress({ phase: 'select', sent: 0, count });
  const s = await step({ step: 'select', id, crc, mode }, (e) => e.step === 'select', 8000);
  if (s.status === 0) {
    onProgress({ phase: 'done', sent: count, count, cached: true });
    return 'cached';
  }
  if (s.status !== STATUS_NOT_CACHED) throw new UploadError(`select: charger answered status ${s.status}`);

  const b = await step({ step: 'begin', id, crc, size: jpeg.length, count, plen, window },
                       (e) => e.step === 'begin', 8000);
  if (b.status !== 0) throw new UploadError(`begin: charger answered status ${b.status}`);

  let next = 0;
  let retries = 0;
  while (next < count) {
    const end = Math.min(next + window, count);
    let ev;
    try {
      for (let n = next; n < end; n++) {
        stop();
        const last = n === end - 1;
        const params = { step: 'data', n, a: hex(pk[n].subarray(0, plen >> 1)),
                         b: hex(pk[n].subarray(plen >> 1)), ack: last };
        if (last) {
          ev = await step(params, (e) => e.step === 'data' && e.n === n, 12000);
        } else {
          const r = await io.call(params);
          if (r?.error) throw new UploadError(`data ${n}: ${r.message || r.error}`);
        }
      }
    } catch (e) {
      if (e instanceof UploadError && e.message === 'cancelled') throw e;
      if (++retries > 3) throw e;
      onProgress({ phase: 'retry', sent: next, count, why: e.message });
      continue;                               // resend this window
    }
    if (ev.status === STATUS_COMPLETE) { next = count; break; }
    if (ev.status !== 0) throw new UploadError(`data: charger answered status ${ev.status}`);
    // The charger says how many it has: carry on from there, which also
    // resends anything it missed.
    next = Number.isInteger(ev.received) ? ev.received : end;
    retries = 0;
    onProgress({ phase: 'data', sent: next, count });
  }
  onProgress({ phase: 'done', sent: count, count });
  return 'sent';
}

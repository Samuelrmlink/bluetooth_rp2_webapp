// Copy and move in the editor sidebar, including between the SD card and
// LittleFS. The firmware rename refuses a path that changes volume, so a
// cross-volume move is a copy and then a delete of the source. Same-volume
// move stays a rename: the bytes do not move, so it needs no free space.
//
// Capacity is checked before any write. LittleFS free comes from Lua
// file.media (there is no HTTP field). SD free comes from GET /api/sd.
// The card's mounted cluster size is not in that JSON. Each SD file and
// directory is counted as a multiple of 4 KB, the smallest legal cluster.
// A larger cluster can still fail the write; that error is reported and
// the source is left in place.
//
// A failed PUT deletes the destination file. This module never writes in
// order to probe free space.

import { badPath, FILE_MAX } from '../tar.js';

export const BLOCK = 4096;
export const SD_FILE_MAX = 4 * 1024 * 1024 * 1024 - 1;

const LFS_FREE_CHUNK = 'local m = file.media() return string.format("%d %d", m.lfs.free or 0, m.lfs.ok and 1 or 0)';

/** The volume a tree path lives on. 'sd' and 'sd/...' are the card. */
export function volume(path) {
  const p = String(path || '');
  return p === 'sd' || p.startsWith('sd/') ? 'sd' : 'lfs';
}

/**
 * Bytes to reserve for a new file. LittleFS alloc, when the source already
 * has it, is exact. A new or inline file (alloc 0) is rounded up to a block
 * so a small file is not assumed to fit in directory metadata. An empty
 * file takes nothing. On the card, round up to the 4 KB cluster floor.
 */
export function storedSize(file, vol, block = BLOCK) {
  const size = file?.size || 0;
  if (vol === 'sd') {
    if (size <= 0) return 0;
    return Math.ceil(size / BLOCK) * BLOCK;
  }
  const alloc = file?.alloc || 0;
  if (alloc > 0) return alloc;
  const b = block || BLOCK;
  if (size <= 0) return 0;
  return Math.ceil(size / b) * b;
}

/**
 * Bytes an existing file gives back when a PUT replaces it. An inline
 * LittleFS file (alloc 0) occupies no block of its own, so replacing it
 * returns nothing. Missing alloc is treated the same way.
 */
export function reclaimSize(file, vol) {
  if (!file) return 0;
  if (vol === 'sd') return storedSize(file, 'sd');
  return file.alloc > 0 ? file.alloc : 0;
}

/**
 * Extra bytes the destination must have. A same-volume move is 0.
 * dirCount includes the folder being copied. LittleFS counts two blocks
 * of directory metadata per folder. The card counts one cluster each.
 */
export function transferNeed({ move, srcVol, destVol, files, dirCount = 0, block = BLOCK, reclaim = 0 }) {
  if (move && srcVol === destVol) return 0;
  let need = 0;
  const b = block || BLOCK;
  if (destVol === 'lfs') {
    need += dirCount * 2 * b;
    for (const f of files || []) need += storedSize(f, 'lfs', b);
  } else {
    need += dirCount * BLOCK;
    for (const f of files || []) need += storedSize(f, 'sd');
  }
  return Math.max(0, need - (reclaim || 0));
}

/** need <= 0 fits without a reading. A missing free value does not fit. */
export function fits(free, need) {
  if (!(need > 0)) return { ok: true };
  if (free == null || !Number.isFinite(free)) return { ok: false, code: 'free' };
  if (free < need) return { ok: false, code: 'space', free, need };
  return { ok: true };
}

/** "bytes ok" from script.eval of file.media. null when the read failed. */
export function parseLfsFree(value) {
  const parts = String(value ?? '').trim().split(/\s+/);
  const free = Number(parts[0]);
  if (parts[1] !== '1' || !Number.isFinite(free) || free < 0) return null;
  return free;
}

/** Free bytes from GET /api/sd, or null unless state is ok. */
export function sdFreeBytes(status) {
  if (!status?.ok || status.state !== 'ok') return null;
  const free = Number(status.free);
  if (!Number.isFinite(free) || free < 0) return null;
  return free;
}

export const lfsFreeChunk = () => LFS_FREE_CHUNK;

/**
 * One file, or a folder and everything under it. list(dir) -> { ok, files, block }.
 * dirs is deepest first, the folder itself last, matching treeOf.
 */
export async function measure(list, path, node) {
  if (!node?.dir) {
    return {
      files: [{ path, size: node?.size || 0, alloc: node?.alloc || 0 }],
      dirs: [],
      block: BLOCK,
    };
  }
  const files = [];
  const dirs = [];
  let block = BLOCK;
  const walk = async (p) => {
    const r = await list(p);
    if (!r?.ok) throw new Error(r?.error || `could not list ${p || '/'}`);
    if (r.block) block = r.block;
    for (const f of r.files || []) {
      const child = p ? `${p}/${f.name}` : f.name;
      if (f.dir) await walk(child);
      else files.push({ path: child, size: f.size || 0, alloc: f.alloc || 0 });
    }
    dirs.push(p);
  };
  await walk(path);
  return { files, dirs, block };
}

function mapTo(src, dest, path) {
  if (path === src) return dest;
  return dest + path.slice(src.length);
}

/**
 * Decide a copy or move before any write.
 * measured: { files, dirs, block } from measure().
 * destHit: the existing destination, or null. Folders are not merged.
 * free: destination free bytes. Pass null to be told when a read is required
 * (code 'free') instead of failing closed.
 */
export function planTransfer({ src, dest, move = false, dir = false, measured, destHit = null, free = null }) {
  const verb = move ? 'move' : 'copy';
  if (!src || src === 'sd') {
    return { ok: false, code: 'card', error: 'The card itself cannot be copied or moved.' };
  }
  if (!dest || dest === 'sd') {
    return { ok: false, code: 'card', error: 'That names the card itself. Pick a path under sd/.' };
  }
  if (dest === src) return { ok: false, code: 'same', error: 'That is the same path.' };
  if (dest.startsWith(`${src}/`)) {
    return {
      ok: false,
      code: 'inside',
      error: move ? 'That would move a folder into itself.' : 'That would copy a folder into itself.',
    };
  }
  const why = badPath(dest);
  if (why) return { ok: false, code: 'path', error: `Cannot ${verb} there: ${why}` };

  const srcVol = volume(src);
  const destVol = volume(dest);
  if (destHit?.dir || (dir && destHit)) {
    if (destHit?.dir && dir) return { ok: false, code: 'merge', error: `${dest} is a folder. Folders are not merged.` };
    if (destHit?.dir) return { ok: false, code: 'merge', error: `${dest} is a folder. Not replaced with a file.` };
    return { ok: false, code: 'merge', error: `${dest} is a file. Not replaced with a folder.` };
  }
  if (destHit && move && srcVol === destVol) {
    return { ok: false, code: 'exists', error: `${dest} already exists. A move on the same storage does not replace it.` };
  }

  const files = measured?.files || [];
  const max = destVol === 'lfs' ? FILE_MAX : SD_FILE_MAX;
  for (const f of files) {
    if ((f.size || 0) > max) {
      const error = destVol === 'lfs'
        ? `${f.path} is ${f.size} bytes, over the ${FILE_MAX / 1024} KB limit on internal storage.`
        : `${f.path} is over the SD card's 4 GB file limit.`;
      return { ok: false, code: 'limit', error, destVol };
    }
  }

  const replace = !!(destHit && !(move && srcVol === destVol));
  const reclaim = replace ? reclaimSize(destHit, destVol) : 0;
  const need = transferNeed({
    move,
    srcVol,
    destVol,
    files,
    dirCount: dir ? (measured?.dirs || []).length : 0,
    block: measured?.block || BLOCK,
    reclaim,
  });
  const fit = fits(free, need);
  if (!fit.ok && fit.code === 'free') return { ok: false, code: 'free', need, destVol };
  if (!fit.ok) return { ok: false, code: 'space', need, free, destVol, error: 'not enough space' };

  const mkdirs = dir ? [...(measured?.dirs || [])].reverse().map((d) => mapTo(src, dest, d)) : [];
  const copied = files.map((f) => ({
    from: f.path,
    to: mapTo(src, dest, f.path),
    size: f.size || 0,
  }));
  const remove = move && srcVol !== destVol
    ? [...files.map((f) => f.path), ...(measured?.dirs || [])]
    : [];
  return {
    ok: true,
    action: move && srcVol === destVol ? 'rename' : 'copy',
    src,
    dest,
    move,
    replace,
    need,
    srcVol,
    destVol,
    files: copied,
    mkdirs,
    remove,
  };
}

/**
 * Run a plan. io.read returns { ok, bytes, aborted }. io.write takes the
 * bytes. A copy that stops leaves the partial destination and the source.
 * The source is removed only after every file is written.
 */
export async function runTransfer(plan, io) {
  if (plan.action === 'rename') {
    const r = await io.rename(plan.src, plan.dest);
    if (!r?.ok) return { ok: false, error: r?.error || 'rename failed', status: r?.status };
    return { ok: true, action: 'rename' };
  }
  let touched = false;
  for (const d of plan.mkdirs || []) {
    if (io.signal?.aborted) return { ok: false, aborted: true, partial: touched };
    const r = await io.mkdir(d);
    if (!r?.ok && r?.status !== 409 && !/exist/i.test(r?.error || '')) {
      return { ok: false, partial: touched, error: `${d}: ${r?.error || 'could not create the folder'}` };
    }
    touched = true;
  }
  let done = 0;
  const total = (plan.files || []).reduce((n, f) => n + (f.size || 0), 0);
  for (let i = 0; i < (plan.files || []).length; i++) {
    const f = plan.files[i];
    if (io.signal?.aborted) return { ok: false, aborted: true, partial: touched, at: f.to };
    const r = await io.read(f.from, {
      signal: io.signal,
      onProgress: (p) => io.onProgress?.({
        done, total, path: f.to, got: p?.got || 0, index: i, count: plan.files.length,
      }),
    });
    if (r?.aborted || io.signal?.aborted) return { ok: false, aborted: true, partial: touched, at: f.to };
    if (!r?.ok) {
      return { ok: false, partial: touched, at: f.to, error: `${f.from}: ${r?.error || 'read failed'}` };
    }
    const w = await io.write(f.to, r.bytes);
    touched = true;
    if (io.signal?.aborted) return { ok: false, aborted: true, partial: true, at: f.to };
    if (!w?.ok) return { ok: false, partial: true, at: f.to, error: `${f.to}: ${w?.error || 'write failed'}` };
    done += f.size || r.bytes?.length || 0;
    io.onProgress?.({ done, total, path: f.to, got: 0, index: i + 1, count: plan.files.length });
  }
  if (plan.remove?.length) {
    const removed = [];
    for (const p of plan.remove) {
      if (io.signal?.aborted) {
        return { ok: false, aborted: true, copied: true, partial: true, at: p, removed };
      }
      const r = await io.remove(p);
      if (!r?.ok && r?.status !== 404) {
        return {
          ok: false, copied: true, partial: true, at: p, removed,
          error: `${p}: ${r?.error || 'could not remove'}`,
        };
      }
      removed.push(p);
    }
  }
  return { ok: true, action: 'copy' };
}

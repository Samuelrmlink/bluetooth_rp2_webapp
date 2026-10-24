// WASI preview1 for the pexcc command (wasi_snapshot_preview1).
// Enough for fopen, printf, and argv. Directories are in-memory trees the
// page fills from the SD card. A guest path is a preopen such as /include.

const ERR = {
  SUCCESS: 0, ACCES: 2, BADF: 8, EXIST: 20, FAULT: 21, INVAL: 28, IO: 29,
  ISDIR: 31, NAMETOOLONG: 37, NOENT: 44, NOSPC: 51, NOTDIR: 54, SPIPE: 70,
  NOTCAPABLE: 76,
};
const OF = { CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8 };
const SEEK = { SET: 0, CUR: 1, END: 2 };
const FILETYPE = { CHAR: 2, DIR: 3, REG: 4 };
const ALL_RIGHTS = 0xffffffffffffffffn;

function tracing() {
  try { return globalThis.process?.env?.PEXCC_WASI_TRACE === '1'; }
  catch { return false; }
}

function asDir(obj) {
  const entries = new Map();
  for (const [k, v] of Object.entries(obj || {})) {
    if (v instanceof Uint8Array) entries.set(k, { kind: 'file', data: new Uint8Array(v) });
    else entries.set(k, asDir(v));
  }
  return { kind: 'dir', entries };
}

export function readGuest(preopens, path) {
  const p = path.startsWith('/') ? path : `/${path}`;
  let best = null;
  for (const pre of preopens) {
    const guest = pre.guest.endsWith('/') && pre.guest.length > 1 ? pre.guest.slice(0, -1) : pre.guest;
    if (p === guest || p.startsWith(`${guest}/`)) {
      if (!best || guest.length > best.guest.length) best = { dir: pre.dir, guest };
    }
  }
  if (!best) return null;
  const rel = p.slice(best.guest.length).replace(/^\//, '');
  if (!rel) return null;
  let cur = best.dir;
  for (const part of rel.split('/')) {
    if (!cur || cur.kind !== 'dir') return null;
    cur = cur.entries.get(part);
  }
  if (!cur || cur.kind !== 'file') return null;
  return new Uint8Array(cur.data);
}

function resolve(dir, path) {
  const parts = [];
  for (const raw of path.split('/')) {
    if (raw === '' || raw === '.') continue;
    if (raw === '..') {
      if (parts.length === 0) return { err: ERR.NOTCAPABLE };
      parts.pop();
      continue;
    }
    parts.push(raw);
  }
  let cur = dir;
  for (let i = 0; i < parts.length; i++) {
    const name = parts[i];
    const last = i === parts.length - 1;
    if (!cur || cur.kind !== 'dir') return { err: ERR.NOTDIR };
    const ent = cur.entries.get(name);
    if (!ent) return last ? { missing: true, parent: cur, name } : { err: ERR.NOENT };
    if (last) return { ent, parent: cur, name };
    cur = ent;
  }
  return { ent: dir, parent: null, name: '' };
}

/**
 * Run a WASI command. preopens: [{ guest: '/include', files: { 'pex.h': Uint8Array } }].
 * Returns { exit, stdout, preopens } with the same directory objects mutated.
 */
export async function startWasi(wasmBytes, opts) {
  const preopens = (opts.preopens || []).map((p) => ({
    guest: p.guest.endsWith('/') && p.guest.length > 1 ? p.guest.slice(0, -1) : p.guest,
    dir: p.dir && p.dir.kind === 'dir' ? p.dir : asDir(p.files),
  }));
  const args = opts.args || ['pexcc'];
  const chunks = [];
  const box = { memory: null };
  const trace = tracing();
  const fds = [
    { kind: 'char', name: 'stdin' },
    { kind: 'char', name: 'stdout' },
    { kind: 'char', name: 'stderr' },
  ];
  for (const pre of preopens) fds.push({ kind: 'dir', dir: pre.dir, guest: pre.guest });

  function mem() {
    const m = box.memory;
    if (!m) return null;
    return new Uint8Array(m.buffer, m.byteOffset, m.byteLength);
  }
  function view() {
    const m = box.memory;
    return new DataView(m.buffer, m.byteOffset, m.byteLength);
  }
  function okRange(ptr, len) {
    const m = mem();
    return !!m && ptr >= 0 && len >= 0 && ptr <= m.length && len <= m.length - ptr;
  }
  function w32(ptr, val) {
    if (!okRange(ptr, 4)) return ERR.FAULT;
    view().setUint32(ptr, val >>> 0, true);
    return ERR.SUCCESS;
  }
  function readStr(ptr, len) {
    if (!okRange(ptr, len)) return null;
    return new TextDecoder().decode(mem().subarray(ptr, ptr + len));
  }
  function iovs(ptr, count, write) {
    if (!okRange(ptr, count * 8)) return { err: ERR.FAULT };
    const v = view();
    const m = mem();
    let n = 0;
    for (let i = 0; i < count; i++) {
      const buf = v.getUint32(ptr + i * 8, true);
      const len = v.getUint32(ptr + i * 8 + 4, true);
      if (!okRange(buf, len)) return { err: ERR.FAULT };
      const slice = m.subarray(buf, buf + len);
      const got = write(slice);
      n += got;
      if (got < len) break;
    }
    return { err: ERR.SUCCESS, n };
  }

  const api = {
    args_sizes_get(argcPtr, bufPtr) {
      const bytes = args.reduce((n, a) => n + new TextEncoder().encode(a).length + 1, 0);
      if (w32(argcPtr, args.length) || w32(bufPtr, bytes)) return ERR.FAULT;
      return ERR.SUCCESS;
    },
    args_get(argvPtr, bufPtr) {
      const enc = new TextEncoder();
      const m = mem();
      const v = view();
      if (!okRange(argvPtr, args.length * 4)) return ERR.FAULT;
      let at = bufPtr;
      for (let i = 0; i < args.length; i++) {
        const b = enc.encode(args[i]);
        if (!okRange(at, b.length + 1)) return ERR.FAULT;
        v.setUint32(argvPtr + i * 4, at, true);
        m.set(b, at);
        m[at + b.length] = 0;
        at += b.length + 1;
      }
      return ERR.SUCCESS;
    },
    environ_sizes_get(countPtr, bufPtr) {
      if (w32(countPtr, 0) || w32(bufPtr, 0)) return ERR.FAULT;
      return ERR.SUCCESS;
    },
    environ_get() { return ERR.SUCCESS; },
    fd_prestat_get(fd, buf) {
      const f = fds[fd];
      if (!f || f.kind !== 'dir' || !f.guest) return ERR.BADF;
      if (!okRange(buf, 8)) return ERR.FAULT;
      const m = mem();
      m.fill(0, buf, buf + 8);
      m[buf] = 0;
      view().setUint32(buf + 4, new TextEncoder().encode(f.guest).length, true);
      return ERR.SUCCESS;
    },
    fd_prestat_dir_name(fd, path, pathLen) {
      const f = fds[fd];
      if (!f || f.kind !== 'dir' || !f.guest) return ERR.BADF;
      const name = new TextEncoder().encode(f.guest);
      if (pathLen < name.length) return ERR.NAMETOOLONG;
      if (!okRange(path, name.length)) return ERR.FAULT;
      mem().set(name, path);
      return ERR.SUCCESS;
    },
    fd_fdstat_get(fd, buf) {
      const f = fds[fd];
      if (!f) return ERR.BADF;
      if (!okRange(buf, 24)) return ERR.FAULT;
      const m = mem();
      m.fill(0, buf, buf + 24);
      const v = view();
      const ft = f.kind === 'char' ? FILETYPE.CHAR : f.kind === 'dir' ? FILETYPE.DIR : FILETYPE.REG;
      m[buf] = ft;
      v.setUint16(buf + 2, f.fdflags || 0, true);
      v.setBigUint64(buf + 8, ALL_RIGHTS, true);
      v.setBigUint64(buf + 16, ALL_RIGHTS, true);
      return ERR.SUCCESS;
    },
    fd_fdstat_set_flags(fd, flags) {
      const f = fds[fd];
      if (!f) return ERR.BADF;
      f.fdflags = flags >>> 0;
      return ERR.SUCCESS;
    },
    fd_close(fd) {
      if (fd < 3 || !fds[fd]) return ERR.BADF;
      fds[fd] = null;
      return ERR.SUCCESS;
    },
    fd_seek(fd, offset, whence, out) {
      const f = fds[fd];
      if (!f) return ERR.BADF;
      if (f.kind === 'char') {
        if (w32(out, 0) || w32(out + 4, 0)) return ERR.FAULT;
        return ERR.SUCCESS;
      }
      if (f.kind !== 'file') return ERR.SPIPE;
      const len = BigInt(f.file.data.length);
      const off = typeof offset === 'bigint' ? offset : BigInt(offset);
      let next;
      if (whence === SEEK.SET) next = off;
      else if (whence === SEEK.CUR) next = BigInt(f.pos) + off;
      else if (whence === SEEK.END) next = len + off;
      else return ERR.INVAL;
      if (next < 0n || next > 0xffffffffn) return ERR.INVAL;
      f.pos = Number(next);
      const v = view();
      if (!okRange(out, 8)) return ERR.FAULT;
      v.setBigUint64(out, BigInt(f.pos), true);
      return ERR.SUCCESS;
    },
    fd_read(fd, iovPtr, iovLen, nread) {
      const f = fds[fd];
      if (!f) return ERR.BADF;
      if (f.kind === 'char') {
        if (w32(nread, 0)) return ERR.FAULT;
        return ERR.SUCCESS;
      }
      if (f.kind !== 'file') return ERR.ISDIR;
      const r = iovs(iovPtr, iovLen, (slice) => {
        const have = f.file.data.length - f.pos;
        const n = Math.min(slice.length, Math.max(0, have));
        if (n > 0) slice.set(f.file.data.subarray(f.pos, f.pos + n));
        f.pos += n;
        return n;
      });
      if (r.err) return r.err;
      if (w32(nread, r.n)) return ERR.FAULT;
      return ERR.SUCCESS;
    },
    fd_write(fd, iovPtr, iovLen, nwritten) {
      const f = fds[fd];
      if (!f) return ERR.BADF;
      if (f.kind === 'char') {
        const parts = [];
        const r = iovs(iovPtr, iovLen, (slice) => {
          parts.push(new Uint8Array(slice));
          return slice.length;
        });
        if (r.err) return r.err;
        if (fd === 1 || fd === 2) for (const p of parts) chunks.push(p);
        if (w32(nwritten, r.n)) return ERR.FAULT;
        return ERR.SUCCESS;
      }
      if (f.kind !== 'file') return ERR.ISDIR;
      if (f.fdflags & 1) f.pos = f.file.data.length;
      const r = iovs(iovPtr, iovLen, (slice) => {
        const end = f.pos + slice.length;
        if (end > f.file.data.length) {
          const grown = new Uint8Array(end);
          grown.set(f.file.data, 0);
          f.file.data = grown;
        }
        f.file.data.set(slice, f.pos);
        f.pos = end;
        return slice.length;
      });
      if (r.err) return r.err;
      if (w32(nwritten, r.n)) return ERR.FAULT;
      return ERR.SUCCESS;
    },
    path_open(dirfd, _dirflags, pathPtr, pathLen, oflags, _base, _inh, fdflags, opened) {
      const dir = fds[dirfd];
      if (!dir || dir.kind !== 'dir') return ERR.BADF;
      const path = readStr(pathPtr, pathLen);
      if (path == null) return ERR.FAULT;
      if (trace) console.error('path_open', dir.guest, JSON.stringify(path), 'oflags', oflags);
      const hit = resolve(dir.dir, path);
      if (hit.err) return hit.err;
      let ent = hit.ent;
      if (hit.missing) {
        if (!(oflags & OF.CREAT)) return ERR.NOENT;
        ent = { kind: 'file', data: new Uint8Array(0) };
        hit.parent.entries.set(hit.name, ent);
      } else if ((oflags & OF.EXCL) && (oflags & OF.CREAT)) {
        return ERR.EXIST;
      }
      if (oflags & OF.DIRECTORY) {
        if (ent.kind !== 'dir') return ERR.NOTDIR;
        const fd = fds.length;
        fds.push({ kind: 'dir', dir: ent, guest: '' });
        if (w32(opened, fd)) return ERR.FAULT;
        return ERR.SUCCESS;
      }
      if (ent.kind === 'dir') {
        const fd = fds.length;
        fds.push({ kind: 'dir', dir: ent, guest: '' });
        if (w32(opened, fd)) return ERR.FAULT;
        return ERR.SUCCESS;
      }
      if (oflags & OF.TRUNC) ent.data = new Uint8Array(0);
      const fd = fds.length;
      fds.push({ kind: 'file', file: ent, pos: 0, fdflags: fdflags >>> 0 });
      if (w32(opened, fd)) return ERR.FAULT;
      return ERR.SUCCESS;
    },
    proc_exit(code) {
      box.exitCode = code >>> 0;
      throw new Error(`proc_exit ${box.exitCode}`);
    },
  };

  const wrapped = {};
  for (const [name, fn] of Object.entries(api)) {
    wrapped[name] = (...xs) => {
      const rc = fn(...xs);
      if (trace && name !== 'fd_write' && name !== 'fd_read' && name !== 'fd_seek') {
        console.error(name, xs.map((x) => typeof x === 'bigint' ? x.toString() : x).join(','), '->', rc);
      }
      return rc;
    };
  }

  const module = await WebAssembly.compile(wasmBytes);
  const instance = await WebAssembly.instantiate(module, { wasi_snapshot_preview1: wrapped });
  box.memory = instance.exports.memory;
  let trapped = null;
  try {
    instance.exports._start();
  } catch (e) {
    if (box.exitCode == null) trapped = e;
  }
  let total = 0;
  for (const c of chunks) total += c.length;
  const stdout = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) { stdout.set(c, at); at += c.length; }
  if (trapped) {
    const err = new Error(`compiler trapped: ${trapped.message || trapped}`);
    err.stdout = stdout;
    throw err;
  }
  return { exit: box.exitCode ?? 0, stdout, preopens };
}

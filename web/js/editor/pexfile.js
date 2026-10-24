// A Pexfile is the project file for one module on the card. Several target
// blocks are several ways to build that one module (board, run mode, sources).
// A second module is a second folder. Paths are relative to the Pexfile.
//
// The page parses this. There is no Make binary. Host `make` does not see
// the name Pexfile.
//
// A target line may end with `{`, and `}` ends that target. src, inc, and
// define may be a `[ ]` list, which replaces the whole list. One entry per
// line, or several on one line. Commas are optional. A repeated key is the
// other form; mixing the two for one key is an error.

const IDENT = /^[A-Za-z_][A-Za-z0-9_]*$/;
const NAME = /^[A-Za-z_][A-Za-z0-9_-]*$/;
const ARCH = /^[a-z][a-z0-9]*$/;
const CONST = /^(0x[0-9A-Fa-f]+|\d+|[A-Za-z_][A-Za-z0-9_]*)$/;
const LEVELS = new Set(['error', 'warn', 'info', 'debug']);
const RANK = { error: 0, warn: 1, info: 2, debug: 3 };

const FILE_KEYS = new Set([
  'name', 'version', 'kind', 'model', 'level', 'log', 'default',
  'arch', 'board', 'out', 'run', 'src', 'inc', 'define', 'target',
]);
const TARGET_KEYS = new Set(['arch', 'board', 'out', 'run', 'level', 'log', 'src', 'inc', 'define']);
const LISTS = new Set(['src', 'inc', 'define']);

export function keepLevel(ceiling, level) {
  return (RANK[level] ?? 99) <= (RANK[ceiling] ?? RANK.info);
}

export class BuildLog {
  constructor(level = 'info') {
    this.level = LEVELS.has(level) ? level : 'info';
    this.lines = [];
  }

  add(level, text) {
    const body = String(text ?? '');
    if (!body || !keepLevel(this.level, level)) return false;
    for (const line of body.split('\n')) {
      if (line) this.lines.push({ level, text: line });
    }
    return true;
  }

  text() {
    return this.lines.length ? `${this.lines.map((l) => l.text).join('\n')}\n` : '';
  }

  last() {
    return this.lines.length ? this.lines[this.lines.length - 1].text : '';
  }
}

// Why a path relative to a Pexfile cannot be used, or null.
export function badRel(path) {
  const p = String(path || '');
  if (!p) return 'path is empty';
  if (p.startsWith('/') || p.startsWith('\\')) return 'path must stay inside the project';
  if (/^[A-Za-z]:/.test(p)) return 'path must stay inside the project';
  if (/[\\:*?"<>|]/.test(p) || /[\0-\x1f]/.test(p)) return 'a character the card cannot name';
  const parts = p.split('/');
  if (parts.some((s) => s === '' || s === '.' || s === '..')) return 'path has an empty segment or ..';
  return null;
}

export function boardMacro(name) {
  return `PEX_BOARD_${String(name || '').toUpperCase().replace(/[^A-Z0-9]/g, '_')}`;
}

function strip(text) {
  const t = String(text || '').replace(/^\uFEFF/, '');
  return t.split(/\r?\n/);
}

function parseDefine(body, lineNo, errors) {
  const m = /^([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*(\S+))?$/.exec(body);
  if (!m || !IDENT.test(m[1])) {
    errors.push({ line: lineNo, message: 'define needs a name, or name=value' });
    return null;
  }
  const value = m[2] == null ? '1' : m[2];
  if (value.includes('(') || !CONST.test(value)) {
    errors.push({ line: lineNo, message: `define ${m[1]} is not an object-like constant` });
    return null;
  }
  return { name: m[1], value };
}

function takePath(body, lineNo, errors, what) {
  const why = badRel(body);
  if (why) {
    errors.push({ line: lineNo, message: `${what}: ${why}` });
    return null;
  }
  if (what === 'src' && !body.endsWith('.c')) {
    errors.push({ line: lineNo, message: `src ${body} is not a .c file` });
    return null;
  }
  return body;
}

function blankTarget(name, shared) {
  return {
    name,
    arch: shared.arch,
    board: shared.board,
    out: shared.out || 'build',
    run: shared.run || 'flash',
    level: shared.level || 'info',
    log: shared.log,
    src: shared.src.slice(),
    inc: shared.inc.slice(),
    define: shared.define.map((d) => ({ ...d })),
    replaced: { src: false, inc: false, define: false },
    listKind: { src: null, inc: null, define: null },
  };
}

function listTokens(chunk) {
  return String(chunk || '').split(/[,\s]+/).filter(Boolean);
}

// `rest` includes the opening `[`. `]` may be absent when the list continues.
function bracketBody(rest) {
  const body = rest.slice(1).trim();
  const at = body.indexOf(']');
  if (at === -1) return { items: listTokens(body), closed: false, error: null };
  if (body.slice(at + 1).trim()) return { items: [], closed: true, error: '] ends the list' };
  return { items: listTokens(body.slice(0, at)), closed: true, error: null };
}

function bracketContinuation(line) {
  const t = line.trim();
  if (t.replace(/[,\s]/g, '') === ']') return { items: [], closed: true, error: null };
  return bracketBody(`[${t}`);
}

function pushListItems(key, dest, items, lineNo, errors) {
  for (const item of items) {
    if (key === 'define') {
      const d = parseDefine(item, lineNo, errors);
      if (d) dest.define.push(d);
      continue;
    }
    const p = takePath(item, lineNo, errors, key);
    if (p) dest[key].push(p);
  }
}

function checkScalar(key, body, lineNo, errors) {
  if (key === 'name' || key === 'default' || key === 'target') {
    if (!NAME.test(body) || body.length > 23) {
      errors.push({ line: lineNo, message: `${key} must be a short name` });
      return null;
    }
  }
  if (key === 'version' && !/^[0-9A-Za-z._+-]+$/.test(body)) {
    errors.push({ line: lineNo, message: 'version has a character the linker will not take' });
    return null;
  }
  if (key === 'kind' && body !== 'app' && body !== 'lua') {
    errors.push({ line: lineNo, message: 'kind is app or lua' });
    return null;
  }
  if (key === 'model' && body !== 'reloc' && body !== 'pic') {
    errors.push({ line: lineNo, message: 'model is reloc or pic' });
    return null;
  }
  if (key === 'run' && body !== 'flash' && body !== 'ram') {
    errors.push({ line: lineNo, message: 'run is flash or ram' });
    return null;
  }
  if (key === 'level' && !LEVELS.has(body)) {
    errors.push({ line: lineNo, message: 'level is error, warn, info, or debug' });
    return null;
  }
  if (key === 'arch' && !ARCH.test(body)) {
    errors.push({ line: lineNo, message: 'arch is a lowercase cpu token such as m33' });
    return null;
  }
  if ((key === 'board' || key === 'out' || key === 'log') && badRel(body)) {
    errors.push({ line: lineNo, message: `${key}: ${badRel(body)}` });
    return null;
  }
  if (key === 'board' && !NAME.test(body)) {
    errors.push({ line: lineNo, message: 'board must be a name such as pico2_w' });
    return null;
  }
  return body;
}

function finishTarget(t) {
  const out = { ...t };
  delete out.replaced;
  delete out.listKind;
  out.out = String(out.out || 'build').replace(/\/+$/, '') || 'build';
  return out;
}

/**
 * Parse a Pexfile. `ok` is false when `errors` is non-empty. `project` is
 * still filled so a log can name every problem.
 */
export function parsePexfile(text) {
  const errors = [];
  const shared = {
    name: '', version: '0.1.0', kind: 'app', model: 'reloc',
    level: 'info', log: null, arch: null, board: null, out: 'build', run: 'flash',
    src: [], inc: [], define: [],
  };
  const set = new Set();
  let defaultTarget = null;
  const targets = [];
  let cur = null;
  let sawTarget = false;
  const sharedKind = { src: null, inc: null, define: null };
  let list = null;
  let braceLine = 0;

  const lines = strip(text);
  lines.forEach((raw, i) => {
    const lineNo = i + 1;
    if (!raw.trim() || raw.trimStart().startsWith('#')) return;
    const trimmed = raw.trim();

    if (list && trimmed !== '}') {
      const part = bracketContinuation(trimmed);
      if (part.error) {
        errors.push({ line: lineNo, message: part.error });
        list = null;
        return;
      }
      pushListItems(list.key, list.dest, part.items, lineNo, errors);
      if (part.closed) list = null;
      return;
    }
    if (list && trimmed === '}') {
      errors.push({ line: list.line, message: `${list.key} list is missing ]` });
      list = null;
    }

    if (trimmed === '}') {
      if (!braceLine || !cur) {
        errors.push({ line: lineNo, message: 'closing brace is outside a target' });
        return;
      }
      braceLine = 0;
      cur = null;
      return;
    }

    const indented = /^[ \t]/.test(raw);
    const bodyLine = trimmed;
    const sp = bodyLine.indexOf(' ');
    const key = sp === -1 ? bodyLine : bodyLine.slice(0, sp);
    const rest = sp === -1 ? '' : bodyLine.slice(sp + 1).trim();

    if (!braceLine && !indented && cur) cur = null;
    if (sawTarget && !braceLine && !indented && key !== 'target') {
      errors.push({ line: lineNo, message: `${key} belongs above the first target, or indented inside one` });
      return;
    }
    if (indented && !cur) {
      errors.push({ line: lineNo, message: 'indented line is outside a target' });
      return;
    }
    if (cur) {
      if (!TARGET_KEYS.has(key)) {
        errors.push({ line: lineNo, message: `unknown key ${key}` });
        return;
      }
    } else if (!FILE_KEYS.has(key)) {
      errors.push({ line: lineNo, message: `unknown key ${key}` });
      return;
    }
    if (!rest && key !== 'define') {
      errors.push({ line: lineNo, message: `${key} needs a value` });
      return;
    }

    if (key === 'target') {
      if (braceLine && cur) {
        errors.push({ line: lineNo, message: `target ${cur.name} is missing }` });
        return;
      }
      let namePart = rest;
      let opens = false;
      if (namePart.endsWith('{')) {
        opens = true;
        namePart = namePart.slice(0, -1).trim();
      }
      const name = checkScalar('target', namePart, lineNo, errors);
      if (!name) return;
      if (targets.some((t) => t.name === name)) {
        errors.push({ line: lineNo, message: `target ${name} is already defined` });
        return;
      }
      sawTarget = true;
      cur = blankTarget(name, shared);
      targets.push(cur);
      if (opens) braceLine = lineNo;
      return;
    }

    const dest = cur || shared;
    if (LISTS.has(key)) {
      const kindMap = cur ? cur.listKind : sharedKind;
      if (rest.startsWith('[')) {
        const part = bracketBody(rest);
        if (part.error) {
          errors.push({ line: lineNo, message: part.error });
          return;
        }
        if (kindMap[key]) {
          errors.push({ line: lineNo, message: `${key} is already set` });
          return;
        }
        if (cur && !cur.replaced[key]) {
          dest[key] = [];
          cur.replaced[key] = true;
        }
        kindMap[key] = 'brackets';
        pushListItems(key, dest, part.items, lineNo, errors);
        if (!part.closed) list = { key, dest, line: lineNo };
        return;
      }
      if (kindMap[key] === 'brackets') {
        errors.push({ line: lineNo, message: `${key} is already set` });
        return;
      }
      kindMap[key] = 'lines';
      if (cur && !cur.replaced[key]) {
        dest[key] = [];
        cur.replaced[key] = true;
      }
      if (key === 'define') {
        const d = parseDefine(rest, lineNo, errors);
        if (d) dest.define.push(d);
        return;
      }
      const p = takePath(rest, lineNo, errors, key);
      if (p) dest[key].push(p);
      return;
    }

    if (key === 'default') {
      const name = checkScalar('default', rest, lineNo, errors);
      if (!name) return;
      if (set.has('default')) errors.push({ line: lineNo, message: 'default is already set' });
      set.add('default');
      defaultTarget = name;
      return;
    }

    const value = checkScalar(key, rest, lineNo, errors);
    if (value == null) return;
    const slot = cur ? `target:${cur.name}:${key}` : key;
    if (set.has(slot)) errors.push({ line: lineNo, message: `${key} is already set` });
    set.add(slot);
    if (cur) cur[key] = value;
    else shared[key] = value;
  });

  if (list) errors.push({ line: list.line, message: `${list.key} list is missing ]` });
  if (braceLine && cur) errors.push({ line: braceLine, message: `target ${cur.name} is missing }` });
  if (!shared.name) errors.push({ line: 0, message: 'name is required' });
  const finished = (targets.length ? targets : [blankTarget('default', shared)]).map(finishTarget);
  if (!targets.length && defaultTarget) {
    errors.push({ line: 0, message: 'default names a target and this file has none' });
  }
  if (defaultTarget && !finished.some((t) => t.name === defaultTarget)) {
    errors.push({ line: 0, message: `default ${defaultTarget} is not a target` });
  }
  const outs = new Map();
  for (const t of finished) {
    if (!t.src.length) errors.push({ line: 0, message: `target ${t.name} has no src` });
    if (outs.has(t.out)) errors.push({ line: 0, message: `targets ${outs.get(t.out)} and ${t.name} both write ${t.out}` });
    else outs.set(t.out, t.name);
  }

  const project = {
    name: shared.name,
    version: shared.version,
    kind: shared.kind,
    model: shared.model,
    level: shared.level || 'info',
    defaultTarget: defaultTarget || (finished[0] ? finished[0].name : null),
    targets: finished,
  };
  return { ok: errors.length === 0, errors, project };
}

export function parseBoard(text) {
  const errors = [];
  const board = { name: '', arch: '', header: null, define: [] };
  const set = new Set();
  strip(text).forEach((raw, i) => {
    const lineNo = i + 1;
    if (!raw.trim() || raw.trimStart().startsWith('#')) return;
    if (/^[ \t]/.test(raw)) {
      errors.push({ line: lineNo, message: 'a board file has no indented keys' });
      return;
    }
    const bodyLine = raw.trim();
    const sp = bodyLine.indexOf(' ');
    const key = sp === -1 ? bodyLine : bodyLine.slice(0, sp);
    const rest = sp === -1 ? '' : bodyLine.slice(sp + 1).trim();
    if (!['name', 'arch', 'header', 'define'].includes(key)) {
      errors.push({ line: lineNo, message: `unknown key ${key}` });
      return;
    }
    if (key === 'define') {
      const d = parseDefine(rest, lineNo, errors);
      if (d) board.define.push(d);
      return;
    }
    if (!rest) {
      errors.push({ line: lineNo, message: `${key} needs a value` });
      return;
    }
    if (set.has(key)) errors.push({ line: lineNo, message: `${key} is already set` });
    set.add(key);
    if (key === 'name' && !NAME.test(rest)) errors.push({ line: lineNo, message: 'board name must be a name such as pico2_w' });
    if (key === 'arch' && !ARCH.test(rest)) errors.push({ line: lineNo, message: 'arch is a lowercase cpu token such as m33' });
    if (key === 'header' && badRel(rest)) errors.push({ line: lineNo, message: `header: ${badRel(rest)}` });
    board[key] = rest;
  });
  if (!board.name) errors.push({ line: 0, message: 'board name is required' });
  if (!board.arch) errors.push({ line: 0, message: 'board arch is required' });
  return { ok: errors.length === 0, errors, board };
}

// Directories of `filePath`, nearest first, each as a Pexfile path.
// The card's root is `sd`. LittleFS stops at the root file `Pexfile`.
export function pexfileCandidates(filePath) {
  const parts = String(filePath || '').split('/').filter(Boolean);
  if (!parts.length) return [];
  const onCard = parts[0] === 'sd';
  const out = [];
  if (parts[parts.length - 1] === 'Pexfile') out.push(parts.join('/'));
  const dir = parts.slice(0, -1);
  while (dir.length > (onCard ? 1 : 0)) {
    out.push([...dir, 'Pexfile'].join('/'));
    dir.pop();
  }
  out.push(onCard ? 'sd/Pexfile' : 'Pexfile');
  return [...new Set(out)];
}

export function boardCandidates(projectDir, boardName) {
  return [
    `${projectDir}/boards/${boardName}.board`,
    `sd/boards/${boardName}.board`,
  ];
}

export function joinProject(dir, rel) {
  const d = String(dir || '').replace(/\/+$/, '');
  const r = String(rel || '').replace(/^\/+/, '').replace(/\/+$/, '');
  return d && r ? `${d}/${r}` : (d || r);
}

export async function findPexfile(filePath, readText) {
  for (const path of pexfileCandidates(filePath)) {
    const r = await readText(path);
    if (r?.ok) return { path, text: r.text };
    if (r && r.status !== 404 && !r.missing) return { path, error: r.error || `HTTP ${r.status}` };
  }
  return null;
}

export async function findBoard(projectDir, boardName, readText) {
  for (const path of boardCandidates(projectDir, boardName)) {
    const r = await readText(path);
    if (r?.ok) return { path, text: r.text };
    if (r && r.status !== 404 && !r.missing) return { path, error: r.error || `HTTP ${r.status}` };
  }
  return null;
}

/**
 * Fill `arch` from the board file. `fallbackArch` is `pex.list`'s arch, used
 * only when the target names neither an arch nor a board (the implicit target).
 */
export function bindTarget(target, boardFile, fallbackArch) {
  const errors = [];
  let arch = target.arch || null;
  if (target.board) {
    if (!boardFile) errors.push(`board file ${target.board}.board was not found`);
    else {
      if (boardFile.name && boardFile.name !== target.board) {
        errors.push(`board file name is ${boardFile.name}, target ${target.name} asked for ${target.board}`);
      }
      if (boardFile.arch && arch && boardFile.arch !== arch) {
        errors.push(`target ${target.name} arch ${arch} does not match board ${target.board} arch ${boardFile.arch}`);
      }
      if (!arch) arch = boardFile.arch || null;
    }
  } else if (!arch) arch = fallbackArch || null;
  if (!arch) errors.push(`target ${target.name} has no arch`);
  return { ...target, arch, boardFile: boardFile || null, errors };
}

/**
 * Which target the Build button runs.
 * `board` is `sys.info.board` when the firmware reports it, otherwise null.
 * `pin` is a target name chosen from the menu this session. A pin whose
 * board is not `board` is ignored once `board` is known; the caller drops it.
 * Status `unreported` means several boards share `arch` and the page is not
 * guessing which board answered.
 */
export function selectTarget(project, { arch, board = null, pin = null } = {}) {
  const targets = project.targets || [];
  const candidates = targets.filter((t) => t.arch === arch);
  if (!candidates.length) {
    return {
      status: 'none',
      target: null,
      reason: arch ? `No target for arch ${arch}` : 'No target',
    };
  }
  let pool = candidates;
  let status = 'matched';
  if (board) {
    pool = candidates.filter((t) => t.board === board);
    if (!pool.length) {
      return { status: 'none', target: null, reason: `This project has no target for ${board}` };
    }
  } else {
    const boards = new Set(candidates.map((t) => t.board || ''));
    if (boards.size > 1) status = 'unreported';
  }
  if (pin) {
    const pinned = pool.find((t) => t.name === pin);
    if (pinned) return { status: 'pinned', target: pinned, reason: '' };
  }
  const def = project.defaultTarget && pool.find((t) => t.name === project.defaultTarget);
  return { status, target: def || pool[0], reason: '' };
}

export function matchingTargets(project, { arch, board = null } = {}) {
  const candidates = (project.targets || []).filter((t) => t.arch === arch);
  if (!board) return candidates;
  return candidates.filter((t) => t.board === board);
}

// Menu rows for a Pexfile: matching targets, then the rest, then Build all.
export function projectBuildItems(project, ctx) {
  const pool = matchingTargets(project, ctx);
  const inPool = new Set(pool.map((t) => t.name));
  const ordered = [...pool, ...(project.targets || []).filter((t) => !inPool.has(t.name))];
  const items = [];
  for (const t of ordered) {
    items.push({ id: 'pex-build', target: t.name, label: `Build ${t.name}` });
    items.push({ id: 'pex-brun', target: t.name, label: `Build and run ${t.name}` });
  }
  items.push({ id: 'pex-build-all', label: 'Build all' });
  return items;
}

export function buildButtonState(project, selection, { boardKnown = false } = {}) {
  if (!project) return { disabled: true, label: 'Build', title: 'This file is not in a project' };
  if (!selection?.target) {
    return { disabled: true, label: 'Build', title: selection?.reason || 'No target' };
  }
  const boardName = selection.target.board;
  const label = boardKnown && boardName && selection.target.name !== boardName
    ? `Build ${project.name} · ${selection.target.name} · ${boardName}`
    : `Build ${project.name} · ${selection.target.name}`;
  let title = label;
  if (selection.status === 'pinned') title += ' — pinned';
  else if (selection.status === 'unreported') title += ' — the board name is not reported';
  else if (boardKnown) title += ` — this board${selection.target.board ? ` (${selection.target.board})` : ''}`;
  return { disabled: false, label, title };
}

export function macroLines({ arch, boardName, boardDefines, targetDefines, header }) {
  const lines = [];
  if (arch) lines.push({ from: 'arch', line: `#define PEX_TARGET_${arch} 1` });
  if (boardName) lines.push({ from: 'board', line: `#define ${boardMacro(boardName)} 1` });
  for (const d of boardDefines || []) lines.push({ from: 'board', line: `#define ${d.name} ${d.value}` });
  for (const d of targetDefines || []) lines.push({ from: 'target', line: `#define ${d.name} ${d.value}` });
  if (header) lines.push({ from: 'header', line: '#include "board.h"' });
  return lines;
}

export function prefixSource(text, lines) {
  if (!lines?.length) return String(text ?? '');
  return `${lines.map((l) => l.line).join('\n')}\n${text ?? ''}`;
}

export function artifactRel(project, target) {
  return `${target.out}/${project.name}.${target.arch}.pex`;
}

export function logRel(target) {
  return target.log || `${target.out}/build.log`;
}

export function sdLoadPath(editorPath) {
  const p = String(editorPath || '');
  if (!p.startsWith('sd/')) return null;
  return `sd:/${p.slice(3)}`;
}

export const POOL_BYTES = 48 * 1024;

/** The largest file an sd: install can stage. The pool is 48 KB; a known free block is tighter. */
export function sdInstallCap(poolLargest) {
  return typeof poolLargest === 'number' ? poolLargest : POOL_BYTES;
}

export function verifiedPexRel(name, arch) {
  return `sd/pex/good/${name}.${arch}.pex`;
}

export function verifiedNoteRel(name, arch) {
  return `sd/pex/good/${name}.${arch}.txt`;
}

/**
 * The one question before an install.
 * `loaded` asks to unload and replace. A file larger than the pool is copied
 * to LittleFS first, because an sd: install reads the whole file into the pool.
 */
export function flashConfirm({ name, arch, loaded = false, bytes = 0, poolCap = POOL_BYTES, sdPath = '' }) {
  const signed = name === 'anker' ? ' This replaces the running signed module.' : '';
  if (loaded) {
    return `Unload ${name} and replace the installed module?${signed} Cancel leaves it loaded. The new file stays on the card.`;
  }
  if (bytes > poolCap) {
    return `Copy the module to pex/${name}.${arch}.pex and install it? The file is ${bytes} bytes, which does not fit in the PEX pool, so it is not installed from the SD card.${signed}`;
  }
  const from = sdPath || `${name}.${arch}.pex`;
  return `Install ${from} into the PEX partition?${signed}`;
}

export function ramDecision({ bytes, poolLargest }) {
  if (typeof poolLargest === 'number' && bytes > poolLargest) {
    return {
      ok: false,
      message: `File is ${bytes} bytes and the PEX pool's largest block is ${poolLargest}. Not loaded.`,
    };
  }
  return { ok: true };
}

export function nestFiles(entries) {
  const root = {};
  for (const e of entries) {
    const parts = String(e.path || '').split('/').filter(Boolean);
    if (!parts.length) continue;
    let cur = root;
    for (let i = 0; i < parts.length - 1; i++) {
      const p = parts[i];
      if (!cur[p] || cur[p] instanceof Uint8Array) cur[p] = {};
      cur = cur[p];
    }
    const data = typeof e.data === 'string' ? new TextEncoder().encode(e.data) : e.data;
    cur[parts[parts.length - 1]] = data;
  }
  return root;
}

// Quote includes look in the .c's directory, then include/. Headers live at
// their project path under /p, and also under /include relative to each inc
// directory. The board header is board.h and its own file name.
export function layoutGuest({ units, headers, boardHeaders }) {
  const tree = nestFiles([
    ...(headers || []).map((h) => ({ path: h.path, data: h.text })),
    ...(units || []).map((u) => ({ path: u.path, data: u.text })),
  ]);
  const includeEntries = [];
  for (const h of headers || []) {
    if (h.includeAs) includeEntries.push({ path: h.includeAs, data: h.text });
  }
  for (const h of boardHeaders || []) includeEntries.push({ path: h.name, data: h.text });
  return {
    inputs: (units || []).map((u) => `/p/${u.path}`),
    tree,
    include: nestFiles(includeEntries),
  };
}

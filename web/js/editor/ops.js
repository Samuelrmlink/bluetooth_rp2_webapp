// Selection, archive planning, the operation list, and its modal.
// One file so the board stores one flash block instead of four. The old
// paths (select.js, archive.js, jobs.js, joblist.js) re-export these names.

import { bytes } from './filemenu.js';
import { Dialog } from '../modals/dialog.js';

// How a gesture changes the file list's picked set.
//
// The cursor stays one path. A plain click selects only that row and the
// caller opens it. Ctrl or Cmd toggles the row and does not open it. Shift
// picks every visible row from the anchor through the row and replaces the
// set. j and k move the cursor and do not come through here, so they leave
// the set alone.

export function applySelect(rows, state, gesture, path) {
  const picked = new Set(state?.picked || []);
  const cursor = path;
  let anchor = state?.anchor || null;
  let open = false;
  if (gesture === 'toggle') {
    if (picked.has(path)) picked.delete(path);
    else picked.add(path);
    if (!anchor) anchor = path;
  } else if (gesture === 'range') {
    const from = anchor && rows.includes(anchor) ? anchor
      : (state?.cursor && rows.includes(state.cursor) ? state.cursor : path);
    const ia = rows.indexOf(from);
    const ib = rows.indexOf(path);
    picked.clear();
    if (ia < 0 || ib < 0) picked.add(path);
    else {
      const lo = Math.min(ia, ib);
      const hi = Math.max(ia, ib);
      for (let i = lo; i <= hi; i++) picked.add(rows[i]);
    }
    anchor = from;
  } else {
    picked.clear();
    picked.add(path);
    anchor = path;
    open = true;
  }
  return { cursor, anchor, picked, open };
}

// Shift is a range even with Ctrl held. The caller decides; this is the
// reading of one click or Space.
export function gestureOf(e) {
  if (e?.shiftKey) return 'range';
  if (e?.ctrlKey || e?.metaKey) return 'toggle';
  return 'plain';
}

// Which download a picked set becomes. A single flash folder is the GET
// the board already streams. Anything else on flash is one POST of the
// chosen paths. The card has no tar walker, so those paths are packed in
// the browser. A mix is still one archive: the flash half, then the card.

const onCard = (p) => p === 'sd' || (typeof p === 'string' && p.startsWith('sd/'));

export function pruneCovered(paths, isDir) {
  const dirs = paths.filter((p) => isDir(p));
  return paths.filter((p) => !dirs.some((d) => d !== p && (p === d || p.startsWith(`${d}/`))));
}

export function archivePlan(paths, isDir = () => false) {
  const list = pruneCovered([...new Set((paths || []).filter((p) => typeof p === 'string' && p))], isDir);
  const card = list.filter(onCard);
  const flash = list.filter((p) => !onCard(p));
  if (!flash.length && !card.length) return { kind: 'empty' };
  if (!flash.length) return { kind: 'card', card };
  const oneDir = flash.length === 1 && !!isDir(flash[0]);
  if (!card.length && oneDir) return { kind: 'get', folder: flash[0] };
  if (!card.length) return { kind: 'post', flash };
  if (oneDir) return { kind: 'mix-get', folder: flash[0], card };
  return { kind: 'mix-post', flash, card };
}

// The editor's operation list. Pure: the modal (joblist.js) draws it, and
// the window starts a job for each open, download, upload, move, delete,
// restore and compile.
//
// The ring is the fraction of the oldest job that is still running. When
// nothing is running it is full. The centre is blank for one job and the
// count of every job still in the list (finished ones included) for more.


const TERMINAL = new Set(['done', 'aborted', 'error']);

export function createJobs() {
  let seq = 1;
  const items = [];
  const listeners = new Set();
  const emit = () => { for (const fn of listeners) fn(items); };
  const find = (id) => items.find((j) => j.id === id);
  return {
    items,
    on(fn) { listeners.add(fn); return () => listeners.delete(fn); },
    start({ title, progress = 0, boardId = 0, onAbort = null, onBoard = null } = {}) {
      const job = {
        id: seq++,
        title: title || 'Working',
        line: title || 'Working',
        progress,
        state: 'run',
        error: '',
        boardId,
        onAbort,
        onBoard,
      };
      items.push(job);
      emit();
      return job.id;
    },
    update(id, { line, progress } = {}) {
      const j = find(id);
      if (!j || TERMINAL.has(j.state)) return;
      if (line != null) j.line = line;
      if (progress != null) j.progress = Math.max(0, Math.min(0.99, progress));
      emit();
    },
    requestAbort(id) {
      const j = find(id);
      if (!j || j.state !== 'run') return;
      j.state = 'aborting';
      try { j.onAbort?.(); } catch { /* the work reports the failure itself */ }
      emit();
    },
    finish(id, { error, aborted } = {}) {
      const j = find(id);
      if (!j || TERMINAL.has(j.state)) return;
      if (aborted || j.state === 'aborting') {
        j.state = 'aborted';
      } else if (error) {
        j.state = 'error';
        j.error = String(error);
      } else {
        j.state = 'done';
        j.progress = 1;
      }
      emit();
    },
    dismiss(id) {
      const i = items.findIndex((j) => j.id === id);
      if (i < 0 || !TERMINAL.has(items[i].state)) return;
      items.splice(i, 1);
      emit();
    },
    clearDone() {
      for (let i = items.length - 1; i >= 0; i--) {
        if (TERMINAL.has(items[i].state)) items.splice(i, 1);
      }
      emit();
    },
    notify(data) {
      if (!data || !data.id) return;
      const j = items.find((x) => x.boardId === data.id && x.state === 'run');
      j?.onBoard?.(data);
    },
  };
}

// A fetch that never got a reply is the word "unreachable". The operations
// list should say what happened. Any other message is already specific.
export function jobError(msg) {
  const m = String(msg || 'failed');
  if (m === 'unreachable' || m === 'disconnected' || m === 'not connected') {
    return 'The device did not answer.';
  }
  return m;
}

export function lineOf(job) {
  if (!job) return '';
  if (job.state === 'aborting') return `${job.line} · Aborting`;
  if (job.state === 'aborted') return `${job.line} · Aborted`;
  if (job.state === 'done') return `${job.line} · Done`;
  if (job.state === 'error') return `${job.line} · ${job.error || 'failed'}`;
  return job.line;
}

// The path is the part that can clip. Everything after the first separator
// is the progress, and the row keeps that even when the name is long.
export function lineParts(job) {
  const text = lineOf(job);
  const i = text.indexOf(' · ');
  if (i < 0) return { head: text, tail: '' };
  return { head: text.slice(0, i), tail: text.slice(i + 3) };
}

// progress is 0..1 of the current (oldest running) job. count is every job
// still listed. showCount is false while that count is 1, so the ring's
// centre stays empty.
export function ringOf(items) {
  const list = items || [];
  const current = list.find((j) => j.state === 'run' || j.state === 'aborting');
  return {
    progress: current ? current.progress : 1,
    count: list.length,
    showCount: list.length > 1,
    visible: list.length > 0,
  };
}

export function byteFrac(got, total) {
  if (!total || total <= 0) return 0;
  if (got >= total) return 0.99;
  return got / total;
}

export function byteLine(verb, name, got, total, via) {
  const left = `${verb} ${name}`;
  const how = via ? `${via} · ` : '';
  if (total > 0 && got > total) return `${left} · ${how}${bytes(got)}`;
  if (total > 0) return `${left} · ${how}${bytes(got)} of ${bytes(total)}`;
  if (got > 0) return `${left} · ${how}${bytes(got)}`;
  return via ? `${left} · ${via}` : left;
}

// One archive file. A large file also says how far into it the stream is.
// `got` is bytes of the archive since this file started, capped by the caller.
const LARGE = 64 * 1024;

export function fileLine(path, { index = 0, total = 0, got = 0, size = 0 } = {}) {
  const count = index && total ? `${index} of ${total}` : '';
  if (size >= LARGE) {
    const into = Math.min(size, Math.max(0, got));
    return [`Adding ${path}`, `${bytes(into)} of ${bytes(size)}`, count].filter(Boolean).join(' · ');
  }
  return [`Adding ${path}`, count].filter(Boolean).join(' · ');
}

// The operations modal and the toolbar ring. Minimizing hides the modal
// and leaves the ring. The ring stays until the list is empty.


const CIRC = 2 * Math.PI * 6;

export class JobList {
  constructor(app, jobs) {
    this.jobs = jobs;
    this.btn = document.getElementById('jobs-btn');
    this.arc = this.btn?.querySelector('.jobs-arc') || null;
    this.countEl = this.btn?.querySelector('.jobs-count') || null;
    if (this.arc) {
      this.arc.style.strokeDasharray = String(CIRC);
      this.arc.style.strokeDashoffset = '0';
    }
    const body = document.createElement('div');
    body.className = 'jobs-body';
    body.innerHTML = `
      <ul class="jobs-list"></ul>
      <div class="jobs-actions">
        <button type="button" class="pane-btn jobs-clear">Clear</button>
      </div>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'jobs-dlg', title: 'Operations', body }));
    this.dlg._overlay.classList.add('jobs-dlg');
    const close = this.dlg._overlay.querySelector('.modal-close');
    if (close) {
      close.textContent = '–';
      close.setAttribute('aria-label', 'Minimize');
      close.title = 'Minimize';
    }
    this.list = body.querySelector('.jobs-list');
    body.querySelector('.jobs-clear').addEventListener('click', () => jobs.clearDone());
    this.list.addEventListener('click', (e) => {
      const row = e.target.closest('[data-id]');
      if (!row) return;
      const id = Number(row.dataset.id);
      if (e.target.closest('.jobs-abort')) jobs.requestAbort(id);
      else if (e.target.closest('.jobs-x')) jobs.dismiss(id);
    });
    this.btn?.addEventListener('click', () => { if (!this.dlg.isOpen) this.dlg.open(); });
    jobs.on(() => this.paint());
    this._n = 0;
    this.paint();
  }

  paint() {
    const ring = ringOf(this.jobs.items);
    const n = this.jobs.items.length;
    if (n > this._n && !this.dlg.isOpen) this.dlg.open();
    this._n = n;
    if (this.btn) this.btn.hidden = !ring.visible;
    if (this.arc) this.arc.style.strokeDashoffset = String(CIRC * (1 - ring.progress));
    if (this.countEl) this.countEl.textContent = ring.showCount ? String(ring.count) : '';
    if (this.btn) {
      const label = ring.visible
        ? (ring.showCount ? `${ring.count} operations` : '1 operation')
        : 'Operations';
      this.btn.setAttribute('aria-label', label);
      this.btn.title = label;
    }
    // Rebuilding the row on every progress tick replaces the Abort button,
    // so a click that started on it never fires. The same id, state, and
    // tail presence keep the button; only the words and the ring move.
    const specs = this.jobs.items.map((j) => {
      const parts = lineParts(j);
      return {
        j,
        parts,
        terminal: j.state === 'done' || j.state === 'aborted' || j.state === 'error',
        key: `${j.id}:${j.state}:${parts.tail ? 1 : 0}`,
      };
    });
    const existing = [...this.list.children];
    const same = specs.length > 0 && existing.length === specs.length
      && existing.every((el, i) => el.dataset?.key === specs[i].key);
    if (same) {
      specs.forEach((s, i) => {
        const row = existing[i];
        const line = row.querySelector('.jobs-line');
        const tail = row.querySelector('.jobs-tail');
        if (line) line.textContent = s.parts.head;
        if (tail) tail.textContent = s.parts.tail;
      });
    } else if (!specs.length) {
      if (existing.length !== 1 || !String(existing[0].className).includes('jobs-empty')) {
        this.list.innerHTML = '<li class="jobs-empty dim">Nothing running.</li>';
      }
    } else {
      this.list.innerHTML = specs.map((s) => {
        const abort = s.j.state === 'run'
          ? `<button type="button" class="jobs-abort" aria-label="Abort">Abort</button>` : '';
        const x = s.terminal
          ? `<button type="button" class="jobs-x" aria-label="Dismiss">&times;</button>` : '';
        const tail = s.parts.tail ? `<span class="jobs-tail">${escapeHtml(s.parts.tail)}</span>` : '';
        return `<li class="jobs-row is-${s.j.state}" data-id="${s.j.id}" data-key="${s.key}">`
          + `<span class="jobs-line">${escapeHtml(s.parts.head)}</span>${tail}${abort}${x}</li>`;
      }).join('');
    }
    const clear = this.dlg.body.querySelector('.jobs-clear');
    if (clear) {
      clear.disabled = !this.jobs.items.some((j) => j.state === 'done' || j.state === 'aborted' || j.state === 'error');
    }
    if (!ring.visible && this.dlg.isOpen) this.dlg.close();
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

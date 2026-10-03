// The editor sidebar's Search tab: one query over every file on the device
// (or one folder), results grouped by file as they are found. The matching
// and the walk are editor/search.js's; this is the panel, the results list
// (j / k / Enter from the keyboard, as the file list) and a cache of what was
// read, so a second search does not read the device again.

import { compileQuery, searchFiles } from './search.js';

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export class SearchPanel {
  // list(dir), readText(path) -> the device; buffer(path) -> the open
  // buffer's text or null; onOpen(path, hit) shows a match.
  constructor(host, { list, readText, buffer, onOpen }) {
    this.list = list;
    this.readText = readText;
    this.buffer = buffer;
    this.onOpen = onOpen;
    this.cache = new Map();          // path -> { size, text }
    this.hits = [];                  // [{ path, m }] in the order shown
    this.cur = -1;                   // the result last opened (F4 goes on from it)
    this.sel = -1;                   // the keyboard's row in the list
    this._gen = 0;
    host.innerHTML = `
      <form class="es-form" autocomplete="off">
        <div class="es-row">
          <input class="es-q" type="search" placeholder="Search in files — Enter" aria-label="Search in files" spellcheck="false">
        </div>
        <div class="es-row es-opts">
          <button type="button" class="es-opt es-case" aria-pressed="false" title="Match case">Aa</button>
          <button type="button" class="es-opt es-word" aria-pressed="false" title="Whole word">ab</button>
          <button type="button" class="es-opt es-re" aria-pressed="false" title="Regular expression">.*</button>
          <input class="es-dir" placeholder="in folder (all but cache/ ota/ var/)" aria-label="Search only in this folder" spellcheck="false">
        </div>
      </form>
      <div class="es-status dim" aria-live="polite"></div>
      <div class="es-results" aria-label="Search results"></div>`;
    this.q = host.querySelector('.es-q');
    this.dir = host.querySelector('.es-dir');
    this.status = host.querySelector('.es-status');
    this.results = host.querySelector('.es-results');
    this.opts = { matchCase: host.querySelector('.es-case'), word: host.querySelector('.es-word'), regex: host.querySelector('.es-re') };
    host.querySelector('.es-form').addEventListener('submit', (e) => { e.preventDefault(); this.run(); });
    // Enter in either box searches (two text boxes and no submit button: a
    // browser does not submit such a form on Enter by itself)
    for (const box of [this.q, this.dir]) {
      box.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.isComposing) { e.preventDefault(); this.run(); }
      });
    }
    for (const b of Object.values(this.opts)) {
      b.addEventListener('click', () => {
        b.setAttribute('aria-pressed', String(b.getAttribute('aria-pressed') !== 'true'));
        if (this.q.value) this.run();
      });
    }
    this.results.addEventListener('click', (e) => {
      const hit = e.target.closest('.es-hit');
      if (hit) { this.go(Number(hit.dataset.i)); return; }
      const file = e.target.closest('.es-file');
      if (file) file.parentElement.classList.toggle('is-folded');
    });
  }

  focus(prefill) {
    if (prefill) this.q.value = prefill;
    this.q.focus();
    this.q.select();
  }

  option(name, on) { this.opts[name].setAttribute('aria-pressed', String(!!on)); }
  _opt(name) { return this.opts[name].getAttribute('aria-pressed') === 'true'; }

  // A file changed (saved, uploaded, deleted): read it again next time.
  invalidate(path) { if (path) this.cache.delete(path); else this.cache.clear(); }

  // Open buffers first (their unsaved text is what you are looking at), then
  // the cache when the listed size still matches, else the device.
  async _read(path, size) {
    const b = this.buffer(path);
    if (b != null) return { ok: true, text: b };
    const c = this.cache.get(path);
    if (c && c.size === size) return { ok: true, text: c.text };
    const r = await this.readText(path);
    if (r.ok) this.cache.set(path, { size, text: r.text });
    return r;
  }

  async run({ q = this.q.value, dir = this.dir.value } = {}) {
    this.q.value = q;
    this.dir.value = dir;
    const gen = ++this._gen;
    const root = dir.trim().replace(/^\/+|\/+$/g, '');
    const { re, error } = compileQuery({ q, matchCase: this._opt('matchCase'), word: this._opt('word'), regex: this._opt('regex') });
    this.hits = [];
    this.cur = -1;
    this.sel = -1;
    this.results.replaceChildren();
    if (!re) { this.status.textContent = error ? `Not a valid pattern: ${error}` : ''; return null; }
    this.status.textContent = 'Searching…';
    let res;
    try {
      res = await searchFiles({
        list: this.list, read: (p, s) => this._read(p, s), root, re,
        isCancelled: () => gen !== this._gen,
        onProgress: (n, of, p) => { if (gen === this._gen && p) this.status.textContent = `Searching… ${n} of ${of} files`; },
        onFile: (path, ms) => { if (gen === this._gen) this._addFile(path, ms); },
      });
    } catch (e) {
      if (gen === this._gen) this.status.textContent = `Could not search: ${e.message}`;
      return null;
    }
    if (gen !== this._gen) return null;
    const where = root ? ` in ${root}/` : '';
    this.status.textContent = res.matches
      ? `${res.matches}${res.truncated ? '+' : ''} match${res.matches === 1 ? '' : 'es'} in ${res.files} file${res.files === 1 ? '' : 's'}${where} (${res.searched} searched)`
        + (res.truncated ? ' — stopped at the first 2000' : '') + (res.errors.length ? ` · ${res.errors.length} unreadable` : '')
      : `No matches${where} (${res.searched} files searched)`;
    return res;
  }

  _addFile(path, ms) {
    const group = document.createElement('div');
    group.className = 'es-group';
    const first = this.hits.length;
    for (const m of ms) this.hits.push({ path, m });
    const line = (m, i) => {
      // the match with some context, the line's start trimmed if it is far
      const from = Math.max(0, m.col - 40);
      const pre = (from ? '…' : '') + m.text.slice(from, m.col).replace(/^\s+/, from ? '' : '');
      const hit = m.text.slice(m.col, m.col + m.len);
      const post = m.text.slice(m.col + m.len, m.col + m.len + 120);
      return `<div class="es-hit" data-i="${first + i}" title="${esc(path)}:${m.line}:${m.col + 1}"><span class="es-ln">${m.line}</span>`
        + `<span class="es-txt">${esc(pre)}<mark>${esc(hit)}</mark>${esc(post)}</span></div>`;
    };
    group.innerHTML = `<div class="es-file" title="${esc(path)}"><span class="es-name">${esc(path.split('/').pop())}</span>`
      + `<span class="es-path dim">${esc(path.split('/').slice(0, -1).join('/'))}</span><span class="es-count">${ms.length}</span></div>`
      + ms.map(line).join('');
    this.results.appendChild(group);
  }

  // Show result i (and remember it, for F4 / :cn).
  go(i) {
    const h = this.hits[i];
    if (!h) return false;
    this.cur = i;
    this.sel = i;
    this._paint();
    this.onOpen(h.path, h.m);
    return true;
  }

  // F4 / Shift+F4, :cn / :cp
  step(d) {
    if (!this.hits.length) return false;
    const n = this.hits.length;
    return this.go(this.cur < 0 ? (d > 0 ? 0 : n - 1) : (this.cur + d + n) % n);
  }

  _paint() {
    for (const el of this.results.querySelectorAll('.es-hit.is-cur, .es-hit.is-sel')) el.classList.remove('is-cur', 'is-sel');
    this.results.querySelector(`.es-hit[data-i="${this.cur}"]`)?.classList.add('is-cur');
    const s = this.results.querySelector(`.es-hit[data-i="${this.sel}"]`);
    s?.classList.add('is-sel');
    s?.scrollIntoView({ block: 'nearest' });
  }

  // The results as a keyboard list (keys.nav.registerList): j / k, gg / G, Enter.
  listController() {
    const set = (i) => {
      if (!this.hits.length) return false;
      this.sel = Math.max(0, Math.min(this.hits.length - 1, i));
      this._paint();
      return true;
    };
    return {
      enter: () => (this.sel >= 0 ? true : set(Math.max(0, this.cur))),
      move: (d) => set(this.sel + d),
      first: () => set(0),
      last: () => set(this.hits.length - 1),
      count: () => this.hits.length,
      activate: () => this.go(this.sel),
    };
  }
}

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createJobs, lineOf, lineParts, ringOf, fileLine, jobError, byteLine } from '../../js/editor/jobs.js';
import { JobList } from '../../js/editor/joblist.js';

const text = (jobs) => jobs.items.map(lineOf);

test('the ring is the oldest running job, and the centre count includes finished jobs', () => {
  const jobs = createJobs();
  assert.deepEqual(ringOf(jobs.items), { progress: 1, count: 0, showCount: false, visible: false });
  const a = jobs.start({ title: 'Opening a' });
  jobs.update(a, { progress: 0.4 });
  assert.equal(ringOf(jobs.items).progress, 0.4);
  assert.equal(ringOf(jobs.items).showCount, false);
  assert.equal(ringOf(jobs.items).count, 1);
  const b = jobs.start({ title: 'Uploading b' });
  jobs.update(b, { progress: 0.9 });
  const ring = ringOf(jobs.items);
  assert.equal(ring.progress, 0.4);
  assert.equal(ring.showCount, true);
  assert.equal(ring.count, 2);
  jobs.finish(a);
  assert.equal(ringOf(jobs.items).progress, 0.9);
  assert.equal(ringOf(jobs.items).count, 2);
  jobs.finish(b);
  assert.equal(ringOf(jobs.items).progress, 1);
  assert.equal(ringOf(jobs.items).visible, true);
});

test('a finished job stays until it is dismissed, and clear leaves a running one', () => {
  const jobs = createJobs();
  const a = jobs.start({ title: 'Opening a' });
  const b = jobs.start({ title: 'Opening b' });
  jobs.finish(a);
  jobs.dismiss(b);
  assert.deepEqual(jobs.items.map((j) => j.id), [a, b]);
  jobs.dismiss(a);
  assert.deepEqual(jobs.items.map((j) => j.id), [b]);
  jobs.finish(b);
  const c = jobs.start({ title: 'Still going' });
  jobs.clearDone();
  assert.deepEqual(jobs.items.map((j) => j.id), [c]);
  assert.equal(jobs.items[0].state, 'run');
});

test('abort says Aborting until the work stops, then Aborted, and finish twice is a no-op', () => {
  const jobs = createJobs();
  let stopped = 0;
  const id = jobs.start({ title: 'Downloading lua/', onAbort: () => { stopped++; } });
  jobs.update(id, { line: 'Adding lua/a.lua · 2 of 9', progress: 0.2 });
  jobs.requestAbort(id);
  assert.equal(stopped, 1);
  assert.equal(jobs.items[0].state, 'aborting');
  assert.equal(text(jobs)[0], 'Adding lua/a.lua · 2 of 9 · Aborting');
  jobs.requestAbort(id);
  assert.equal(stopped, 1);
  jobs.finish(id, { aborted: true });
  assert.equal(jobs.items[0].state, 'aborted');
  assert.equal(text(jobs)[0], 'Adding lua/a.lua · 2 of 9 · Aborted');
  jobs.finish(id);
  assert.equal(jobs.items[0].state, 'aborted');
  jobs.dismiss(id);
  assert.equal(jobs.items.length, 0);
});

test('a failure names the reason and stays until it is dismissed', () => {
  const jobs = createJobs();
  const id = jobs.start({ title: 'Opening anker.m33.dis' });
  jobs.finish(id, { error: 'unreachable' });
  assert.equal(text(jobs)[0], 'Opening anker.m33.dis · unreachable');
  assert.equal(jobError('unreachable'), 'The device did not answer.');
  assert.equal(jobError('disconnected'), 'The device did not answer.');
  assert.equal(jobError('write failed'), 'write failed');
  assert.equal(ringOf(jobs.items).progress, 1);
  jobs.dismiss(id);
  assert.equal(jobs.items.length, 0);
});

test('HTTP and WebSocket transfers name the transport in the tail', () => {
  const http = byteLine('Downloading', 'lua/main.lua', 12 * 1024, 1024 * 1024, 'HTTP');
  const ws = byteLine('Downloading', 'lua/main.lua', 12 * 1024, 1024 * 1024, 'WebSocket');
  assert.equal(lineParts({ line: http, state: 'run' }).head, 'Downloading lua/main.lua');
  assert.equal(lineParts({ line: http, state: 'run' }).tail, 'HTTP · 12.0 KB of 1.00 MB');
  assert.equal(lineParts({ line: ws, state: 'run' }).tail, 'WebSocket · 12.0 KB of 1.00 MB');
  const jobs = createJobs();
  let stopped = 0;
  const id = jobs.start({ title: 'Downloading lua/main.lua', onAbort: () => { stopped++; } });
  jobs.update(id, { line: ws, progress: 0.01 });
  jobs.requestAbort(id);
  assert.equal(stopped, 1);
  assert.equal(jobs.items[0].state, 'aborting');
  assert.equal(lineParts(jobs.items[0]).head, 'Downloading lua/main.lua');
  assert.match(lineParts(jobs.items[0]).tail, /WebSocket/);
});

test('a long name clips and the progress stays in the tail', () => {
  const jobs = createJobs();
  const id = jobs.start({ title: 'Opening anker.m33.dis' });
  jobs.update(id, { line: 'Opening anker.m33.dis · 4.0 KB of 1.50 MB' });
  assert.deepEqual(lineParts(jobs.items[0]), {
    head: 'Opening anker.m33.dis',
    tail: '4.0 KB of 1.50 MB',
  });
  jobs.requestAbort(id);
  assert.deepEqual(lineParts(jobs.items[0]), {
    head: 'Opening anker.m33.dis',
    tail: '4.0 KB of 1.50 MB · Aborting',
  });
  assert.equal(lineOf(jobs.items[0]), 'Opening anker.m33.dis · 4.0 KB of 1.50 MB · Aborting');
});

function el(tag) {
  const node = {
    tagName: String(tag).toUpperCase(),
    nodeType: 1,
    className: '',
    id: '',
    _text: '',
    children: [],
    dataset: {},
    style: {},
    hidden: false,
    disabled: false,
    attributes: {},
    parentNode: null,
    listeners: {},
    setAttribute(k, v) {
      this.attributes[k] = String(v);
      if (k === 'class') this.className = String(v);
      else if (k === 'id') this.id = String(v);
      else if (k === 'hidden') this.hidden = true;
      else if (k.startsWith('data-')) {
        const prop = k.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
        this.dataset[prop] = String(v);
      }
    },
    getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attributes, k) ? this.attributes[k] : null; },
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    removeEventListener() {},
    appendChild(kid) {
      kid.parentNode = this;
      this.children.push(kid);
      return kid;
    },
    querySelector(sel) { return queryAll(this, sel)[0] || null; },
    querySelectorAll(sel) { return queryAll(this, sel); },
    closest(sel) {
      let n = this;
      while (n) {
        if (matches(n, sel)) return n;
        n = n.parentNode;
      }
      return null;
    },
    contains(other) {
      if (!other) return false;
      if (other === this) return true;
      return this.children.some((c) => c.contains?.(other));
    },
    getBoundingClientRect() { return { left: 0, top: 0, width: 240, height: 80 }; },
    setPointerCapture() {},
    focus() {},
    click() {
      const ev = {
        target: this,
        preventDefault() {},
        stopPropagation() { ev.stopped = true; },
      };
      let n = this;
      while (n && !ev.stopped) {
        for (const fn of n.listeners?.click || []) fn(ev);
        n = n.parentNode;
      }
    },
  };
  node.classList = {
    add(...cs) {
      const set = new Set(node.className.split(/\s+/).filter(Boolean));
      for (const c of cs) set.add(c);
      node.className = [...set].join(' ');
    },
    remove(...cs) {
      const drop = new Set(cs);
      node.className = node.className.split(/\s+/).filter((c) => c && !drop.has(c)).join(' ');
    },
    contains(c) { return node.className.split(/\s+/).includes(c); },
  };
  Object.defineProperty(node, 'textContent', {
    get() { return node._text; },
    set(v) { node._text = String(v); node.children = []; },
  });
  Object.defineProperty(node, 'innerHTML', {
    set(html) {
      node.children = [];
      for (const kid of parseHtml(html)) node.appendChild(kid);
    },
    get() { return ''; },
  });
  return node;
}

function matches(node, sel) {
  if (!node || node.nodeType !== 1) return false;
  if (sel.startsWith('.')) return node.className.split(/\s+/).includes(sel.slice(1));
  if (sel.startsWith('[') && sel.endsWith(']')) return node.getAttribute(sel.slice(1, -1)) != null;
  return node.tagName === sel.toUpperCase();
}

function queryAll(root, sel) {
  const out = [];
  const walk = (n) => {
    for (const kid of n.children || []) {
      if (matches(kid, sel)) out.push(kid);
      walk(kid);
    }
  };
  walk(root);
  return out;
}

function decode(s) {
  return s.replace(/&times;/g, '×').replace(/&quot;/g, '"').replace(/&gt;/g, '>')
    .replace(/&lt;/g, '<').replace(/&amp;/g, '&');
}

function parseHtml(html) {
  const roots = [];
  const stack = [{ appendChild(kid) { roots.push(kid); } }];
  const re = /<!--[\s\S]*?-->|<\/([a-zA-Z0-9]+)>|<([a-zA-Z0-9]+)([^>]*)>|([^<]+)/g;
  let m;
  while ((m = re.exec(html))) {
    if (m[0].startsWith('<!--')) continue;
    if (m[1]) {
      if (stack.length > 1) stack.pop();
      continue;
    }
    if (m[2]) {
      const node = el(m[2]);
      const attrs = m[3] || '';
      for (const am of attrs.matchAll(/([:@a-zA-Z0-9_-]+)(?:\s*=\s*"([^"]*)"|\s*=\s*'([^']*)')?/g)) {
        if (am[1] === '/') continue;
        node.setAttribute(am[1], decode(am[2] ?? am[3] ?? ''));
      }
      stack[stack.length - 1].appendChild(node);
      const self = /\/\s*$/.test(attrs) || /^(hr|br|img|input|meta|link)$/i.test(m[2]);
      if (!self) stack.push(node);
      continue;
    }
    const text = decode(m[4]);
    if (text) stack[stack.length - 1]._text = `${stack[stack.length - 1]._text || ''}${text}`;
  }
  return roots;
}

function jobsDom() {
  const btn = el('button');
  btn.id = 'jobs-btn';
  btn.innerHTML = '<svg><circle class="jobs-arc"></circle></svg><span class="jobs-count"></span>';
  globalThis.document = {
    body: el('body'),
    activeElement: null,
    createElement: (tag) => el(tag),
    getElementById: (id) => (id === 'jobs-btn' ? btn : null),
  };
  globalThis.window = {
    matchMedia() { return { matches: false, addEventListener() {} }; },
    addEventListener() {},
    innerWidth: 1200,
    innerHeight: 800,
  };
  const app = { registerDialog: (d) => d };
  return { app };
}

test('progress updates leave the Abort button in place, and a click still aborts', () => {
  const { app } = jobsDom();
  const jobs = createJobs();
  const view = new JobList(app, jobs);
  const a = jobs.start({ title: 'Opening anker.m33.dis' });
  const b = jobs.start({ title: 'Opening other.txt' });
  jobs.update(a, { line: 'Opening anker.m33.dis · 4.0 KB of 1.50 MB', progress: 0.2 });
  jobs.update(b, { line: 'Opening other.txt · 1 KB', progress: 0.1 });
  const buttons = () => [...view.list.querySelectorAll('.jobs-abort')];
  const [first, second] = buttons();
  assert.equal(buttons().length, 2);
  jobs.update(a, { line: 'Opening anker.m33.dis · 8.0 KB of 1.50 MB', progress: 0.5 });
  assert.equal(buttons()[0], first);
  assert.equal(buttons()[1], second);
  assert.equal(view.list.querySelector('.jobs-tail').textContent, '8.0 KB of 1.50 MB');
  first.click();
  assert.equal(jobs.items[0].state, 'aborting');
  assert.equal(view.list.querySelectorAll('.jobs-abort').length, 1);
  assert.match(view.list.querySelector('.jobs-tail').textContent, /Aborting/);
});

test('an archive line names the file, and a large file says how far the stream is', () => {
  assert.equal(fileLine('pex/a.lua', { index: 3, total: 12, size: 400 }), 'Adding pex/a.lua · 3 of 12');
  assert.equal(
    fileLine('pex/anker.m33.dis', { index: 3, total: 12, got: 1200000, size: 4 * 1024 * 1024 }),
    'Adding pex/anker.m33.dis · 1.14 MB of 4.00 MB · 3 of 12',
  );
});

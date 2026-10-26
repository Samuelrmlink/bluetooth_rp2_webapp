import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PEX_PART, SECTOR, PexPartition, partitionModel, partitionActions } from '../../js/editor/pexpart.js';

const live = {
  mounted: true, size: 262144, free: 0, largest: 0, seq: 42, moves: 0, rw_area: 23488,
  modules: [
    { name: 'sample', version: '1.0.0', model: 'pic', offset: 8192, size: 84896, code: 74060, rw: 7640, signed: true, loaded: false, xip_ready: true },
    { name: 'anker', version: '1.0.0', model: 'reloc', offset: 94208, size: 164596, code: 136400, rw: 23488, signed: true, loaded: true, xip_ready: true },
  ],
};

test('the partition tab is one sentinel path', () => {
  assert.equal(PEX_PART, 'pex://partition');
  assert.equal(SECTOR, 4096);
});

test('a full partition: index, then the two modules, and no free cell', () => {
  const m = partitionModel(live);
  assert.equal(m.sectors, 64);
  assert.equal(m.indexBytes, 8192);
  assert.equal(m.used, 262144 - 8192);
  assert.equal(m.free, 0);
  assert.equal(m.fragmented, false);
  assert.equal(m.cells.filter((c) => c.kind === 'index').length, 2);
  assert.equal(m.cells.filter((c) => c.kind === 'free').length, 0);
  assert.equal(m.cells[2].name, 'sample');
  assert.equal(m.cells[22].name, 'sample');
  assert.equal(m.cells[23].name, 'anker');
  assert.equal(m.cells[63].name, 'anker');
  assert.deepEqual(m.modules.map((x) => x.name), ['sample', 'anker']);
});

function domStub() {
  function node(tag) {
    return {
      tag, className: '', textContent: '', hidden: false, children: [], dataset: {},
      disabled: false, type: '', title: '',
      setAttribute() {},
      addEventListener() {},
      classList: { add() {} },
      append(...kids) { this.children.push(...kids); },
      appendChild(kid) { this.children.push(kid); return kid; },
      replaceChildren(...kids) { this.children = kids; },
      querySelector(sel) {
        const want = sel.startsWith('.') ? sel.slice(1) : sel;
        const walk = (kids) => {
          for (const kid of kids || []) {
            if ((kid.className || '').split(/\s+/).includes(want)) return kid;
            const hit = walk(kid.children);
            if (hit) return hit;
          }
          return null;
        };
        return walk(this.children);
      },
    };
  }
  globalThis.document = {
    createElement: node,
    createTextNode(text) { return { text }; },
  };
}

test('Start, Stop, and Uninstall follow the selected installed module', () => {
  assert.deepEqual(partitionActions(null, 'running'), { start: false, stop: false, remove: false, state: '' });
  assert.deepEqual(partitionActions(live.modules[1], ''), { start: true, stop: true, remove: true, state: 'loaded' });
  assert.deepEqual(partitionActions(live.modules[0], ''), { start: true, stop: false, remove: true, state: '' });
  assert.equal(partitionActions(live.modules[1], 'running').start, false);
  assert.equal(partitionActions(live.modules[1], 'running').stop, true);
  assert.equal(partitionActions(live.modules[0], 'stopped').stop, true);

  domStub();
  const surface = document.createElement('div');
  const removed = [];
  const started = [];
  const stopped = [];
  const part = new PexPartition(surface, {
    onRemove: (name) => removed.push(name),
    onStart: (name) => started.push(name),
    onStop: (name) => stopped.push(name),
  });
  part.show(live);
  const root = surface.children[0];
  assert.equal(root.querySelector('.ed-px-start').disabled, true);
  assert.equal(root.querySelector('.ed-px-stop').disabled, true);
  assert.equal(root.querySelector('.ed-px-remove').disabled, true);
  part.show(live, { selected: 'anker', states: { anker: 'running' } });
  const start = root.querySelector('.ed-px-start');
  const stop = root.querySelector('.ed-px-stop');
  const btn = root.querySelector('.ed-px-remove');
  assert.equal(start.disabled, true);
  assert.equal(stop.disabled, false);
  assert.equal(stop.dataset.pex, 'anker');
  assert.equal(btn.disabled, false);
  assert.equal(btn.dataset.pex, 'anker');
  part.onRemove(btn.dataset.pex);
  part.onStop(stop.dataset.pex);
  assert.deepEqual(removed, ['anker']);
  assert.deepEqual(stopped, ['anker']);
  assert.deepEqual(started, []);

  part.show(live, { selected: 'sample' });
  const idleStart = root.querySelector('.ed-px-start');
  assert.equal(idleStart.disabled, false);
  assert.equal(idleStart.dataset.pex, 'sample');
  assert.equal(root.querySelector('.ed-px-stop').disabled, true);
  part.onStart(idleStart.dataset.pex);
  assert.deepEqual(started, ['sample']);
});

test('free space in more than one piece is fragmented', () => {
  const m = partitionModel({
    mounted: true, size: 32768, free: 12288, largest: 4096, rw_area: 0,
    modules: [{ name: 'a', offset: 8192, size: 4096 }],
  });
  assert.equal(m.fragmented, true);
  assert.equal(m.used, 32768 - 8192 - 12288);
  const kinds = m.cells.map((c) => c.kind === 'module' ? c.name : c.kind);
  assert.deepEqual(kinds, ['index', 'index', 'a', 'free', 'free', 'free', 'free', 'free']);
});

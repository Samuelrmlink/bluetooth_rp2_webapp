import { test } from 'node:test';
import assert from 'node:assert/strict';

globalThis.location = { search: '', hostname: 'localhost', port: '', protocol: 'http:' };
globalThis.requestAnimationFrame = (cb) => setTimeout(() => cb(0), 0);

const { EditorWindow } = await import('../../js/windows/editor.js');
const { createJobs } = await import('../../js/editor/jobs.js');

function harness() {
  const ed = Object.create(EditorWindow.prototype);
  ed.buffers = new Map();
  ed.jobs = createJobs();
  ed._said = [];
  ed._say = (m) => { ed._said.push(m); };
  return ed;
}

function textFetch(body) {
  const bytes = new TextEncoder().encode(body);
  globalThis.fetch = async () => ({
    ok: true,
    status: 200,
    headers: { get: (name) => String(name).toLowerCase() === 'content-length' ? String(bytes.length) : null },
    body: new ReadableStream({
      start(controller) {
        controller.enqueue(bytes);
        controller.close();
      },
    }),
  });
}

test('a finished read is kept', async () => {
  textFetch('print(1)\n');
  const ed = harness();
  const b = await ed._load('lua/a.lua');
  assert.equal(b.text, 'print(1)\n');
  assert.equal(ed.buffers.get('lua/a.lua'), b);
  assert.equal(ed._said.length, 0);
});

test('abort after the body, before the buffer is inserted, does not open the file', async () => {
  textFetch('not shown\n');
  const ed = harness();
  globalThis.requestAnimationFrame = (cb) => {
    const id = ed.jobs.items.find((j) => j.state === 'run');
    if (id) ed.jobs.requestAbort(id.id);
    return setTimeout(() => cb(0), 0);
  };
  const b = await ed._load('lua/anker.m33.dis');
  assert.equal(b, null);
  assert.equal(ed.buffers.has('lua/anker.m33.dis'), false);
  assert.equal(ed.jobs.items[0].state, 'aborted');
  assert.deepEqual(ed._said, []);
});

test('abort after an image body does not keep the picture', async () => {
  const bytes = new Uint8Array([1, 2, 3, 4]);
  globalThis.fetch = async () => ({
    ok: true,
    status: 200,
    headers: { get: () => String(bytes.length) },
    body: new ReadableStream({
      start(controller) {
        controller.enqueue(bytes);
        controller.close();
      },
    }),
  });
  const ed = harness();
  globalThis.requestAnimationFrame = (cb) => {
    const id = ed.jobs.items.find((j) => j.state === 'run');
    if (id) ed.jobs.requestAbort(id.id);
    return setTimeout(() => cb(0), 0);
  };
  const b = await ed._load('img/a.png');
  assert.equal(b, null);
  assert.equal(ed.buffers.has('img/a.png'), false);
  assert.equal(ed.jobs.items[0].state, 'aborted');
});

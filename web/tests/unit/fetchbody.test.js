import { test } from 'node:test';
import assert from 'node:assert/strict';

globalThis.location = { search: '', hostname: 'localhost', port: '', protocol: 'http:' };
const { fetchBody, readTextStream } = await import('../../js/fs.js');

function streamOf(parts) {
  let i = 0;
  return new ReadableStream({
    pull(controller) {
      if (i >= parts.length) {
        controller.close();
        return;
      }
      const part = parts[i++];
      if (part.workMs) {
        const end = performance.now() + part.workMs;
        while (performance.now() < end) { /* a buffered chunk still burns the turn */ }
      }
      controller.enqueue(part.bytes);
    },
  });
}

function mockFetch(parts) {
  const length = parts.reduce((n, p) => n + p.bytes.length, 0);
  globalThis.fetch = async () => ({
    ok: true,
    status: 200,
    headers: { get: (name) => String(name).toLowerCase() === 'content-length' ? String(length) : null },
    body: streamOf(parts),
  });
  return length;
}

test('a short buffered read still returns the bytes', async () => {
  mockFetch([{ bytes: new TextEncoder().encode('hello') }]);
  const r = await fetchBody('/api/fs/lua/a.lua');
  assert.equal(r.ok, true);
  assert.equal(r.aborted, undefined);
  assert.equal(new TextDecoder().decode(r.bytes), 'hello');
});

test('abort queued during a short buffered read is applied before the bytes are returned', async () => {
  const parts = Array.from({ length: 5 }, () => ({ bytes: new Uint8Array([1, 2, 3, 4]) }));
  mockFetch(parts);
  const ctl = new AbortController();
  setTimeout(() => ctl.abort(), 0);
  const r = await fetchBody('/api/fs/lua/big.dis', { signal: ctl.signal });
  assert.equal(r.ok, false);
  assert.equal(r.aborted, true);
  assert.equal(r.bytes, undefined);
});

test('a long buffered read yields so abort runs before the body is finished', async () => {
  const total = 40;
  let produced = 0;
  globalThis.fetch = async () => ({
    ok: true,
    status: 200,
    headers: { get: () => String(total) },
    body: new ReadableStream({
      pull(controller) {
        if (produced >= total) {
          controller.close();
          return;
        }
        const end = performance.now() + 2;
        while (performance.now() < end) { /* keep this turn busy */ }
        produced++;
        controller.enqueue(new Uint8Array([produced & 255]));
      },
    }),
  });
  const ctl = new AbortController();
  setTimeout(() => ctl.abort(), 0);
  const r = await fetchBody('/api/fs/lua/anker.m33.dis', { signal: ctl.signal });
  assert.equal(r.ok, false);
  assert.equal(r.aborted, true);
  assert.ok(produced < total, `read all ${produced} chunks before abort`);
});

test('readTextStream decodes a finished body and skips the decode once abort wins', async () => {
  mockFetch([{ bytes: new TextEncoder().encode('hello') }]);
  const ok = await readTextStream('lua/a.lua');
  assert.equal(ok.text, 'hello');

  let decoded = 0;
  const Orig = globalThis.TextDecoder;
  globalThis.TextDecoder = class extends Orig {
    decode(...args) {
      decoded++;
      return super.decode(...args);
    }
  };
  try {
    mockFetch([{ bytes: new TextEncoder().encode('hello') }]);
    const ctl = new AbortController();
    setTimeout(() => ctl.abort(), 0);
    const r = await readTextStream('lua/big.dis', { signal: ctl.signal });
    assert.equal(r.aborted, true);
    assert.equal(r.text, undefined);
    assert.equal(decoded, 0);
  } finally {
    globalThis.TextDecoder = Orig;
  }
});

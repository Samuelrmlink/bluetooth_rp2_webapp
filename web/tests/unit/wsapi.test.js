import test from 'node:test';
import assert from 'node:assert/strict';
import { WsApi } from '../../js/wsapi.js';

/* A stand-in for the browser's WebSocket: enough to drive the client's
 * correlation logic without a network or a DOM. */
class FakeWS {
  constructor() { this.sent = []; this.readyState = 1; FakeWS.last = this; }
  send(s) { this.sent.push(JSON.parse(s)); }
  close() { this.onclose?.(); }
  deliver(obj) { this.onmessage?.({ data: JSON.stringify(obj) }); }
}

/* Stub the global WebSocket and drive connect() for real, so the test
 * exercises the handlers the client actually installs rather than a
 * hand-wired subset -- the close path in particular only exists there. */
function wired({ reconnect = false } = {}) {
  globalThis.WebSocket = FakeWS;
  const api = new WsApi('ws://x/ws');
  api.timeoutMs = 300;
  if (!reconnect) api.connect = Object.assign(api.connect.bind(api), {});
  api.connect();
  const fake = FakeWS.last;
  fake.onopen();
  return { api, fake };
}

test('node is the first field, and a local call omits it', async () => {
  const { api, fake } = wired();
  const remote = api.call('fs.read', { path: 'sd/log/c5.log', offset: 0, len: 480 }, 'bluetooth-rp2-webapp');
  const sent = fake.sent[0];
  assert.equal(Object.keys(sent)[0], 'node');
  assert.equal(sent.node, 'bluetooth-rp2-webapp');
  assert.equal(sent.op, 'fs.read');
  fake.deliver({ node: 'bluetooth-rp2-webapp', id: sent.id, ok: true, result: { n: 0, eof: true } });
  assert.deepEqual(await remote, { n: 0, eof: true });
  const local = api.call('sys.info', {});
  assert.equal(fake.sent[1].node, undefined);
  assert.equal(Object.keys(fake.sent[1])[0], 'id');
  fake.deliver({ id: fake.sent[1].id, ok: true, result: {} });
  assert.deepEqual(await local, {});
});

test('a reply resolves the matching call, by id', async () => {
  const { api, fake } = wired();
  const p = api.call('sys.info');
  const sent = fake.sent[0];
  assert.equal(sent.op, 'sys.info');
  fake.deliver({ id: sent.id, ok: true, result: { firmware: 'x' } });
  assert.deepEqual(await p, { firmware: 'x' });
});

test('replies are matched to their own call, whatever order they arrive in', async () => {
  const { api, fake } = wired();
  const a = api.call('one');
  const b = api.call('two');
  fake.deliver({ id: fake.sent[1].id, ok: true, result: 2 });
  fake.deliver({ id: fake.sent[0].id, ok: true, result: 1 });
  assert.equal(await a, 1);
  assert.equal(await b, 2);
});

test('an error reply rejects with the code intact', async () => {
  const { api, fake } = wired();
  const p = api.call('nope');
  fake.deliver({ id: fake.sent[0].id, ok: false, error: { code: 'unknown_op', message: 'nope' } });
  await assert.rejects(p, (e) => e.code === 'unknown_op');
});

test('events go to listeners and never to pending calls', async () => {
  const { api, fake } = wired();
  const seen = [];
  api.on('scan.device', d => seen.push(d));
  const p = api.call('sys.info');
  fake.deliver({ evt: 'scan.device', data: { addr: 'A' } });
  fake.deliver({ id: fake.sent[0].id, ok: true, result: 'done' });
  assert.equal(await p, 'done');
  assert.deepEqual(seen, [{ addr: 'A' }]);
});

test('the wildcard listener sees the topic too', () => {
  const { api, fake } = wired();
  const seen = [];
  api.on('*', m => seen.push(m.evt));
  fake.deliver({ evt: 'log', data: {} });
  assert.deepEqual(seen, ['log']);
});

test('a peer event stays on that board and off the local listeners', () => {
  const { api, fake } = wired();
  const local = [];
  const wild = [];
  const peer = [];
  const peerWild = [];
  api.on('fs.changed', (d) => local.push(d));
  api.on('*', (m) => wild.push(m));
  const view = api.scope('ESP32C5-3844be0a31f0');
  view.on('fs.changed', (d) => peer.push(d));
  view.on('*', (m) => peerWild.push(m));
  fake.deliver({ node: 'ESP32C5-3844be0a31f0', evt: 'fs.changed', data: { path: 'lua/main.lua' } });
  fake.deliver({ evt: 'fs.changed', data: { path: 'local.lua' } });
  assert.deepEqual(local, [{ path: 'local.lua' }]);
  assert.deepEqual(wild, [{ evt: 'fs.changed', data: { path: 'local.lua' } }]);
  assert.deepEqual(peer, [{ path: 'lua/main.lua' }]);
  assert.equal(peerWild.length, 1);
  assert.equal(peerWild[0].node, 'ESP32C5-3844be0a31f0');
  assert.equal(peerWild[0].evt, 'fs.changed');
  assert.deepEqual(peerWild[0].data, { path: 'lua/main.lua' });
});

test('a scope sends node first on the page socket', async () => {
  const { api, fake } = wired();
  const view = api.scope('ESP32C5-3844be0a31f0');
  const pending = view.call('sys.info');
  const sent = fake.sent[0];
  assert.equal(Object.keys(sent)[0], 'node');
  assert.equal(sent.node, 'ESP32C5-3844be0a31f0');
  assert.equal(sent.op, 'sys.info');
  fake.deliver({ node: sent.node, id: sent.id, ok: true, result: { board: 'esp32c5' } });
  assert.deepEqual(await pending, { board: 'esp32c5' });
  view.close();
  assert.equal(api.connected, true);
});

test('a pinned socket names its board and ignores other boards', async () => {
  globalThis.WebSocket = FakeWS;
  const api = new WsApi('ws://x/ws', 'ESP32C5-3844be0a31f0');
  api.connect();
  const fake = FakeWS.last;
  fake.onopen();
  const seen = [];
  api.on('fs.changed', (d) => seen.push(d));
  const pending = api.call('sys.info');
  const sent = fake.sent[0];
  assert.equal(Object.keys(sent)[0], 'node');
  assert.equal(sent.node, 'ESP32C5-3844be0a31f0');
  fake.deliver({ evt: 'fs.changed', data: { path: 'local' } });
  fake.deliver({ node: 'other', evt: 'fs.changed', data: { path: 'other' } });
  fake.deliver({ node: 'ESP32C5-3844be0a31f0', evt: 'fs.changed', data: { path: 'peer' } });
  assert.deepEqual(seen, [{ path: 'peer' }]);
  fake.deliver({ node: sent.node, id: sent.id, ok: true, result: { board: 'esp32c5' } });
  assert.deepEqual(await pending, { board: 'esp32c5' });
});

test('a close rejects everything in flight rather than hanging', async () => {
  const { api, fake } = wired();
  const p = api.call('slow');
  api.connect = () => {};          // do not let the reconnect timer fire
  fake.close();
  await assert.rejects(p, /disconnected/);
});

test('park drops the socket and does not dial again until unpark', async () => {
  let n = 0;
  globalThis.WebSocket = class extends FakeWS {
    constructor() { super(); n++; FakeWS.last = this; }
  };
  const api = new WsApi('ws://x/ws');
  api.connect();
  FakeWS.last.onopen();
  assert.equal(api.connected, true);
  const p = api.call('slow');
  api.park();
  await assert.rejects(p, /disconnected/);
  assert.equal(api.connected, false);
  api.connect();
  assert.equal(n, 1);
  api.unpark();
  assert.equal(n, 2);
  assert.equal(FakeWS.last.readyState, 1);
});

test('park cancels a reconnect that was already scheduled', async () => {
  let n = 0;
  globalThis.WebSocket = class extends FakeWS {
    constructor() { super(); n++; FakeWS.last = this; }
  };
  const api = new WsApi('ws://x/ws');
  api._backoff = 30;
  api.connect();
  FakeWS.last.onopen();
  FakeWS.last.onclose();
  api.park();
  await new Promise((resolve) => setTimeout(resolve, 80));
  assert.equal(n, 1);
  api.unpark();
  assert.equal(n, 2);
});

test('off() removes a listener', () => {
  const { api, fake } = wired();
  let n = 0;
  const fn = () => n++;
  api.on('log', fn);
  fake.deliver({ evt: 'log', data: {} });
  api.off('log', fn);
  fake.deliver({ evt: 'log', data: {} });
  assert.equal(n, 1);
});

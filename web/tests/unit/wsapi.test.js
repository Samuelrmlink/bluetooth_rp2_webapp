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

test('a close rejects everything in flight rather than hanging', async () => {
  const { api, fake } = wired();
  const p = api.call('slow');
  api.connect = () => {};          // do not let the reconnect timer fire
  fake.close();
  await assert.rejects(p, /disconnected/);
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

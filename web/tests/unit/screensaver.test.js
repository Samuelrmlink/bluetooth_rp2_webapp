import { test } from 'node:test';
import assert from 'node:assert/strict';
import { crc32, packets, packetLength, defaultId, upload, UploadError } from '../../js/devices/anker-scp/screensaver.js';

test('crc32 is zlib\'s (the charger checks the image against it)', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xCBF43926);
  assert.equal(crc32(new Uint8Array(0)), 0);
});

test('packets: fixed length, the last zero-padded', () => {
  const p = packets(Uint8Array.from({ length: 30700 }, (_, i) => i & 0xFF), 156);
  assert.equal(p.length, 197);                         // the app's own count for 30700 B
  assert.equal(p[196].length, 156);
  assert.equal(p[196][30700 - 196 * 156], 0);          // padding after the end
});

test('packet length from the MTU: one data frame per ATT write', () => {
  assert.equal(packetLength(255), 156);
  assert.equal(packetLength(185), 145);
  assert.equal(packetLength(23), 0);
});

test('default id is stable per picture', () => {
  assert.equal(defaultId(0xc01ff4d0), defaultId(0xc01ff4d0));
  assert.ok(defaultId(0xffffffff) >= 100000 && defaultId(0xffffffff) < 1000000);
});

// A charger in miniature: answers select / begin / window acks as events.
function fakeCharger({ cached = false, drop = -1, busyOnce = false } = {}) {
  const waiters = [];
  const got = new Set();
  let count = 0, dropped = false, busy = busyOnce;
  const emit = (ev) => setTimeout(() => {
    const i = waiters.findIndex((w) => w.match(ev));
    if (i >= 0) waiters.splice(i, 1)[0].resolve(ev);
  }, 0);
  return {
    got,
    io: {
      wait: (match, ms) => new Promise((resolve, reject) => {
        const w = { match, resolve };
        waiters.push(w);
        setTimeout(() => { const i = waiters.indexOf(w); if (i >= 0) { waiters.splice(i, 1); reject(new UploadError('timeout')); } }, ms);
      }),
      call: async (p) => {
        if (busy) { busy = false; return { error: 'busy' }; }
        if (p.step === 'select') emit({ step: 'select', status: cached ? 0 : 0x11 });
        else if (p.step === 'begin') { count = p.count; emit({ step: 'begin', status: 0 }); }
        else {
          assert.equal(p.a.length + p.b.length, 312);     // two hex halves of 156 B
          if (p.n === drop && !dropped) dropped = true;   // lost on the air once
          else got.add(p.n);
          if (p.ack) {
            let have = 0;
            while (got.has(have)) have++;
            emit({ step: 'data', n: p.n, status: have === count ? 0x10 : 0, received: have });
          }
        }
        return { queued: true };
      },
    },
  };
}

const jpeg = Uint8Array.from({ length: 30700 }, (_, i) => (i * 7) & 0xFF);

test('upload: select, begin, 20 windows, complete', async () => {
  const c = fakeCharger();
  const phases = [];
  assert.equal(await upload({ jpeg, id: 5 }, c.io, { onProgress: (p) => phases.push(p.phase) }), 'sent');
  assert.equal(c.got.size, 197);
  assert.deepEqual([phases[0], phases.at(-1)], ['select', 'done']);
});

test('upload: a picture the charger already has is only selected', async () => {
  const c = fakeCharger({ cached: true });
  assert.equal(await upload({ jpeg, id: 5 }, c.io), 'cached');
  assert.equal(c.got.size, 0);
});

test('upload: resumes from what the charger says it received', async () => {
  const c = fakeCharger({ drop: 13 });
  assert.equal(await upload({ jpeg, id: 5 }, c.io), 'sent');
  assert.equal(c.got.size, 197);                       // 13 was sent again
});

test('upload: a busy queue is waited out', async () => {
  const c = fakeCharger({ busyOnce: true });
  assert.equal(await upload({ jpeg, id: 5 }, c.io), 'sent');
});

test('upload: cancel stops it', async () => {
  const c = fakeCharger();
  const ac = new AbortController();
  ac.abort();
  await assert.rejects(upload({ jpeg, id: 5 }, c.io, { signal: ac.signal }), /cancelled/);
});

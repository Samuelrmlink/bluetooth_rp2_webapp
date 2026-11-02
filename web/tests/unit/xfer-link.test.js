import { test } from 'node:test';
import assert from 'node:assert/strict';
import { allowBulk, pumpLink, b64encode, b64decode, FILE_RAW, LINK_SND_BUF, FILE_FRAME } from '../../js/xfer/link.js';
import { readChunks, writeChunks } from '../../js/xfer/wsfile.js';

const MIB = 2 * 1024 * 1024;

test('the reserve matches the firmware gate', () => {
  assert.equal(allowBulk(LINK_SND_BUF, LINK_SND_BUF, FILE_FRAME), true);
  assert.equal(allowBulk(LINK_SND_BUF, 4793, FILE_FRAME), true);
  assert.equal(allowBulk(LINK_SND_BUF, 4792, FILE_FRAME), false);
  assert.equal(allowBulk(LINK_SND_BUF, 0, FILE_FRAME), false);
  assert.equal(allowBulk(0, 0, FILE_FRAME), false);
});

test('a 2 MiB transfer does not drop or delay telemetry', () => {
  const r = pumpLink({ size: MIB });
  assert.equal(r.left, 0);
  assert.equal(r.rawSent, MIB);
  assert.equal(r.telDrop, 0);
  assert.equal(r.late, 0);
  assert.ok(r.telSent >= Math.floor(MIB / FILE_RAW));
});

test('stopping near 200 KB leaves the rest and still drops no telemetry', () => {
  const r = pumpLink({ size: MIB, stopAt: 200000 });
  assert.ok(r.rawSent >= 200000);
  assert.ok(r.left > 0);
  assert.equal(r.telDrop, 0);
  assert.equal(r.late, 0);
  assert.equal(r.aborted, true);
});

test('filling the buffer with file frames drops telemetry', () => {
  const r = pumpLink({ size: MIB, greedy: true });
  assert.ok(r.telDrop > 0);
});

test('base64 round trip of one chunk', () => {
  const raw = Uint8Array.from({ length: FILE_RAW }, (_, i) => (i * 17 + 3) & 255);
  const back = b64decode(b64encode(raw));
  assert.equal(back.length, raw.length);
  assert.equal(back.every((b, i) => b === raw[i]), true);
});

test('fs.read retries busy without moving the offset, and abort stops', async () => {
  const seen = [];
  let n = 0;
  const call = async (op, params) => {
    seen.push(params.offset);
    n++;
    if (params.offset === 0 && n < 3) {
      throw Object.assign(new Error('telemetry has the link'), { code: 'busy' });
    }
    const left = 1000 - params.offset;
    const len = Math.min(FILE_RAW, left);
    const bytes = new Uint8Array(len).fill(7);
    return { data: b64encode(bytes), size: 1000, n: len, eof: params.offset + len >= 1000, offset: params.offset };
  };
  const r = await readChunks(call, 'lua/a.bin');
  assert.equal(r.ok, true);
  assert.equal(r.bytes.length, 1000);
  assert.equal(seen.filter((o) => o === 0).length >= 3, true);
  const ac = new AbortController();
  let calls = 0;
  const slow = async (op, params) => {
    calls++;
    if (calls === 2) ac.abort();
    const len = Math.min(FILE_RAW, 5000 - params.offset);
    return { data: b64encode(new Uint8Array(len)), size: 5000, n: len, eof: false, offset: params.offset };
  };
  const stopped = await readChunks(slow, 'lua/b.bin', { signal: ac.signal });
  assert.equal(stopped.aborted, true);
  assert.ok(stopped.got < 5000);
});

test('fs.write sends node first only when a peer is named, and retries busy', async () => {
  const sent = [];
  let busy = true;
  const call = async (op, params, node) => {
    sent.push({ op, offset: params.offset, node, truncate: params.truncate });
    if (busy) {
      busy = false;
      throw Object.assign(new Error('telemetry has the link'), { code: 'busy' });
    }
    return { n: 4, offset: params.offset, size: 4 };
  };
  const r = await writeChunks(call, 'sd/log/c5.log', new Uint8Array([1, 2, 3, 4]), { node: 'bluetooth-rp2-webapp' });
  assert.equal(r.ok, true);
  assert.equal(r.got, 4);
  assert.equal(sent[0].offset, 0);
  assert.equal(sent[1].offset, 0);
  assert.equal(sent[0].node, 'bluetooth-rp2-webapp');
  assert.equal(sent[0].truncate, true);
});

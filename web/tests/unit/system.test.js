import { test } from 'node:test';
import assert from 'node:assert/strict';
import { heapRows, fetchArena, restartText, REBOOT_CONFIRM } from '../../js/settings/system.js';

const heap = {
  arena: { bytes: 163840, used: 18016, peak: 133768, free_bytes: 145824, largest_free: 55120,
           free_blocks: 9, alloc_fails: 2, alloc_fail_last: 192 },
  objects: { table: { n: 10, bytes: 3000 }, short_string: { n: 50, bytes: 5000 }, upvalue: { n: 0, bytes: 0 }, total: 8000 },
  tables: [{ name: 'registry', bytes: 2048 }],
  threads: [],
};

test('heap rows: used, fragmentation, failures, objects by size', () => {
  const rows = Object.fromEntries(heapRows(heap));
  assert.equal(rows.Used, '17.6 KB of 160.0 KB (peak 130.6 KB)');
  assert.match(rows['Largest free block'], /^53\.8 KB of 142\.4 KB free, in 9 pieces$/);
  assert.equal(rows['Failed allocations'], '2 (last 192 B)');
  assert.ok(rows.Objects.startsWith('short string 4.9 KB (50)'), rows.Objects);   // largest first, empty types left out
  assert.equal(rows['Largest tables'], 'registry 2.0 KB');
  assert.equal(rows.Coroutines, 'none');
});

test('the arena is fetched 1 KB at a time, in order', async () => {
  const calls = [];
  const ws = { call: async (op, p) => { calls.push(p.offset); return { size: 2500, base: 7, header: 16, hex: 'ab'.repeat(Math.min(1024, 2500 - p.offset)) }; } };
  const a = await fetchArena(ws);
  assert.deepEqual(calls, [0, 1024, 2048]);
  assert.equal(a.hex.length, 5000);
  assert.deepEqual([a.base, a.header, a.size], [7, 16, 2500]);
});

test('reboot confirm says a normal boot is what starts the list', () => {
  assert.match(REBOOT_CONFIRM, /normal one/);
  assert.match(REBOOT_CONFIRM, /watchdog reboot/);
  assert.match(REBOOT_CONFIRM, /double reset/);
});

test('what a VM rebuild did, in words', () => {
  assert.equal(restartText({ stopped: 0, resumed: 0, before: 17560, after: 15640, largest_free_before: 61640, largest_free: 144000 }),
    'Lua VM rebuilt: 0 script(s) stopped; arena 17.1 KB → 15.3 KB, largest free block 60.2 KB → 140.6 KB.');
  assert.match(restartText({ stopped: 2, resumed: 2, before: 1, after: 1 }), /2 script\(s\) stopped and started again/);
});

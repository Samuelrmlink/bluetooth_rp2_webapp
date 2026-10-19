// Settings › SD card format arithmetic (js/sd-format.js), which mirrors
// src/log/sd_api.c.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GIB, defaultFs, defaultCluster, alignSectors, derive, fmtBytes, fmtCluster, CLUSTERS } from '../../js/settings/sd-format.js';

const CARD_64 = 62_537_072_640; // the bench card's CSD size
const CARD_32 = 31_914_983_424;
const CARD_8 = 7_948_206_080;
const CARD_256M = 255_852_544;

test('the default filesystem follows the SD classes: exFAT above 32 GiB', () => {
  assert.equal(defaultFs(CARD_64), 'exfat');
  assert.equal(defaultFs(CARD_32), 'fat32'); // a "32 GB" card is under 32 GiB
  assert.equal(defaultFs(CARD_8), 'fat32');
  assert.equal(defaultFs(33 * GIB), 'exfat');
});

test('default clusters: the SD formatter for exFAT, by size for FAT32', () => {
  assert.equal(defaultCluster('exfat', CARD_64), 131072);
  assert.equal(defaultCluster('exfat', CARD_32), 32768);
  assert.equal(defaultCluster('exfat', 512 * GIB), 262144);
  assert.equal(defaultCluster('fat32', CARD_64), 32768);
  assert.equal(defaultCluster('fat32', 2 * GIB), 16384);
  assert.equal(defaultCluster('fat32', 1e9), 8192);
  assert.equal(defaultCluster('fat32', CARD_256M), 4096);
  assert.equal(alignSectors(CARD_32), 8192);
  assert.equal(alignSectors(CARD_64), 32768);
});

test('every default is a choice FatFs takes, either filesystem, any card from 1 GB', () => {
  // Below ~260 MB even 4 KB clusters are too few for FAT32: said, not hidden.
  assert.match(derive('fat32', 4096, CARD_256M).why, /too few clusters/);
  for (const bytes of [1e9, CARD_8, CARD_32, CARD_64, 512 * GIB]) {
    for (const fs of ['fat32', 'exfat']) {
      const d = derive(fs, defaultCluster(fs, bytes), bytes);
      assert.ok(d.ok, `${fs} on ${bytes}: ${d.why}`);
      assert.ok(CLUSTERS[fs].includes(defaultCluster(fs, bytes)));
    }
  }
});

test('FAT32 needs enough clusters: a big cluster on a small card is refused', () => {
  const d = derive('fat32', 65536, CARD_256M);
  assert.equal(d.ok, false);
  assert.match(d.why, /too few clusters/);
  assert.equal(derive('fat32', 65536, CARD_64).ok, true);
  assert.equal(derive('exfat', 3000, CARD_64).ok, false);
});

test('what a format writes: FAT32 two FATs, exFAT one plus a bitmap', () => {
  const f = derive('fat32', 32768, CARD_64);
  // 62.5 GB / 32 KB = ~1.9 M clusters * 4 B * 2 FATs = ~15 MB
  assert.ok(f.fatBytes > 14e6 && f.fatBytes < 16e6, String(f.fatBytes));
  const e = derive('exfat', 131072, CARD_64);
  assert.ok(e.fatBytes > 1.8e6 && e.fatBytes < 2.0e6, String(e.fatBytes));
  assert.ok(e.writeSectors < f.writeSectors / 5);
  assert.ok(e.estSeconds < f.estSeconds);
});

test('formatting of sizes', () => {
  assert.equal(fmtBytes(CARD_64), '62.5 GB');
  assert.equal(fmtBytes(512), '512 B');
  assert.equal(fmtCluster(131072), '128 KB');
  assert.equal(fmtCluster(1048576), '1 MB');
});

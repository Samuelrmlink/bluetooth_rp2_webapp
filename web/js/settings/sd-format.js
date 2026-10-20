// From km003c_rp2_webapp (web/js/sd-format.js). The arithmetic behind
// Settings › SD card › Format: the defaults a card's
// size implies, and what a filesystem/cluster choice comes to. Mirrors
// src/sd/sd_api.c (sd_default_*, sd_cluster_ok, format_total) so the page
// greys out exactly what the board would refuse; the board checks again.

export const GIB = 1024 ** 3;

export const CLUSTERS = {
  fat32: [4096, 8192, 16384, 32768, 65536],
  exfat: [4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576],
};

// SDXC (over 32 GiB) is exFAT by the SD spec; SDHC and smaller, FAT32.
export function defaultFs(bytes) {
  return bytes > 32 * GIB ? 'exfat' : 'fat32';
}

export function defaultCluster(fs, bytes) {
  if (fs === 'exfat') return bytes <= 32 * GIB ? 32768 : bytes <= 256 * GIB ? 131072 : 262144;
  // 32 KB (the SD formatter's SDHC choice) from 4 GiB; below that the
  // largest that still leaves FAT32 its 65,525+ clusters.
  if (bytes >= 4 * GIB) return 32768;
  let c = 32768;
  while (c > 4096 && bytes / c < 70000) c /= 2;
  return c;
}

// Data-area alignment, in sectors: the card's allocation unit.
export function alignSectors(bytes) {
  return bytes <= 32 * GIB ? 8192 : 32768;
}

// What a choice comes to: cluster count, FAT size, the sectors the format
// writes, and whether FatFs would take it (and why not).
export function derive(fs, cluster, bytes) {
  const sectors = Math.floor(bytes / 512);
  const align = alignSectors(bytes);
  const data = sectors > align * 2 ? sectors - align : 0;
  const spc = cluster / 512;
  const clusters = Math.floor(data / spc);
  let ok = true;
  let why = '';
  const hi = fs === 'exfat' ? 1048576 : 65536;
  if (cluster < 4096 || cluster > hi || (cluster & (cluster - 1))) {
    ok = false;
    why = fs === 'exfat' ? 'exFAT clusters are 4 KB to 1 MB' : 'FAT32 clusters are 4 KB to 64 KB';
  } else if (fs === 'fat32' && clusters < 65526 + 1024) {
    ok = false;
    why = 'too few clusters for FAT32 on this card — choose a smaller cluster';
  } else if (fs === 'fat32' && clusters > 0x0ffffff5) {
    ok = false;
    why = 'too many clusters for FAT32 — choose a larger cluster';
  } else if (fs === 'exfat' && (clusters < 16 || clusters > 0x7ffffffd)) {
    ok = false;
    why = 'cluster count out of range for exFAT';
  }
  const all = Math.floor(sectors / spc);
  const fatSectors = Math.ceil((all * 4) / 512);
  let write;
  if (fs === 'fat32') write = 1 + 32 + 2 * fatSectors + spc;
  else write = 1 + 24 + fatSectors + Math.ceil(all / 8 / 512) + 12 + spc;
  return {
    clusters,
    fatBytes: fatSectors * 512 * (fs === 'fat32' ? 2 : 1),
    writeSectors: write,
    // ~400 KB/s of metadata is what a cheap card manages over SPI while it
    // also erases; good enough for "about a minute" vs "a few seconds".
    estSeconds: Math.max(1, Math.round((write * 512) / 400000)),
    ok,
    why,
  };
}

// Card capacities the way the label on the card counts them.
export function fmtBytes(n) {
  if (n == null || !Number.isFinite(n)) return '—';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  let v = n;
  while (v >= 1000 && i < u.length - 1) {
    v /= 1000;
    i++;
  }
  return `${i ? v.toFixed(v >= 100 ? 0 : 1) : v} ${u[i]}`;
}

// Cluster sizes the way formatters name them (binary).
export function fmtCluster(n) {
  return n >= 1048576 ? `${n / 1048576} MB` : `${n / 1024} KB`;
}

export function fmtDuration(s) {
  if (s < 60) return `${s} s`;
  const m = Math.round(s / 60);
  return `${m} min`;
}

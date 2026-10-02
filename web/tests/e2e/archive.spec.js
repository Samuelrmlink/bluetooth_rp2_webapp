// Folders as tar archives (download, restore with a preview), compressed
// files edited as text, .bak for web files, and the image preview (fw 1.95).
import { test, expect } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, chmodSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ready } from './helpers.js';
import { parseTar } from '../../js/tar.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');
const dlg = (page) => page.locator('#restore-dlg');
const ed = (page, fn, arg) => page.evaluate(([f, a]) => new Function('w', 'a', `return (${f})(w, a)`)(window.__btgwApp.windows.get('editor').win, a), [fn.toString(), arg]);

async function editor(page, open = 'lua/anker.lua') {
  await ready(page);
  await page.evaluate((p) => window.__btgwApp.openFile(p), open);
  await page.waitForFunction(() => {
    const v = window.__btgwApp._layout.version;
    const s = window.__settle || (window.__settle = { v, t: performance.now() });
    if (s.v !== v) { s.v = v; s.t = performance.now(); }
    return performance.now() - s.t > 1500;
  }, null, { polling: 100 });
}

// The stored bytes of a file, through the archive route (a plain GET of a
// .gz is unpacked by the browser, by design).
async function stored(page, path) {
  const folder = path.split('/').slice(0, -1).join('/');
  const r = await page.request.get(`/api/tar/${folder}`);
  const t = parseTar(new Uint8Array(await r.body()));
  const name = path.split('/').slice(folder.split('/').length - 1).join('/');
  return t.entries.find((e) => e.path === name)?.data;
}

// An archive as a Linux PC makes one: GNU tar, odd permissions, an empty
// folder, a symlink.
function linuxTar(files, { dirs = [], link = null } = {}) {
  const root = mkdtempSync(join(tmpdir(), 'btgw-e2e-'));
  try {
    for (const d of dirs) mkdirSync(join(root, d), { recursive: true });
    for (const [p, text, mode] of files) {
      mkdirSync(join(root, p, '..'), { recursive: true });
      writeFileSync(join(root, p), text);
      if (mode) chmodSync(join(root, p), mode);
    }
    if (link) execFileSync('ln', ['-s', link[1], join(root, link[0])]);
    const top = [...new Set([...files.map((f) => f[0]), ...dirs].map((p) => p.split('/')[0]))];
    return execFileSync('tar', ['--format=gnu', '-cf', '-', '-C', root, ...top]);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

test('folder menu: Download as .tar is the device\'s archive of that folder', async ({ page }) => {
  await editor(page);
  await row(page, 'lua').click({ button: 'right' });
  const [dl] = await Promise.all([
    page.waitForEvent('download'),
    menu(page).getByRole('button', { name: 'Download as .tar' }).click(),
  ]);
  expect(dl.suggestedFilename()).toBe('lua.tar');
  const t = parseTar(readFileSync(await dl.path()));
  expect(t.entries[0]).toEqual({ path: 'lua', dir: true });
  expect(t.entries.map((e) => e.path)).toContain('lua/profiles/anker/run.lua');
});

test('Upload archive here: a Linux tar, permissions ignored, a preview, then written', async ({ page }) => {
  await editor(page);
  await row(page, 'lua').click({ button: 'right' });
  await menu(page).getByRole('button', { name: /Upload archive here/ }).click();
  const tar = linuxTar([
    ['pkg/run.lua', 'print("from a pc")\n', 0o755],
    ['pkg/private.lua', 'return 7\n', 0o600],
  ], { dirs: ['pkg/empty'], link: ['pkg/link.lua', 'run.lua'] });
  await page.locator('.ed-tar-input').setInputFiles({ name: 'pkg.tar', mimeType: 'application/x-tar', buffer: tar });
  await expect(dlg(page)).toBeVisible();
  await expect(dlg(page).locator('.rs-summary')).toContainText('2 files');
  await expect(dlg(page).locator('.rs-summary')).toContainText('into lua/');
  await expect(dlg(page).locator('.rs-summary')).toContainText('1 empty folder');
  await expect(dlg(page).locator('.rs-lists')).toContainText('pkg/link.lua — symlink (not a file)');
  await dlg(page).getByRole('button', { name: 'Restore 2 files' }).click();
  await expect(dlg(page).locator('.rs-status')).toHaveText('Restored 2 files and 1 folder.');
  expect(await (await page.request.get('/api/fs/lua/pkg/private.lua')).text()).toBe('return 7\n');
  expect(await (await page.request.get('/api/fs/lua/pkg/run.lua')).text()).toBe('print("from a pc")\n');
  const empty = await (await page.request.get('/api/fs/lua/pkg/empty/')).json();
  expect(empty.files).toEqual([]);
});

test('Restore archive… at the root: overwrites are named first, cache/ skipped, nothing deleted', async ({ page }) => {
  await editor(page);
  const before = await (await page.request.get('/api/fs/lua/pinecil.lua')).text();
  const tar = linuxTar([['lua/anker.lua', '-- restored\n'], ['cache/x.luac', 'junk'], ['lua/new.lua', 'return 1\n']]);
  await page.locator('.ed-b-more').click();
  await page.locator('.ed-more-menu [data-act="restore"]').click();
  await page.locator('.ed-tar-input').setInputFiles({ name: 'btgw.tar', mimeType: 'application/x-tar', buffer: tar });
  await expect(dlg(page).locator('.rs-summary')).toContainText('1 new, 1 overwritten');
  await expect(dlg(page).locator('.rs-list.rs-warn')).toContainText('lua/anker.lua');
  await expect(dlg(page).locator('.rs-lists')).toContainText('cache/x.luac — cache/');
  await dlg(page).getByRole('button', { name: 'Restore 2 files' }).click();
  await expect(dlg(page).locator('.rs-status')).toHaveText('Restored 2 files.');
  expect(await (await page.request.get('/api/fs/lua/anker.lua')).text()).toBe('-- restored\n');
  expect(await (await page.request.get('/api/fs/lua/pinecil.lua')).text()).toBe(before);   // untouched
  expect((await page.request.get('/api/fs/cache/x.luac')).status()).toBe(404);
});

test('a .tar.gz or a non-tar is refused before anything is written', async ({ page }) => {
  await editor(page);
  await page.locator('.ed-b-more').click();
  await page.locator('.ed-more-menu [data-act="restore"]').click();
  await page.locator('.ed-tar-input').setInputFiles({ name: 'x.tar.gz', mimeType: 'application/gzip', buffer: Buffer.from([0x1f, 0x8b, 8, 0]) });
  await expect(dlg(page).locator('.rs-summary')).toContainText('only plain .tar');
  await expect(dlg(page).getByRole('button', { name: /^Restore/ })).toBeHidden();
});

test('a .gz web file opens as text, saves compressed, and keeps a .bak to restore', async ({ page }) => {
  await editor(page, 'www/demo.js.gz');
  await expect(page.locator('.ed-surface textarea')).toHaveValue(/answer = 42/);
  await ed(page, (w) => { w.core.setValue(w.core.value.replace('42', '43'), 'text'); w._edited(w.core.value); });
  await page.keyboard.press('Control+KeyS');
  await expect(page.locator('.ed-notice')).toContainText('compressed');
  await expect(page.locator('.ed-notice')).toContainText('demo.js.gz.bak');
  const bytes = await stored(page, 'www/demo.js.gz');
  expect([bytes[0], bytes[1]]).toEqual([0x1f, 0x8b]);                   // stored gzip
  expect(await (await page.request.get('/api/fs/www/demo.js.gz')).text()).toContain('answer = 43');
  // Restore .bak: the old one back, the new one becomes the .bak
  await row(page, 'www/demo.js.gz').click({ button: 'right' });
  page.once('dialog', (d) => d.accept());
  await menu(page).getByRole('button', { name: /Restore \.bak/ }).click();
  await expect(page.locator('.ed-notice')).toContainText('Restored www/demo.js.gz');
  expect(await (await page.request.get('/api/fs/www/demo.js.gz')).text()).toContain('answer = 42');
  await expect(page.locator('.ed-surface textarea')).toHaveValue(/answer = 42/);
});

test('Store compressed, then uncompressed: a Lua module round-trips', async ({ page }) => {
  await editor(page);
  const text = await (await page.request.get('/api/fs/lua/pinecil.lua')).text();
  await row(page, 'lua/pinecil.lua').click({ button: 'right' });
  await menu(page).getByRole('button', { name: 'Store compressed (.gz)' }).click();
  await expect(row(page, 'lua/pinecil.lua.gz')).toBeVisible();
  await expect(row(page, 'lua/pinecil.lua')).toHaveCount(0);
  expect(await (await page.request.get('/api/fs/lua/pinecil.lua.gz')).text()).toBe(text);
  await row(page, 'lua/pinecil.lua.gz').click({ button: 'right' });
  await expect(menu(page).locator('.sm-info')).toContainText('compressed (gzip)');
  await menu(page).getByRole('button', { name: 'Store uncompressed' }).click();
  await expect(row(page, 'lua/pinecil.lua')).toBeVisible();
  await expect(row(page, 'lua/pinecil.lua.gz')).toHaveCount(0);
  expect(await (await page.request.get('/api/fs/lua/pinecil.lua')).text()).toBe(text);
});

test('pictures: a PNG shows with its pixel size; an SVG (.svg.gz) toggles to text', async ({ page }) => {
  await editor(page, 'www/dot.png');
  await expect(page.locator('.ed-preview')).toBeVisible();
  await expect(page.locator('.ed-pv-info')).toContainText('3 × 2 px');
  await page.evaluate(() => window.__btgwApp.openFile('www/logo.svg.gz'));
  await expect(page.locator('.ed-pv-info')).toContainText('40 × 20 px');
  await page.locator('.ed-b-svg').click();
  await expect(page.locator('.ed-preview')).toBeHidden();
  await expect(page.locator('.ed-surface textarea')).toHaveValue(/<svg/);
  await page.locator('.ed-b-svg').click();
  await expect(page.locator('.ed-preview')).toBeVisible();
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(page.locator('.ed-preview')).toBeHidden();
});

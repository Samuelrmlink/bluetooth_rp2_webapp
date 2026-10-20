// The SD card in the web app (docs/sd.md): the editor's top-level sd/ folder
// (over /api/sd/files) and Settings › SD card, against the devserver's mock
// card. test.sd switches the card's state, or drops the module (a build
// without CONFIG_SD).
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');
const sdFile = (page, p) => page.evaluate(async (path) => {
  const r = await fetch(`/api/sd/files/${path}`);
  return r.ok ? r.text() : `HTTP ${r.status}`;
}, p);

async function editor(page) {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(row(page, 'lua/anker.lua')).toBeVisible();
  await page.waitForFunction(() => {
    const v = window.__btgwApp._layout.version;
    const s = window.__settle || (window.__settle = { v, t: performance.now() });
    if (s.v !== v) { s.v = v; s.t = performance.now(); }
    return performance.now() - s.t > 1500;
  }, null, { polling: 100 });
}

async function openSd(page) {
  await expect(row(page, 'sd')).toBeVisible();
  await row(page, 'sd').click();
  await expect(row(page, 'sd/readme.txt')).toBeVisible();
}

test('the card is a top-level sd folder: browse, open, edit and save a file on it', async ({ page }) => {
  await editor(page);
  await openSd(page);
  await expect(row(page, 'sd/logs')).toBeVisible();
  await expect(row(page, 'sd/empty')).toBeVisible();
  await page.evaluate(() => window.__btgwApp.setPref('vim', false));
  await page.evaluate(() => window.__btgwApp.openFile('sd/readme.txt'));
  await expect(page.locator('.ed-input')).toHaveValue(/on the card/);
  await page.click('.ed-input');
  await page.keyboard.press('Control+End');
  await page.keyboard.type('edited\n');
  await page.keyboard.press('Control+KeyS');
  await expect.poll(() => sdFile(page, 'readme.txt')).toBe('on the card\nedited\n');
  // flash-only rules stay off the card: no .bak was made
  await expect.poll(() => sdFile(page, 'readme.txt.bak')).toBe('HTTP 404');
});

test('New folder here… on the card makes the folder there', async ({ page }) => {
  await editor(page);
  await openSd(page);
  await row(page, 'sd').click({ button: 'right' });
  await expect(menu(page).getByRole('button', { name: 'SD card settings…' })).toBeVisible();
  await expect(menu(page).getByRole('button', { name: 'Download as .tar' })).toHaveCount(0);   // tar is flash-only
  await expect(menu(page).getByRole('button', { name: /Delete/ })).toHaveCount(0);              // the card itself
  page.once('dialog', (d) => d.accept('sd/photos/2026'));
  await menu(page).getByRole('button', { name: 'New folder here…' }).click();
  await expect(row(page, 'sd/photos')).toBeVisible();
  const st = await page.evaluate(async () => (await fetch('/api/sd/files/photos')).json());
  expect(st.files).toEqual([{ name: '2026', dir: true }]);
});

test('the 128 KB flash limit does not apply on the card; moving across is refused', async ({ page }) => {
  await editor(page);
  const r = await page.evaluate(async () => {
    const fs = await import('/js/fs.js');
    const big = new Blob([new Uint8Array(300 * 1024)]);
    return {
      sd: await fs.write('sd/big.bin', big),
      flash: await fs.write('big.bin', big),
      across: await fs.rename('sd/readme.txt', 'lua/readme.txt'),
      within: await fs.rename('sd/readme.txt', 'sd/logs/readme.txt'),
    };
  });
  expect(r.sd.ok).toBe(true);
  expect(r.sd.bytes).toBe(300 * 1024);
  expect(r.flash.ok).toBe(false);
  expect(r.flash.status).toBe(413);
  expect(r.across.ok).toBe(false);
  expect(r.across.error).toMatch(/between the SD card and flash/);
  expect(r.within.ok).toBe(true);
  expect(await sdFile(page, 'logs/readme.txt')).toBe('on the card\n');
});

test('no card: the folder says why instead of listing', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.ws.call('test.sd', { state: 'none' }));
  const r = await page.evaluate(async () => (await import('/js/fs.js')).list('sd'));
  expect(r.ok).toBe(false);
  expect(r.status).toBe(503);
  expect(r.error).toBe('no card');
});

test('a build without the card: no sd folder, no SD card settings', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.ws.call('test.sd', { module: false }));
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(row(page, 'lua/anker.lua')).toBeVisible();
  await expect(row(page, 'sd')).toHaveCount(0);
  await page.keyboard.press('Alt+KeyS');
  await expect(page.locator('#sect-sd')).toBeHidden();
  await page.evaluate(() => window.__btgwApp.ws.call('test.sd', { module: true }));   // hand it back
});

test('Settings › SD card: status, format with confirmation and progress, bench', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-sd'));
  const st = page.locator('.sdc-status');
  await expect(st).toContainText('ready');
  await expect(st).toContainText('SDHC · 31.9 GB');
  await expect(st).toContainText('SDIO 4-bit · 18.8 MHz');
  await expect(page.locator('#sd-fs')).toHaveValue('fat32');            // the card's default
  await expect(page.locator('#sd-cluster')).toHaveValue('32768');
  await page.locator('#sd-fs').selectOption('exfat');
  await expect(page.locator('.sdc-derived')).toContainText('clusters');
  const asked = [];
  page.once('dialog', (d) => { asked.push(d.message()); d.accept(); });
  await page.getByRole('button', { name: 'Format…' }).click();
  await expect.poll(() => asked[0] || '').toMatch(/Format SD "SC32G".*EXFAT.*erased/s);
  await expect(page.locator('.sdc-note')).toHaveText('formatted as exfat', { timeout: 10000 });
  const files = await page.evaluate(async () => (await fetch('/api/sd/files')).json());
  expect(files.files).toEqual([]);                                     // a fresh card
  await page.getByRole('button', { name: 'Run 10 MB bench' }).click();
  await expect(page.locator('.sdc-bench')).toContainText('write 2.1 MB/s · read 5.4 MB/s');
});

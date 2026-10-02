// Settings -> System: reboot, Lua VM restart, the heap census and its download.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

async function system(page) {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-system'));
}

test('the Lua memory census is shown', async ({ page }) => {
  await system(page);
  const heap = page.locator('.sys-heap');
  await expect(heap).toContainText('of 160.0 KB');
  await expect(heap).toContainText('registry');
});

test('Restart Lua VM asks, then reports what it stopped', async ({ page }) => {
  await system(page);
  page.once('dialog', (d) => d.accept());
  await page.getByRole('button', { name: 'Restart Lua VM' }).click();
  await expect(page.locator('.sys-note')).toContainText('Lua VM rebuilt: 1 script(s) stopped;');
  await expect(page.locator('.sys-note')).toContainText('largest free block 60.2 KB → 140.6 KB');
});

test('Defragment rebuilds the VM and starts the running scripts again, and says so in the Console', async ({ page }) => {
  await system(page);
  page.once('dialog', (d) => { expect(d.message()).toContain('start again'); d.accept(); });
  await page.getByRole('button', { name: 'Defragment Lua memory' }).click();
  await expect(page.locator('.sys-note')).toContainText('1 script(s) stopped and started again');
  const running = await page.evaluate(async () => (await window.__btgwApp.ws.call('script.list')).scripts.find((x) => x.name === 'anker')?.running);
  expect(running).toBe(true);
  await page.keyboard.press('Escape');
  await page.evaluate(() => window.__btgwApp.show('console'));
  await expect(page.locator('.console-line').filter({ hasText: 'Lua VM rebuilt' }).first()).toBeVisible();
});

test('Reboot asks first; No sends nothing', async ({ page }) => {
  await system(page);
  page.once('dialog', (d) => d.dismiss());
  await page.getByRole('button', { name: 'Reboot gateway' }).click();
  await expect(page.locator('.sys-note')).toHaveText('');
  page.once('dialog', (d) => d.accept());
  await page.getByRole('button', { name: 'Reboot gateway' }).click();
  await expect(page.locator('.sys-note')).toContainText('Rebooting');
});

test('Download heap dump saves the census and the whole arena', async ({ page }) => {
  await system(page);
  const [dl] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('button', { name: 'Download heap dump' }).click(),
  ]);
  expect(dl.suggestedFilename()).toMatch(/^lua-heap-.*\.json$/);
  const body = JSON.parse(await (await dl.createReadStream()).toArray().then((c) => Buffer.concat(c).toString()));
  expect(body.heap.arena.used).toBeGreaterThan(0);
  expect(body.arena.hex.length).toBe(4096 * 2);
  await expect(page.getByRole('button', { name: 'Download heap dump' })).toBeEnabled();
});

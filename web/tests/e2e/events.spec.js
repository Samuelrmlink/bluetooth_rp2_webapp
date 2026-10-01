// GATT Events: the Scanner's table behaviour (resize, reorder, Columns,
// keyboard cells, the cell menu) plus search with n / Shift+N.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const rows = (page) => page.locator('.ge-table tbody tr');
const heads = (page) => page.$$eval('.ge-table thead th[data-key]', (t) => t.map((x) => x.dataset.key));
const col = (page, key) => page.locator(`.ge-table thead th[data-key="${key}"]`);
const cursor = (page) => page.evaluate(() => {
  const el = document.querySelector('.ge-table .is-cursor');
  if (!el) return null;
  return el.tagName === 'TH' ? `th:${el.dataset.key}`
    : `td:${[...el.parentElement.parentElement.rows].indexOf(el.parentElement)}:${[...el.parentElement.cells].indexOf(el)}`;
});

// Connect, then read Device Name twice, Battery Level and the vendor
// characteristic, and write once: six events of known content.
async function events(page) {
  await ready(page);
  await page.evaluate(async () => {
    const app = window.__btgwApp;
    const r = await app.ws.call('scan.list');
    const addr = r.devices.find((x) => x.connectable).addr;
    await app.ws.call('dev.connect', { device: addr });
    for (let i = 0; i < 40 && !app.devices.get(addr)?.gatt; i++) await new Promise((res) => setTimeout(res, 50));
    for (const id of ['0.0', '2.0', '3.0', '0.0']) await app.ws.call('gatt.read', { device: addr, id });
    app.show('gatt-events');
  });
  await expect(rows(page)).toHaveCount(4);
}

test('the Scanner\'s table: keyboard cells, a/d sort, Shift+L widens and moves, drag reorder', async ({ page }) => {
  await events(page);
  await page.locator('.ge-wrap').focus();
  await page.keyboard.press('Space');
  await expect.poll(() => cursor(page)).toBe('td:0:0');
  await page.keyboard.press('KeyL');
  await page.keyboard.press('KeyJ');
  await expect.poll(() => cursor(page)).toBe('td:1:1');
  const key = (await heads(page))[1];
  await page.keyboard.press('KeyA');
  await expect(col(page, key)).toHaveAttribute('aria-sort', 'ascending');
  const w0 = (await col(page, key).boundingBox()).width;
  await page.keyboard.press('Shift+KeyL');
  await expect.poll(async () => Math.round((await col(page, key).boundingBox()).width - w0)).toBe(8);
  await page.keyboard.press('KeyK');
  await page.keyboard.press('KeyK');
  await expect.poll(() => cursor(page)).toBe(`th:${key}`);
  await page.keyboard.press('Shift+KeyL');
  await expect.poll(async () => (await heads(page))[2]).toBe(key);
  await expect(page.locator('#keyhint')).toContainText('move column');
  // drag the Value heading to the front
  const v = await col(page, 'value').boundingBox();
  const t = await col(page, 'time').boundingBox();
  await page.mouse.move(v.x + 20, v.y + v.height / 2);
  await page.mouse.down();
  await page.mouse.move(t.x + 5, t.y + t.height / 2, { steps: 6 });
  await page.mouse.up();
  await expect.poll(async () => (await heads(page))[0]).toBe('value');
});

test('Columns dialog: optional columns, remembered', async ({ page }) => {
  await events(page);
  await page.locator('.ge-wrap').focus();
  await page.keyboard.press('KeyC');
  const dlg = page.locator('[id^="gatt-events-columns-"]').filter({ visible: true });
  await expect(dlg).toBeVisible();
  await dlg.locator('input[data-col="hex"]').check();
  await dlg.locator('input[data-col="dev"]').uncheck();
  await expect(col(page, 'hex')).toHaveCount(1);
  await expect(col(page, 'dev')).toHaveCount(0);
  const i = (await heads(page)).indexOf('hex');
  await expect(rows(page).first().locator('td').nth(i)).toHaveText('6d6f636b');   // raw, whatever the format
  await page.keyboard.press('Escape');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.evaluate(() => window.__btgwApp.show('gatt-events'));
  await expect(col(page, 'hex')).toHaveCount(1);
  await expect(col(page, 'dev')).toHaveCount(0);
});

test('search: / opens it selected, matches marked, n / Shift+N and Prev / Next step, only-matching filters', async ({ page }) => {
  await events(page);
  await page.locator('.ge-wrap').focus();
  await page.keyboard.press('Slash');
  const dlg = page.locator('[id^="gatt-events-search-"]').filter({ visible: true });
  const box = dlg.locator('.gs-q');
  await expect(box).toBeFocused();
  await page.keyboard.type('mock');
  await expect(page.locator('.ge-table td.is-match')).toHaveCount(2);          // two reads of "mock"
  await expect(page.locator('.ge-table td.is-match-cur')).toHaveCount(1);
  await expect(dlg.locator('.gs-count')).toHaveText('1 of 2');
  await expect(page.locator('.ge-search-btn')).toHaveText('Search (1/2)');
  await dlg.locator('.gs-next').click();
  await expect(dlg.locator('.gs-count')).toHaveText('2 of 2');
  await dlg.locator('.gs-next').click();
  await expect(dlg.locator('.gs-count')).toHaveText('1 of 2');                // wraps
  await dlg.locator('.gs-prev').click();
  await expect(dlg.locator('.gs-count')).toHaveText('2 of 2');
  await box.press('Enter');
  await expect(dlg.locator('.gs-count')).toHaveText('1 of 2');
  // only matching
  await dlg.locator('.gs-only').check();
  await expect(rows(page)).toHaveCount(2);
  await dlg.locator('.gs-only').uncheck();
  await expect(rows(page)).toHaveCount(4);
  await page.keyboard.press('Escape');                                          // leave the box
  await page.keyboard.press('Escape');                                          // close the dialog
  await expect(dlg).toBeHidden();
  // in the table, n / Shift+N move the cell cursor between the matches
  await page.locator('.ge-wrap').focus();
  await page.keyboard.press('Space');
  await page.keyboard.press('KeyN');
  await expect.poll(() => cursor(page)).toMatch(/^td:/);
  const a = await cursor(page);
  await page.keyboard.press('KeyN');
  await expect.poll(() => cursor(page)).not.toBe(a);
  const b = await cursor(page);
  await expect(page.locator('.ge-table td.is-match-cur')).toHaveCount(1);
  await page.keyboard.press('Shift+KeyN');
  await expect.poll(() => cursor(page)).toBe(a);
  expect(a).not.toBe(b);
});

test('cell menu: Enter opens it, the value copies, Search for this, Esc closes only the menu', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await events(page);
  const vi = (await heads(page)).indexOf('value');
  await page.locator('.ge-wrap').focus();
  await page.keyboard.press('Space');
  for (let i = 0; i < vi; i++) await page.keyboard.press('KeyL');
  await page.keyboard.press('Enter');
  const menu = page.locator('#pane-popover .s-menu');
  await expect(menu).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#pane-popover')).toBeHidden();
  await expect(page.locator('.ge-wrap')).toHaveAttribute('data-nav-active', '');
  // click a "mock" value cell: copy, then search for it
  const cell = rows(page).filter({ hasText: 'mock' }).first().locator('td').nth(vi);
  await cell.click();
  await menu.locator('.sm-value').click();
  await expect(page.locator('.toast')).toHaveText('Copied');
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe('mock');
  await cell.click();
  await menu.getByRole('button', { name: 'Search for this' }).click();
  await expect(page.locator('.ge-table td.is-match')).toHaveCount(2);
});

test('embedded in GATT (tabbed view): its own Search and Columns, and / works there', async ({ page }) => {
  await events(page);
  await page.keyboard.press('Alt+Shift+KeyT');
  await page.evaluate(() => window.__btgwApp.show('gatt'));
  await page.click('.g-ev');
  const host = page.locator('.gatt-events-host');
  await expect(host.locator('.ge-search-btn')).toBeVisible();
  await expect(host.locator('.ge-cols-btn')).toBeVisible();
  await host.locator('.ge-wrap').focus();
  await page.keyboard.press('Slash');
  await expect(page.locator('[id^="gatt-events-search-"]').filter({ visible: true }).locator('.gs-q')).toBeFocused();
});

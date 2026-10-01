// The Scanner: paged loading, the Filter and Columns dialogs, a keyboard
// grid, the cell menu, drag-to-reorder, and the clear buttons -- against the
// mock, whose three devices carry real advertisement payloads.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const rows = (page) => page.locator('.s-table tbody tr');
const heads = (page) => page.$$eval('.s-table thead th[data-key]', (t) => t.map((x) => x.dataset.key));
const col = (page, key) => page.locator(`.s-table thead th[data-key="${key}"]`);
const cellIn = async (page, rowText, key) => {
  const i = (await heads(page)).indexOf(key);
  return rows(page).filter({ hasText: rowText }).locator('td').nth(i);
};

async function scanner(page) {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.show('scanner'));
  await expect(rows(page)).toHaveCount(3);          // all three, though the mock pages two at a time
}

test('every device loads (scan.list is paged), and the count says so', async ({ page }) => {
  await scanner(page);
  await expect(page.locator('.s-count')).toHaveText('3 devices');
  expect(await heads(page)).toEqual(['action', 'addr', 'rssi', 'name', 'mfg', 'services']);
  expect((await heads(page))[0]).toBe('action');      // Connect stays first
});

test('Filter dialog: live text filters, RSSI slider, Alt+N between boxes, Clear filters, remembered', async ({ page }) => {
  await scanner(page);
  await page.click('.s-filter-btn');
  const dlg = page.locator('#scanner-filter-dlg');
  await expect(dlg).toBeVisible();
  await dlg.locator('.sf-name').fill('hrm');
  await expect(rows(page)).toHaveCount(1);
  await expect(page.locator('.s-count')).toHaveText('1 of 3 devices');
  await expect(page.locator('.s-filter-btn')).toHaveText('Filter (1)');
  await dlg.locator('.sf-name').fill('');
  await dlg.locator('.sf-rssi').fill('-60');
  await expect(dlg.locator('.sf-rssi-v')).toHaveText('≥ -60 dBm');
  await expect(rows(page)).toHaveCount(1);                  // only HRM (-52) is that strong
  await dlg.locator('.sf-name').focus();
  await page.keyboard.press('Alt+KeyN');
  await expect(dlg.locator('.sf-mfg')).toBeFocused();
  await page.keyboard.press('Alt+KeyN');
  await page.keyboard.press('Alt+KeyN');
  await expect(dlg.locator('.sf-name')).toBeFocused();      // wraps
  await page.keyboard.press('Alt+Shift+KeyN');
  await expect(dlg.locator('.sf-svc')).toBeFocused();
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await expect(rows(page)).toHaveCount(1);                  // remembered
  await page.click('.s-filter-btn');
  await dlg.locator('.sf-clear').click();
  await expect(rows(page)).toHaveCount(3);
  await expect(dlg.locator('.sf-rssi-v')).toHaveText('off');
  await page.keyboard.press('Escape');
  await expect(dlg).toBeHidden();
});

test('Expire removes devices not heard for that long, during a scan only, never a connected one', async ({ page }) => {
  await scanner(page);
  await page.evaluate(() => {
    const s = window.__btgwApp.devices;
    s.upsert({ addr: '00:11:22:33:44:55', rssi: -70, name: 'stale' });
    s.devices.get('00:11:22:33:44:55').seen = Date.now() - 60000;
  });
  await expect(rows(page)).toHaveCount(4);
  await page.click('.s-filter-btn');
  await page.locator('.sf-exp').fill('1');                  // 5 s
  await expect(page.locator('.sf-exp-v')).toHaveText('5 s');
  await page.keyboard.press('Escape');
  await page.waitForTimeout(1500);
  await expect(rows(page)).toHaveCount(4);                   // not scanning: kept
  await page.click('.s-toggle');
  await expect(rows(page).filter({ hasText: 'stale' })).toHaveCount(0, { timeout: 4000 });
  await expect(rows(page)).toHaveCount(3);
});

test('Columns dialog shows any advertisement field, raw included, and remembers', async ({ page }) => {
  await scanner(page);
  await page.locator('.s-wrap').focus();
  await page.keyboard.press('KeyC');
  const dlg = page.locator('#scanner-columns-dlg');
  await expect(dlg).toBeVisible();
  for (const k of ['adv', 'name_complete', 'name_short', 'uuid128i', 'tx', 'company']) await dlg.locator(`input[data-col="${k}"]`).check();
  await dlg.locator('input[data-col="mfg"]').uncheck();
  await expect(col(page, 'adv')).toHaveCount(1);
  await expect(col(page, 'mfg')).toHaveCount(0);
  await expect(await cellIn(page, 'HRM-9000', 'name_complete')).toHaveText('HRM-9000');
  await expect(await cellIn(page, 'HRM-9000', 'name_short')).toHaveText('HRM');
  await expect(await cellIn(page, 'HRM-9000', 'uuid128i')).toHaveText('6e400001-b5a3-f393-e0a9-e50e24dcca9e');
  await expect(await cellIn(page, 'HRM-9000', 'tx')).toHaveText('-12');
  await expect(await cellIn(page, 'D9:AA', 'company')).toHaveText('Apple (0x004c)');
  await expect(await cellIn(page, 'D9:AA', 'adv')).toHaveText('02010607ff4c0012020003');
  await page.keyboard.press('Escape');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await expect(col(page, 'adv')).toHaveCount(1);
  await expect(col(page, 'mfg')).toHaveCount(0);
});

test('keyboard grid: move cells, sort with a/d, resize and reorder with Shift+H/L', async ({ page }) => {
  await scanner(page);
  await page.locator('.s-wrap').focus();
  await page.keyboard.press('Space');
  const cursor = () => page.evaluate(() => {
    const el = document.querySelector('.s-table .is-cursor');
    if (!el) return null;
    return el.tagName === 'TH' ? `th:${el.dataset.key}` : `td:${[...el.parentElement.cells].indexOf(el)}`;
  });
  await expect.poll(cursor).toBe('td:0');
  await page.keyboard.press('KeyL');
  await page.keyboard.press('KeyL');
  const key = (await heads(page))[2];
  await expect.poll(cursor).toBe('td:2');
  // a / d sort that column
  await page.keyboard.press('KeyD');
  await expect(col(page, key)).toHaveAttribute('aria-sort', 'descending');
  await page.keyboard.press('KeyA');
  await expect(col(page, key)).toHaveAttribute('aria-sort', 'ascending');
  // Shift+L on a row widens it by 8
  const w0 = (await col(page, key).boundingBox()).width;
  await page.keyboard.press('Shift+KeyL');
  expect(Math.round((await col(page, key).boundingBox()).width - w0)).toBe(8);
  // up to the header; Shift+L there moves the column right
  await page.keyboard.press('KeyK');
  await expect.poll(cursor).toBe(`th:${key}`);
  await page.keyboard.press('Shift+KeyL');
  await expect.poll(async () => (await heads(page))[3]).toBe(key);
  await expect.poll(cursor).toBe(`th:${key}`);              // the cursor goes with it
  await expect(page.locator('#keyhint')).toContainText('move column');
  // Enter on a cell opens its menu; Esc closes it
  await page.keyboard.press('KeyJ');
  await page.keyboard.press('KeyH');
  await page.keyboard.press('KeyH');
  await page.keyboard.press('Enter');
  await expect(page.locator('#pane-popover .s-menu')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#pane-popover')).toBeHidden();
});

test('cell menu: Filter by a service, and the full value copies', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await scanner(page);
  await (await cellIn(page, 'HRM-9000', 'services')).click();
  const menu = page.locator('#pane-popover .s-menu');
  await expect(menu).toBeVisible();
  await menu.getByRole('button', { name: 'Filter by 180d Heart Rate' }).click();
  await expect(rows(page)).toHaveCount(1);
  await expect(page.locator('.toast')).toContainText('Filtered by 180d Heart Rate');
  await page.click('.s-filter-btn');
  await page.locator('.sf-clear').click();
  await page.keyboard.press('Escape');
  await (await cellIn(page, 'D9:AA', 'mfg')).click();
  await menu.locator('.sm-value').click();
  await expect(page.locator('.toast')).toHaveText('Copied');
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe('4c0012020003');
});

test('drag a heading to move the column; columns can be narrow', async ({ page }) => {
  await scanner(page);
  const before = await heads(page);
  const name = await col(page, 'name').boundingBox();
  const addr = await col(page, 'addr').boundingBox();
  await page.mouse.move(name.x + 20, name.y + name.height / 2);
  await page.mouse.down();
  await page.mouse.move(addr.x + 10, addr.y + addr.height / 2, { steps: 6 });
  await page.mouse.up();
  const after = await heads(page);
  expect(after.indexOf('name')).toBe(before.indexOf('addr'));
  await expect(col(page, 'name')).not.toHaveAttribute('aria-sort', 'ascending');   // a drag is not a sort
  // RSSI down to the minimum
  const r = col(page, 'rssi').locator('.col-resize');
  const b = await r.boundingBox();
  await page.mouse.move(b.x + 3, b.y + b.height / 2);
  await page.mouse.down();
  await page.mouse.move(b.x - 200, b.y + b.height / 2, { steps: 4 });
  await page.mouse.up();
  expect(Math.round((await col(page, 'rssi').boundingBox()).width)).toBe(24);
});

test('Connect from the row; Clear list keeps a connected device', async ({ page }) => {
  await scanner(page);
  await rows(page).filter({ hasText: 'HRM-9000' }).locator('.s-conn').click();
  await expect(rows(page).filter({ hasText: 'HRM-9000' }).locator('.s-conn')).toHaveText('Disconnect');
  await page.click('.s-clear');
  await expect(rows(page)).toHaveCount(1);
  await expect(rows(page).first()).toContainText('HRM-9000');
});

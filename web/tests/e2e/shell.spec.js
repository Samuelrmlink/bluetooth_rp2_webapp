// The window manager taken from the km003c app, in this app: every UI is a
// window, and the keyboard reaches all of it.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

test('every built-in window is offered, grouped, and the first visit is arranged', async ({ page }) => {
  await ready(page);
  await expect(page.locator('#tiles .tile')).toHaveCount(3);
  const groups = await page.$$eval('#tiles .tile .pane-pick >> nth=0', (s) =>
    [...s[0].querySelectorAll('optgroup')].map((g) => [g.label, [...g.children].map((o) => o.textContent)]));
  expect(groups[0]).toEqual(['Tools', ['Editor', 'Scanner', 'GATT', 'GATT Events', 'Console', 'Hardware']]);
});

test('Alt+hjkl moves between windows; ? lists every binding; Alt+S opens Settings', async ({ page }) => {
  await ready(page);
  const active = () => page.evaluate(() => document.querySelector('#tiles .tile[data-active]')?.dataset.pane);
  expect(await active()).toBe('editor');
  await page.keyboard.press('Alt+KeyL');
  expect(await active()).toBe('scanner');
  await page.keyboard.press('Alt+KeyJ');
  expect(await active()).toBe('console');
  await page.keyboard.press('Alt+KeyH');
  expect(await active()).toBe('editor');

  await page.keyboard.press('Escape');
  await page.keyboard.press('?');
  const rows = await page.locator('#help-modal .keys tr').count();
  const bindings = await page.evaluate(async () => (await import('/js/keys.js')).BINDINGS.filter((b) => b.group).length);
  expect(rows).toBe(bindings);
  await page.keyboard.press('Escape');

  await page.keyboard.press('Alt+KeyS');
  await expect(page.locator('#settings-modal')).toBeVisible();
  for (const id of ['sect-ui', 'sect-wifi', 'sect-wireguard', 'sect-bluetooth', 'sect-firmware']) {
    await expect(page.locator(`#${id}`)).toHaveCount(1);
  }
});

test('tabbed view survives a reload', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+Shift+KeyT');
  await expect(page.locator('#tabbar')).toBeVisible();
  await page.reload();
  await expect(page.locator('#tabbar')).toBeVisible();
});

test('the scanner lists devices, connects from the keyboard, and l opens GATT', async ({ page }) => {
  await ready(page);
  await page.click('.s-toggle');
  await expect(page.locator('.s-table tbody tr')).toHaveCount(3);
  await page.focus('.s-wrap');
  await page.keyboard.press('Enter');     // enter the list
  await page.keyboard.press('Enter');     // connect the selected (strongest) row
  await expect(page.locator('.s-table tr.is-connected')).toHaveCount(1);
  await page.keyboard.press('KeyL');
  await expect(page.locator('.t-svc').first()).toHaveText(/Generic Access/);
});

test('scanner filters as you type with the scan stopped; Alt+N steps the boxes', async ({ page }) => {
  await ready(page);
  const rows = page.locator('.s-table tbody tr');
  await page.click('.s-toggle');
  await expect(rows).toHaveCount(3);
  await page.click('.s-toggle');                       // stop: filtering must still work
  await expect(page.locator('.s-toggle')).toHaveText('Scan');
  await expect(page.locator('.s-count')).toHaveText('3 devices');
  await page.fill('.s-name', 'hrm');
  await expect(rows).toHaveCount(1);
  await expect(page.locator('.s-count')).toHaveText('1 of 3 devices');
  await page.fill('.s-name', '');
  await expect(rows).toHaveCount(3);

  await page.focus('.s-name');
  await page.keyboard.press('Alt+KeyN');
  await expect(page.locator('.s-mfg')).toBeFocused();
  await page.keyboard.type('4c00');
  await expect(rows).toHaveCount(1);
  await page.keyboard.press('Alt+KeyN');
  await page.keyboard.press('Alt+KeyN');
  await expect(page.locator('.s-rssi')).toBeFocused();
  await page.keyboard.press('Alt+KeyN');               // wraps
  await expect(page.locator('.s-name')).toBeFocused();
  await page.keyboard.press('Alt+Shift+KeyN');
  await expect(page.locator('.s-rssi')).toBeFocused();
  // Out of the box, Alt+N is the next window again.
  await page.keyboard.press('Escape');
  await page.keyboard.press('Alt+KeyN');
  await expect(page.locator('#tiles .tile[data-active]')).not.toHaveAttribute('data-pane', 'scanner');
});

test('scanner connects by its button; columns sort and resize, and are remembered', async ({ page }) => {
  await ready(page);
  await page.click('.s-toggle');
  const rows = page.locator('.s-table tbody tr');
  await expect(rows).toHaveCount(3);
  const names = () => page.$$eval('.s-table tbody tr td:nth-child(3)', (tds) => tds.map((t) => t.textContent));
  await page.click('.s-table th[data-key="name"] .col-sort');
  await expect.poll(names).toEqual(['ASHDJW61F45100348', 'HRM-9000', '']);
  await page.click('.s-table th[data-key="name"] .col-sort');
  await expect.poll(names).toEqual(['HRM-9000', 'ASHDJW61F45100348', '']);
  await expect(page.locator('.s-table th[data-key="name"]')).toHaveAttribute('aria-sort', 'descending');

  const th = page.locator('.s-table th[data-key="name"]');
  const w0 = (await th.boundingBox()).width;
  const h = await page.locator('.s-table th[data-key="name"] .col-resize').boundingBox();
  await page.mouse.move(h.x + 3, h.y + h.height / 2);
  await page.mouse.down();
  await page.mouse.move(h.x + 83, h.y + h.height / 2, { steps: 4 });
  await page.mouse.up();
  const w1 = (await th.boundingBox()).width;
  expect(Math.round(w1 - w0)).toBe(80);

  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await expect(rows).toHaveCount(3);
  expect(Math.round((await th.boundingBox()).width)).toBe(Math.round(w1));
  await expect.poll(names).toEqual(['HRM-9000', 'ASHDJW61F45100348', '']);

  await rows.filter({ hasText: 'HRM-9000' }).locator('.s-conn').click();
  await expect(page.locator('.s-table tr.is-connected')).toHaveCount(1);
  await expect(rows.filter({ hasText: 'HRM-9000' }).locator('.s-conn')).toHaveText('Disconnect');
});

test('tabbed view: Alt+N visits every tab in order, one at a time; Alt+Shift+N goes back', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+Shift+KeyT');
  await expect(page.locator('#tabbar')).toBeVisible();
  const tabs = await page.$$eval('#tabbar .tab', (b) => b.map((x) => x.dataset.pane));
  expect(tabs.length).toBeGreaterThanOrEqual(5);
  const active = () => page.$eval('#tabbar .tab.active', (b) => b.dataset.pane);
  // From wherever it starts, n presses visit n consecutive tabs and wrap.
  const start = tabs.indexOf(await active());
  for (let k = 1; k <= tabs.length; k++) {
    await page.keyboard.press('Alt+KeyN');
    await expect.poll(active).toBe(tabs[(start + k) % tabs.length]);
  }
  for (let k = 1; k <= 2; k++) {
    await page.keyboard.press('Alt+Shift+KeyN');
    await expect.poll(active).toBe(tabs[(start - k + tabs.length) % tabs.length]);
  }
});

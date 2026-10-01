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

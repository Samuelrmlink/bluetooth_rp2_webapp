// The window manager taken from the km003c app, in this app: every UI is a
// window, and the keyboard reaches all of it.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

test('every built-in window is offered, grouped, and the first visit is arranged', async ({ page }) => {
  await ready(page);
  await expect(page.locator('#tiles .tile')).toHaveCount(3);
  const groups = await page.$$eval('#tiles .tile .pane-pick >> nth=0', (s) =>
    [...s[0].querySelectorAll('optgroup')].map((g) => [g.label, [...g.children].map((o) => o.textContent)]));
  expect(groups[0]).toEqual(['Tools', ['Editor', 'Scanner', 'GATT', 'Console', 'Hardware']]);
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

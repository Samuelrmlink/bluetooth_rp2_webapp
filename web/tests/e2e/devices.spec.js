// Device plugins: discovered from telemetry, fetched only when opened,
// writing through the driver's anker.set, and taken apart when closed.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const CONTROLS = 'anker-pps.controls@E8EECCB6897E';
const control = (page, label) => page.locator('.dev-controls .ctl').filter({ has: page.locator('.ctl-label', { hasText: new RegExp(`^${label}$`) }) });

test('discovered but not fetched; opened, fetched and live; closed, taken apart', async ({ page }) => {
  const fetched = [];
  page.on('request', (r) => { if (/\/js\/devices\/[^/]+\/plugin\.(js|css)/.test(r.url())) fetched.push(r.url().replace(/^.*\/js\//, '')); });
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  const groups = await page.$$eval('#tiles .tile .pane-pick >> nth=0', (s) => [...s[0].querySelectorAll('optgroup')].map((g) => g.label));
  expect(groups).toContain('C1000');
  expect(fetched).toEqual([]);

  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  await expect(control(page, 'Battery').locator('.ctl-readout')).toHaveText('87 %');
  expect(fetched.sort()).toEqual(['devices/anker-pps/plugin.css', 'devices/anker-pps/plugin.js']);
  await expect(page.locator('link[data-plugin="anker-pps"]')).toHaveCount(1);

  await page.evaluate((id) => {
    const t = [...document.querySelectorAll('#tiles .tile')].find((x) => x.dataset.pane === id);
    window.__btgwApp._layout.setPane(Number(t.dataset.tile), 'hardware');
  }, CONTROLS);
  await expect(page.locator('link[data-plugin]')).toHaveCount(0);
  expect(await page.evaluate((id) => window.__btgwApp.windows.get(id).win.mounted, CONTROLS)).toBe(false);
});

test('a write is pending until the device confirms it', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const car = control(page, 'Car output');
  await expect(car.locator('.ctl-switch')).toHaveText('Off');
  await car.locator('.ctl-switch').click();
  await expect(car).toHaveClass(/is-pending/);
  await expect(car).not.toHaveClass(/is-pending/, { timeout: 6000 });
  await expect(car.locator('.ctl-switch')).toHaveText('On');
});

test('unverified and uncaptured setters are shown, disabled, with the reason', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  await expect(control(page, 'AC charge limit').locator('input')).toBeDisabled();
  await expect(control(page, 'AC charge limit').locator('.ctl-note')).toContainText('payload not yet captured');
});

test('Unclassified lists every field no control claims, binary as hex', async ({ page }) => {
  await ready(page);
  const id = CONTROLS.replace('.controls@', '.unclassified@');
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  const fields = page.locator('.dev-fields tbody tr td:first-child');
  await expect(fields.filter({ hasText: 'unknown_b1' })).toHaveCount(1);
  await expect(fields.filter({ hasText: /^battery_pct$/ })).toHaveCount(0);
  await expect(page.locator('.dev-fields tbody tr').filter({ hasText: 'capability_block' }).locator('td').nth(1)).toHaveText('02 01 01 01  (hex)');
});

test('AC output asks before switching off, and a No sends nothing', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const ac = control(page, 'AC output').locator('.ctl-switch');
  await expect(ac).toHaveText('On');
  const asked = [];
  page.once('dialog', (d) => { asked.push(d.message()); d.dismiss(); });
  await ac.click();
  await expect.poll(() => asked.length).toBe(1);
  expect(asked[0]).toContain('AC outlet OFF');
  await page.waitForTimeout(1500);
  await expect(ac).toHaveText('On');
  page.once('dialog', (d) => d.accept());
  await ac.click();
  await expect(ac).toHaveText('Off');
  await ac.click();                      // on: no question
  await expect(ac).toHaveText('On');
});

test('Unclassified: Copy all puts the table, as shown, on the clipboard', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await ready(page);
  const id = CONTROLS.replace('.controls@', '.unclassified@');
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await expect(page.locator('.dev-fields tbody tr').first()).toBeVisible();
  await page.click('.dev-copy');
  await expect(page.locator('.toast')).toContainText(/Copied \d+ fields/);
  const text = await page.evaluate(() => navigator.clipboard.readText());
  const lines = text.split('\n');
  expect(lines[0]).toMatch(/^# C1000 · A1761 · /);
  expect(lines[1]).toBe('Field\tValue\tChanged');
  expect(lines.some((l) => l.startsWith('unknown_b1\t'))).toBe(true);
});

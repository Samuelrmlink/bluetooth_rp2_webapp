// The A2687's Ports window and its writable settings, against the mock's
// A2687 (dev/devserver.py: setters validated as the Lua driver does, timers
// that count down). Every write goes through anker.set and shows as pending
// until the charger's telemetry confirms it.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const PORTS = 'anker-scp.ports@7CE913601FD8';
const CONTROLS = 'anker-scp.controls@7CE913601FD8';
const card = (page, n) => page.locator(`.scp-card[data-port="${n}"]`);
const row = (scope, label) => scope.locator('.ctl', { has: scope.page().locator('.ctl-label', { hasText: new RegExp(`^${label}$`) }) });
const writes = (page) => page.evaluate(async () => (await window.__btgwApp.ws.call('test.a2687')).writes);
const state = (page) => page.evaluate(async () => (await window.__btgwApp.ws.call('test.a2687')).state);

async function open(page, id = PORTS) {
  await ready(page);
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await expect(page.locator('.scp-card, .dev-controls .ctl').first()).toBeVisible();
}

test('a card per port: watts, volts and amps, cable, device, state', async ({ page }) => {
  await open(page);
  await expect(card(page, 1).locator('.scp-watts .ctl-readout')).toHaveText('59.8 W');
  await expect(row(card(page, 1), 'Voltage · current').locator('.ctl-readout')).toHaveText('20.08 V · 2.98 A');
  await expect(row(card(page, 1), 'Cable').locator('.ctl-readout')).toHaveText('100W cable');
  await expect(row(card(page, 1), 'Device').locator('.ctl-readout')).toHaveText('brand 3 · model 17');
  await expect(card(page, 1)).toHaveAttribute('data-state', 'charging');
  await expect(card(page, 3)).toHaveAttribute('data-state', 'off');
});

test('a port off asks first (in the app\'s dialog, Cancel the default); on does not', async ({ page }) => {
  await open(page);
  const sw = row(card(page, 2), 'Output on').locator('.ctl-switch');
  await expect(sw).toHaveText('On');
  await sw.click();
  const dlg = page.locator('#confirm-dlg');
  await expect(dlg.locator('.cf-text')).toContainText('Turn off USB-C2?');
  await expect(dlg.locator('.cf-text')).toContainText('delivering 0.5 W');
  await page.keyboard.press('Escape');                              // no
  await page.waitForTimeout(800);
  expect((await writes(page)).length).toBe(0);
  await sw.click();
  await dlg.locator('.cf-ok').click();
  await expect(sw).toHaveText('Off');
  expect(await writes(page)).toEqual([{ field: 'c2_on', value: 0 }]);
  await expect(card(page, 2)).toHaveAttribute('data-state', 'off');
  await sw.click();                                                 // on: no question
  await expect(sw).toHaveText('On');
  expect((await writes(page)).at(-1)).toEqual({ field: 'c2_on', value: 1 });
});

test('auto-off: the presets set the time (not add to it), it counts down, Off cancels', async ({ page }) => {
  await open(page);
  const t = row(card(page, 1), 'Auto-off');
  await t.locator('.ctl-step', { hasText: '2 h' }).click();
  await expect.poll(async () => (await state(page)).c1_control.countdown_s).toBe(7200);
  await expect(t.locator('.ctl-timer')).toHaveValue(/^1:59:|^2:00:00/);
  await t.locator('.ctl-step', { hasText: '1 h' }).click();          // a preset, not +1 h
  await expect.poll(async () => (await state(page)).c1_control.countdown_s).toBe(3600);
  await t.locator('.ctl-step', { hasText: 'Off' }).click();
  await expect.poll(async () => (await state(page)).c1_control.countdown_active).toBe(false);
  await expect(t.locator('.ctl-timer')).toHaveValue('off');
  // a typed time
  await t.locator('.ctl-timer').fill('25m');
  await t.locator('.ctl-timer').press('Enter');
  expect((await writes(page)).at(-1)).toEqual({ field: 'c1_timer_s', value: 1500 });
});

test('charging mode: Standard and its option; Custom applies the stored profile', async ({ page }) => {
  await open(page);
  const mode = page.locator('.scp-power select').first();
  await mode.selectOption('1');
  await expect.poll(async () => (await state(page)).charging_protocol).toBe(1);
  await page.locator('.scp-power select').nth(1).selectOption('laptop_mode');
  await expect.poll(async () => (await state(page)).fixed_allocation).toBe('laptop_mode');
  await mode.selectOption('2');
  await expect.poll(async () => (await state(page)).charging_protocol).toBe(2);
  expect((await writes(page)).at(-1)).toEqual({ field: 'custom_mode', value: { mode: 1, auto_exit: 1, c1_w: 140, c2_w: 20, c3_w: 0 } });
});

test('custom mode editor: the 160 W budget is enforced; Apply sends it and says when it is applied', async ({ page }) => {
  await open(page);
  const ed = page.locator('.scp-custom');
  await ed.locator('.scp-wn[data-port="2"]').fill('40');
  await expect(ed.locator('.scp-total')).toContainText('180 W in all');
  await expect(ed.locator('.scp-apply-custom')).toBeDisabled();
  await ed.locator('.scp-wn[data-port="1"]').fill('100');
  await ed.locator('.scp-wn[data-port="3"]').fill('15');
  await expect(ed.locator('.scp-total')).toHaveText('Total 155 W / 160 W');
  await ed.locator('.scp-apply-custom').click();
  await expect(ed.locator('.scp-status')).toHaveText('applied');
  const s = await state(page);
  expect([s.charging_protocol, s.custom_power.c1_w, s.custom_power.c2_w, s.custom_power.c3_w]).toEqual([2, 100, 40, 15]);
});

test('protocols: the default shown as unknown, the PD limit enforced, Save sends all three ports', async ({ page }) => {
  await open(page);
  const pr = page.locator('.scp-protocols');
  await expect(pr.locator('.scp-proto-note')).toContainText('Not known');
  const box = (port, bit) => pr.locator(`input[data-port="${port}"][data-bit="${bit}"]`);
  await expect(box(1, 2)).toBeDisabled();                           // PD 12V: three PPS ranges on already
  await box(1, 5).uncheck();                                        // one PPS range off ...
  await expect(box(1, 2)).toBeEnabled();                            // ... so PD 12V may go on
  await box(1, 2).check();
  await box(3, 0).uncheck();                                        // C3 without UFCS
  await pr.locator('.scp-apply-proto').click();
  await expect(pr.locator('.scp-status')).toHaveText('applied');
  expect((await writes(page)).at(-1)).toEqual({ field: 'protocols', value: { c1: 0x1f, c2: 59, c3: 58 } });
  await expect(pr.locator('.scp-proto-note')).toContainText('As this gateway last set them');
});

test('the Controls window: brightness, screen-off, rotation, language, screensaver write', async ({ page }) => {
  await open(page, CONTROLS);
  const c = (label) => row(page.locator('.dev-controls'), label);
  await c('Screen off after').locator('select').selectOption('30min');
  await expect.poll(async () => (await state(page)).screen_off).toBe('30min');
  await c('Language').locator('select').selectOption('de');
  await expect.poll(async () => (await state(page)).language).toBe('de');
  await c('Screen rotation').locator('select').selectOption('left');
  await expect.poll(async () => (await state(page)).direction).toBe('left');
  await c('Screensaver').locator('.ctl-switch').click();
  await expect.poll(async () => (await state(page)).screensaver.enabled).toBe(false);
  const slider = c('Brightness').locator('input[type=range]');
  await slider.fill('60');
  await slider.dispatchEvent('change');
  await expect.poll(async () => (await state(page)).backlight_pct).toBe(60);
});

test('from the keyboard alone: j reaches a port\'s switch; Space asks; Esc keeps it on', async ({ page }) => {
  await open(page);
  await page.locator('#tiles .tile[data-active]').focus();
  for (let i = 0; i < 12; i++) {
    await page.keyboard.press('KeyJ');
    if (await page.evaluate(() => document.activeElement?.classList.contains('ctl-switch'))) break;
  }
  expect(await page.evaluate(() => document.activeElement?.classList.contains('ctl-switch'))).toBe(true);
  await page.keyboard.press('Space');
  await expect(page.locator('#confirm-dlg')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#confirm-dlg')).toBeHidden();
  expect((await writes(page)).length).toBe(0);
});

const HISTORY = 'anker-scp.history@7CE913601FD8';

test('History: read on open; Total and per-port views; hover tooltip; keyboard; table', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), HISTORY);
  await page.evaluate((x) => window.__btgwApp.show(x), HISTORY);
  await expect(page.locator('.hs-status')).toContainText('73 points, 5 min apart');
  await expect(page.locator('.hs-line')).toHaveCount(1);                       // Total: one series
  await expect(page.locator('.hs-legend .hs-key')).toHaveCount(0);            // ... needs no legend
  await page.locator('.hs-metric[data-metric="w"]').click();
  await expect(page.locator('.hs-line')).toHaveCount(3);
  await expect(page.locator('.hs-legend .hs-key')).toHaveText(['C1', 'C2', 'C3']);
  // end labels do not overprint
  const ys = await page.locator('.hs-end').evaluateAll((ts) => ts.map((t) => Number(t.getAttribute('y'))).sort((a, b) => a - b));
  for (let i = 1; i < ys.length; i++) expect(ys[i] - ys[i - 1]).toBeGreaterThanOrEqual(12.9);
  // hover: every port's value at that time, a gap shown as —
  const box = await page.locator('.hs-svg').boundingBox();
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height / 2);
  await expect(page.locator('.hs-tip')).toBeVisible();
  await expect(page.locator('.hs-tip')).toContainText('C3 —');
  // the keyboard: into the chart, l / h move by 5 min, Enter opens the copy menu
  await page.mouse.move(0, 0);
  await page.locator('.hs-plot').focus();
  await page.keyboard.press('Space');
  const t0 = await page.locator('.hs-tip b').textContent();
  await page.keyboard.press('KeyH');
  const t1 = await page.locator('.hs-tip b').textContent();
  expect(t0).not.toBe(t1);
  await page.keyboard.press('Enter');
  await expect(page.locator('#pane-popover .s-menu .sm-value')).toContainText(t1);
  await page.keyboard.press('Escape');
  // the table: every point, newest first
  await page.locator('.hs-table-wrap summary').click();
  await expect(page.locator('.hs-table tbody tr')).toHaveCount(73);
});

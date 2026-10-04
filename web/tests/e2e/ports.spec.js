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

// --- the Power section: shown per the mode the charger REPORTS ------------------

const power = (page) => page.locator('.scp-power');
const modeSel = (page) => row(power(page), 'Charging mode').locator('select');
const stdRow = (page) => row(power(page), 'Standard mode');
const editor = (page) => page.locator('.scp-custom');
const dlg = (page) => page.locator('#confirm-dlg');
const mock = (page, p) => page.evaluate((x) => window.__btgwApp.ws.call('test.a2687', x), p);
const CUSTOM_PROFILE = { mode: 1, auto_exit: true, c1_w: 90, c2_w: 70, c3_w: 0, c1_proto: 11, c2_proto: 11, c3_proto: 11 };
const inCustom = (page) => mock(page, { set: { charging_protocol: 2, custom_mode_number: 1, custom_power: CUSTOM_PROFILE } });

test('AI Mode: neither Standard\'s option nor the custom editor is shown', async ({ page }) => {
  await open(page);
  await expect(modeSel(page)).toHaveValue('0');
  await expect(stdRow(page)).toBeHidden();
  await expect(editor(page)).toBeHidden();
  await expect(power(page).locator('.scp-mode-note')).toHaveText(/shares its 160 W/);
});

test('Standard: the switch asks first, Cancel sends nothing; the option shows only once the charger confirms', async ({ page }) => {
  await open(page);
  await mock(page, { lag: 2 });                                     // the charger takes 2 s to report it
  await modeSel(page).selectOption('1');
  await expect(dlg(page).locator('.cf-text')).toContainText('from AI Mode 2.0 to Standard');
  await page.keyboard.press('Escape');                              // Cancel (the default)
  await expect(dlg(page)).toBeHidden();
  await expect(modeSel(page)).toHaveValue('0');                     // back to what the charger reports
  await page.waitForTimeout(500);
  expect(await writes(page)).toEqual([]);
  await modeSel(page).selectOption('1');
  await dlg(page).locator('.cf-ok').click();
  expect(await writes(page)).toEqual([{ field: 'charging_protocol', value: 1 }]);
  // sent, not yet reported: still hidden
  await page.waitForTimeout(800);
  await expect(stdRow(page)).toBeHidden();
  await expect.poll(async () => (await state(page)).charging_protocol, { timeout: 5000 }).toBe(1);
  await expect(stdRow(page)).toBeVisible();
  await expect(editor(page)).toBeHidden();
  await mock(page, { lag: 0.3 });
  await stdRow(page).locator('select').selectOption('laptop_mode'); // no question for the option
  await expect.poll(async () => (await state(page)).fixed_allocation).toBe('laptop_mode');
  expect((await writes(page)).at(-1)).toEqual({ field: 'fixed_allocation', value: 'laptop_mode' });
});

test('Custom: the question names the stored profile and its risks; the editor shows only once confirmed', async ({ page }) => {
  await open(page);
  await mock(page, { lag: 2, set: { custom_power: CUSTOM_PROFILE } });
  await page.waitForFunction((id) => window.__btgwApp.windows.get(id)?.win?.dev?.sample?.custom_power?.c1_w === 90, PORTS);
  await modeSel(page).selectOption('2');
  const text = dlg(page).locator('.cf-text');
  await expect(text).toContainText('C1 90 W · C2 70 W · C3 0 W');
  await expect(text).toContainText('Automatic deactivation is on');
  await dlg(page).locator('.cf-ok').click();
  expect((await writes(page)).at(-1)).toEqual({ field: 'custom_mode', value: { mode: 1, auto_exit: 1, c1_w: 90, c2_w: 70, c3_w: 0 } });
  await page.waitForTimeout(800);
  await expect(editor(page)).toBeHidden();                          // not confirmed yet
  await expect.poll(async () => (await state(page)).charging_protocol, { timeout: 5000 }).toBe(2);
  await expect(editor(page)).toBeVisible();
  await expect(stdRow(page)).toBeHidden();
  await expect(power(page).locator('.scp-mode-note')).toContainText('returns the charger to AI Mode');
  await expect(editor(page).locator('.scp-wn[data-port="1"]')).toHaveValue('90');
});

test('the custom editor: the 160 W budget is enforced; Apply edits the profile in use and says when it is applied', async ({ page }) => {
  await open(page);
  await inCustom(page);
  const ed = editor(page);
  await expect(ed).toBeVisible();
  await ed.locator('.scp-wn[data-port="1"]').fill('100');
  await expect(ed.locator('.scp-total')).toContainText('170 W in all');
  await expect(ed.locator('.scp-apply-custom')).toBeDisabled();
  await ed.locator('.scp-wn[data-port="2"]').fill('40');
  await ed.locator('.scp-wn[data-port="3"]').fill('15');
  await expect(ed.locator('.scp-total')).toHaveText('Total 155 W / 160 W');
  await ed.locator('.scp-apply-custom').click();
  await expect(ed.locator('.scp-status')).toHaveText('applied');
  const s = await state(page);
  expect([s.charging_protocol, s.custom_power.c1_w, s.custom_power.c2_w, s.custom_power.c3_w]).toEqual([2, 100, 40, 15]);
  expect((await writes(page)).at(-1)).toEqual({ field: 'custom_mode', value: { mode: 1, auto_exit: 1, c1_w: 100, c2_w: 40, c3_w: 15 } });
});

test('leaving Custom (here, or the charger by itself) hides the editor; nothing is sent on its behalf', async ({ page }) => {
  await open(page);
  await inCustom(page);
  await expect(editor(page)).toBeVisible();
  await editor(page).locator('.scp-wn[data-port="1"]').fill('60');     // an edit in progress ...
  // ... and the charger leaves Custom by itself (Automatic deactivation, the phone app)
  await mock(page, { set: { charging_protocol: 0 } });
  await expect(editor(page)).toBeHidden();
  await expect(stdRow(page)).toBeHidden();
  await expect(modeSel(page)).toHaveValue('0');
  // back in Custom: the editor shows the charger's profile, not the abandoned edit
  await inCustom(page);
  await expect(editor(page)).toBeVisible();
  await expect(editor(page).locator('.scp-wn[data-port="1"]')).toHaveValue('90');
  // and from here: AI Mode, asked and confirmed
  await modeSel(page).selectOption('0');
  await dlg(page).locator('.cf-ok').click();
  await expect(editor(page)).toBeHidden();
  expect(await writes(page)).toEqual([{ field: 'charging_protocol', value: 0 }]);
});

test('a stray Tab on the mode select asks instead of switching the charger', async ({ page }) => {
  await open(page);
  await inCustom(page);
  await expect(modeSel(page)).toHaveValue('2');
  await modeSel(page).focus();
  await page.keyboard.press('Tab');                                  // keys.js: Tab steps a <select>'s value
  await expect(dlg(page)).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(modeSel(page)).toHaveValue('2');
  await page.waitForTimeout(500);
  expect(await writes(page)).toEqual([]);
  expect((await state(page)).charging_protocol).toBe(2);
});

test('reloading the page in Custom leaves the charger in Custom and sends nothing', async ({ page }) => {
  await open(page);
  await inCustom(page);
  await expect(editor(page)).toBeVisible();
  for (let i = 0; i < 2; i++) {
    await page.reload();                                             // not open(): that resets the mock
    await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
    await page.waitForFunction((x) => window.__btgwApp.windows.has(x), PORTS);
    await page.evaluate((x) => window.__btgwApp.show(x), PORTS);
    await expect(modeSel(page)).toHaveValue('2');
    await expect(editor(page)).toBeVisible();
  }
  await page.waitForTimeout(1000);
  expect(await writes(page)).toEqual([]);
  expect((await state(page)).charging_protocol).toBe(2);
});

// Every control j reaches from the pane's first control (Esc out of a text field,
// which takes letters as text).
async function walk(page, steps = 30) {
  await page.locator('.scp-card[data-port="1"] .ctl-readout').first().focus();   // from the top each time
  const seen = new Set();
  for (let i = 0; i < steps; i++) {
    if (await page.evaluate(() => document.activeElement?.matches('input[type=text], input:not([type])'))) await page.keyboard.press('Escape');
    await page.keyboard.press('KeyJ');
    seen.add(await page.evaluate(() => {
      const a = document.activeElement;
      return a?.closest('.scp-custom') ? 'custom' : a?.closest('.ctl')?.querySelector('.ctl-label')?.textContent || a?.className || '';
    }));
  }
  return seen;
}

test('hidden settings are out of the keyboard\'s reach; shown, they are in it', async ({ page }) => {
  await open(page);                                                  // AI Mode: both hidden
  let seen = await walk(page);
  expect(seen.has('Charging mode')).toBe(true);                      // the walk got past it
  expect(seen.has('custom')).toBe(false);
  expect(seen.has('Standard mode')).toBe(false);
  await inCustom(page);
  await expect(editor(page)).toBeVisible();
  seen = await walk(page);
  expect(seen.has('custom')).toBe(true);
  expect(seen.has('Standard mode')).toBe(false);
  await mock(page, { set: { charging_protocol: 1 } });
  await expect(stdRow(page)).toBeVisible();
  seen = await walk(page);
  expect(seen.has('Standard mode')).toBe(true);
  expect(seen.has('custom')).toBe(false);
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

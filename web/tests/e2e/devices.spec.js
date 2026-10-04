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
  await expect(control(page, 'AC frequency').locator('select')).toBeDisabled();
  await expect(control(page, 'AC frequency').locator('.ctl-note')).toContainText('payload not yet captured');
});

test('AC timer: +1 h, +15 m, -15 m from the time left, a typed time, Cancel', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const t = control(page, 'AC timer');
  const box = t.locator('.ctl-timer');
  const secs = async () => {
    const [h, m, s] = (await box.inputValue()).split(':').map(Number);
    return h * 3600 + m * 60 + s;
  };
  await expect(box).toHaveValue('off');
  await t.getByRole('button', { name: '+1 h' }).click();
  await expect(t).toHaveClass(/is-pending/);
  await expect(t).not.toHaveClass(/is-pending/, { timeout: 6000 });
  await expect.poll(secs).toBeGreaterThan(3590);
  // it counts down between samples
  const a = await secs();
  await expect.poll(secs, { timeout: 4000 }).toBeLessThan(a);
  await t.getByRole('button', { name: '+15 m' }).click();
  await expect.poll(secs, { timeout: 6000 }).toBeGreaterThan(4480);
  await t.getByRole('button', { name: '\u221215 m' }).click();
  await expect.poll(secs, { timeout: 6000 }).toBeLessThan(3600);
  expect(await secs()).toBeGreaterThan(3570);
  // typed
  await box.fill('1h30m');
  await box.press('Enter');
  await expect.poll(secs, { timeout: 6000 }).toBeGreaterThan(5380);
  await box.fill('soon');
  await box.press('Enter');
  await expect(t.locator('.ctl-note')).toContainText('not a time');
  await expect.poll(secs).toBeGreaterThan(5300);
  await t.getByRole('button', { name: 'Cancel' }).click();
  await expect(box).toHaveValue('off', { timeout: 6000 });
  await expect(t).not.toHaveClass(/is-pending/, { timeout: 6000 });
});

test('a countdown that runs out switches its output off (mock), and -15 m stops at 0', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const car = control(page, 'Car output').locator('.ctl-switch');
  const t = control(page, 'DC timer');
  if (await car.textContent() !== 'On') { await car.click(); await expect(car).toHaveText('On', { timeout: 6000 }); }
  await t.locator('.ctl-timer').fill('3s');
  await t.locator('.ctl-timer').press('Enter');
  await expect(car).toHaveText('Off', { timeout: 10000 });
  await expect(t.locator('.ctl-timer')).toHaveValue('off');
  await t.getByRole('button', { name: '\u221215 m' }).click();   // from nothing: 0, not negative
  await expect(t).not.toHaveClass(/is-pending/, { timeout: 6000 });
  await expect(t.locator('.ctl-timer')).toHaveValue('off');
});

test('AC charge limit writes watts and follows the device', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const c = control(page, 'AC charge limit');
  const box = c.locator('input');
  await expect(box).toBeEnabled();
  await expect(box).toHaveValue('600');
  await box.fill('700');
  await box.press('Enter');
  await expect(c).toHaveClass(/is-pending/);
  await expect(c).not.toHaveClass(/is-pending/, { timeout: 6000 });
  await box.blur();
  await expect(box).toHaveValue('700');
  await box.fill('600');
  await box.press('Enter');
  await expect(c).not.toHaveClass(/is-pending/, { timeout: 6000 });
});

test('All Data: every field and what shows it; "Unclassified only" keeps the rest; binary as hex', async ({ page }) => {
  await ready(page);
  const id = CONTROLS.replace('.controls@', '.unclassified@');
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  const fields = page.locator('.dev-fields tbody tr td:first-child');
  const rowOf = (f) => page.locator('.dev-fields tbody tr').filter({ has: page.locator('td:first-child', { hasText: new RegExp(`^${f}$`) }) });
  await expect(fields.filter({ hasText: 'unknown_b1' })).toHaveCount(1);
  await expect(rowOf('battery_pct').locator('td').nth(2)).toHaveText('Battery');        // shown by a control
  await expect(rowOf('serial').locator('td').nth(2)).toHaveText('device identity');
  await expect(rowOf('unknown_b1')).toHaveClass(/is-unclassified/);
  expect(await page.evaluate((x) => window.__btgwApp.windows.get(x).def.title, id)).toMatch(/· All Data$/);
  await page.locator('.dev-only input').check();
  await expect(fields.filter({ hasText: /^battery_pct$/ })).toHaveCount(0);
  await expect(fields.filter({ hasText: 'unknown_b1' })).toHaveCount(1);
  await page.reload();                                                                   // remembered
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await expect(page.locator('.dev-only input')).toBeChecked();
  await expect(fields.filter({ hasText: /^battery_pct$/ })).toHaveCount(0);
  await expect(page.locator('.dev-fields tbody tr').filter({ hasText: 'capability_block' }).locator('td').nth(1)).toHaveText('02 01 01 01  (hex)');
});

test('AC output asks before switching off, and a No sends nothing', async ({ page }) => {
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), CONTROLS);
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  const ac = control(page, 'AC output').locator('.ctl-switch');
  await expect(ac).toHaveText('On');
  // asked in the app's own dialog (not the browser's, which can be suppressed)
  const dlg = page.locator('#confirm-dlg');
  await ac.click();
  await expect(dlg).toBeVisible();
  await expect(dlg.locator('.cf-text')).toContainText('AC outlet OFF');
  await expect(dlg.locator('.cf-cancel')).toBeFocused();            // Cancel is the default
  await page.keyboard.press('Enter');
  await expect(dlg).toBeHidden();
  await page.waitForTimeout(1500);
  await expect(ac).toHaveText('On');
  await ac.click();
  await dlg.locator('.cf-ok').click();
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
  expect(lines[1]).toBe('Field\tValue\tShown as\tChanged');
  expect(lines.some((l) => l.startsWith('unknown_b1\t'))).toBe(true);
});

test('two devices at once: each its own windows and tabs, named in the picker', async ({ page }) => {
  const SCP = 'anker-scp.controls@7CE913601FD8';
  await ready(page);
  await page.waitForFunction((ids) => ids.every((id) => window.__btgwApp.windows.has(id)), [CONTROLS, SCP]);
  const groups = await page.$$eval('#tiles .tile .pane-pick >> nth=0', (s) => [...s[0].querySelectorAll('optgroup')].map((g) => g.label));
  expect(groups).toEqual(expect.arrayContaining(['C1000', 'A2687']));
  await page.evaluate((id) => window.__btgwApp.show(id), SCP);
  await expect(control(page, 'USB-C1').locator('.ctl-readout')).toHaveText('20.08 V · 2.98 A · 59.8 W');
  await expect(control(page, 'USB-C3').locator('.ctl-readout')).toHaveText('idle');
  await expect(control(page, 'USB-C2 port').locator('.ctl-readout')).toHaveText('on · 60W cable');
  // the closed picker says whose Controls these are
  const picked = await page.evaluate((id) => {
    const t = [...document.querySelectorAll('#tiles .tile')].find((x) => x.dataset.pane === id);
    const s = t.querySelector('.pane-pick');
    return s.options[s.selectedIndex].textContent;
  }, SCP);
  expect(picked).toBe('A2687 · Controls');
  // tabbed: both devices' Controls are tabs of their own
  await page.evaluate((id) => window.__btgwApp.show(id), CONTROLS);
  await page.keyboard.press('Alt+Shift+KeyT');
  await page.evaluate((id) => window.__btgwApp.show(id), SCP);
  const tabs = await page.$$eval('.tab', (t) => t.map((x) => x.textContent.trim()));
  expect(tabs.some((t) => t.includes('C1000 · Controls'))).toBe(true);
  expect(tabs.some((t) => t.includes('A2687 · Controls'))).toBe(true);
});

test('A2687 Screensaver: a picture is cropped, sent in windows, and a resend is a cache hit', async ({ page }) => {
  const SS = 'anker-scp.screensaver@7CE913601FD8';
  await ready(page);
  await page.waitForFunction((id) => window.__btgwApp.windows.has(id), SS);
  await page.evaluate((id) => window.__btgwApp.show(id), SS);
  const win = page.locator('.ss');
  await expect(win.locator('.ss-send')).toBeDisabled();
  // a 320x200 picture drawn by the browser itself; it is cropped to 240x240
  const png = Buffer.from(await page.evaluate(() => {
    const c = document.createElement('canvas');
    c.width = 320; c.height = 200;
    const g = c.getContext('2d');
    g.fillStyle = '#3a6'; g.fillRect(0, 0, 320, 200);
    g.fillStyle = '#fff'; g.fillRect(100, 50, 120, 100);
    return c.toDataURL('image/png').split(',')[1];
  }), 'base64');
  await win.locator('.ss-file').setInputFiles({ name: 'p.png', mimeType: 'image/png', buffer: png });
  await expect(win.locator('.ss-info')).toContainText('B JPEG');
  await expect(win.locator('.ss-id')).not.toHaveValue('');
  await win.locator('.ss-send').click();
  await expect(win.locator('.ss-status')).toContainText('Sent in', { timeout: 15000 });
  await expect(win.locator('.ss-bar')).toHaveAttribute('value', '1');
  await win.locator('.ss-send').click();
  await expect(win.locator('.ss-status')).toContainText('already had this picture', { timeout: 15000 });
});

// The cell menu, as in GATT Events: a click (or Enter on the cell cursor)
// opens it with the whole value in a wrapping box that copies on a tap.
async function openUnclassified(page, controlsId) {
  const id = controlsId.replace('.controls@', '.unclassified@');
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await expect(page.locator(`.pane-device .dev-fields tbody tr`).first()).toBeVisible();
}
const unMenu = (page) => page.locator('#pane-popover .s-menu');

test('Unclassified: a click on a cell opens its menu; the value wraps and copies; the row copies', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await ready(page);
  await openUnclassified(page, CONTROLS);
  const row = page.locator('.dev-fields tbody tr').filter({ hasText: 'capability_block' });
  await row.locator('td').nth(1).click();
  await expect(unMenu(page)).toBeVisible();
  await expect(unMenu(page).locator('.sm-title')).toContainText('capability_block');
  const value = unMenu(page).locator('.sm-value');
  await expect(value).toHaveText('02 01 01 01  (hex)');
  expect(await value.evaluate((e) => getComputedStyle(e).whiteSpace)).toBe('pre-wrap');   // wraps
  await expect(row.locator('td').nth(1)).toHaveClass(/is-menu-cell/);
  // the cell stays put while open, though telemetry keeps arriving
  await page.waitForTimeout(1500);
  await expect(unMenu(page)).toBeVisible();
  await value.click();
  await expect(page.locator('.toast')).toContainText('Copied');
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe('02 01 01 01  (hex)');
  // Copy row: field, value, changed -- tab-separated
  await row.locator('td').first().click();
  await unMenu(page).getByRole('button', { name: 'Copy row' }).click();
  expect((await page.evaluate(() => navigator.clipboard.readText())).split('\t').slice(0, 2)).toEqual(['capability_block', '02 01 01 01  (hex)']);
});

test('Unclassified: from the keyboard -- Space into the table, l to the value, Enter opens the menu, Esc closes it', async ({ page }) => {
  await ready(page);
  await openUnclassified(page, CONTROLS);
  const wrap = page.locator('.pane-device .table-wrap').filter({ has: page.locator('.dev-fields') });
  await wrap.focus();
  await page.keyboard.press('Space');
  await page.keyboard.press('KeyJ');
  await page.keyboard.press('KeyL');
  await page.keyboard.press('Enter');
  await expect(unMenu(page)).toBeVisible();
  await expect(unMenu(page).locator('.sm-value')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(unMenu(page)).toBeHidden();
});

test('the A2687\'s Unclassified window has the same cell menu', async ({ page }) => {
  await ready(page);
  // (the mock registers the A2687 a moment after load)
  const handle = await page.waitForFunction(() => [...window.__btgwApp.windows.entries()]
    .find(([k, w]) => /unclassified@/.test(k) && /^A2687 · /.test(w.def.title || ''))?.[0]);
  const id = await handle.jsonValue();
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await expect(page.locator('.dev-fields tbody tr').first()).toBeVisible();
  await page.locator('.dev-fields tbody tr').first().locator('td').nth(1).click();
  await expect(unMenu(page).locator('.sm-value')).toBeVisible();
});

// --- the device windows from the keyboard alone ---------------------------------
const A2687 = 'anker-scp.controls@7CE913601FD8';
async function focusWindow(page, id) {
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  await page.locator('#tiles .tile[data-active]').focus();       // as Alt+HJKL leaves it
}
const active = (page) => page.evaluate(() => {
  const a = document.activeElement;
  return { cls: a.className, text: (a.textContent || a.value || '').trim(), menu: !!document.querySelector('#pane-popover .s-menu:not([hidden])') && !document.getElementById('pane-popover').hidden };
});

test('Controls from the keyboard: j reaches the readouts; Enter opens the value menu; Esc returns; on to the switches', async ({ page }) => {
  await ready(page);
  await focusWindow(page, CONTROLS);
  await page.keyboard.press('KeyJ');
  let a = await active(page);
  expect(a.cls).toContain('ctl-readout');                          // Battery, the first control
  await page.keyboard.press('Enter');
  await expect(page.locator('#pane-popover .s-menu .sm-title')).toContainText('C1000 · Battery');
  await expect(page.locator('#pane-popover .s-menu .sm-value')).toHaveText(/%/);
  await page.keyboard.press('Escape');
  await expect(page.locator('#pane-popover')).toBeHidden();
  a = await active(page);
  expect(a.cls).toContain('ctl-readout');                          // back where it was
  for (let i = 0; i < 8; i++) await page.keyboard.press('KeyJ');   // down past the readouts
  a = await active(page);
  expect(a.cls).toContain('ctl-switch');                           // Car output
});

test('the A2687\'s Controls (all readouts) are keyboard stops too: j does not fall out into the title bar', async ({ page }) => {
  await ready(page);
  await focusWindow(page, A2687);
  await page.keyboard.press('KeyJ');
  expect((await active(page)).cls).toContain('ctl-readout');
  await page.keyboard.press('KeyJ');
  await page.keyboard.press('Space');                              // Space opens it too
  await expect(page.locator('#pane-popover .s-menu .sm-title')).toContainText('A2687 · USB-C2');
  await page.keyboard.press('Escape');
});

test('All Data from the keyboard: j lands on the table (not its header buttons); Space, j l, Enter: the cell menu', async ({ page }) => {
  await ready(page);
  await focusWindow(page, CONTROLS.replace('.controls@', '.unclassified@'));
  const seen = [];
  for (let i = 0; i < 4; i++) {
    await page.keyboard.press('KeyJ');
    seen.push((await active(page)).cls);
  }
  expect(seen.some((c) => /col-sort/.test(c))).toBe(false);        // the header is not a stop
  expect(seen).toContain('table-wrap');
  await page.locator('.pane-device .table-wrap').filter({ has: page.locator('.dev-fields') }).focus();
  await page.keyboard.press('Space');
  await page.keyboard.press('KeyJ');
  await page.keyboard.press('KeyL');
  await page.keyboard.press('Enter');
  await expect(page.locator('#pane-popover .s-menu .sm-value')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#pane-popover')).toBeHidden();
  // still in the table: the next j moves its cursor
  await expect(page.locator('.pane-device .table-wrap[data-nav-active]')).toHaveCount(1);
});

test('the Screensaver window: j reaches its controls', async ({ page }) => {
  await ready(page);
  await focusWindow(page, A2687.replace('.controls@', '.screensaver@'));
  await page.keyboard.press('KeyJ');
  expect((await active(page)).cls).toContain('ss-choose');
});

test('All Data columns, as every table\'s: drag to reorder, drag the edge to resize, Columns hides one', async ({ page }) => {
  await ready(page);
  const id = CONTROLS.replace('.controls@', '.unclassified@');
  await page.waitForFunction((x) => window.__btgwApp.windows.has(x), id);
  await page.evaluate((x) => window.__btgwApp.show(x), id);
  const heads = () => page.locator('.dev-fields thead th').evaluateAll((ths) => ths.map((t) => t.textContent.trim()).filter(Boolean));
  expect((await heads()).slice(0, 4)).toEqual(['Field', 'Value', 'Shown as', 'Changed']);
  // resize: drag Value's right edge
  const valueTh = page.locator('.dev-fields thead th', { hasText: 'Value' });
  const w0 = (await valueTh.boundingBox()).width;
  const edge = valueTh.locator('.col-resize');
  const eb = await edge.boundingBox();
  await page.mouse.move(eb.x + eb.width / 2, eb.y + eb.height / 2);
  await page.mouse.down();
  await page.mouse.move(eb.x + 80, eb.y + eb.height / 2, { steps: 5 });
  await page.mouse.up();
  expect((await valueTh.boundingBox()).width).toBeGreaterThan(w0 + 50);
  // reorder: drag 'Changed' left onto 'Value'
  const ch = await page.locator('.dev-fields thead th', { hasText: 'Changed' }).locator('.col-sort').boundingBox();
  const vb = await valueTh.boundingBox();
  await page.mouse.move(ch.x + ch.width / 2, ch.y + ch.height / 2);
  await page.mouse.down();
  await page.mouse.move(vb.x + 10, vb.y + vb.height / 2, { steps: 8 });
  await page.mouse.up();
  expect((await heads()).indexOf('Changed')).toBeLessThan((await heads()).indexOf('Shown as'));
  // hide one in the Columns dialog
  await page.locator('.dev-cols').click();
  const dlg = page.locator('[id^="alldata-columns-"]');
  await expect(dlg).toBeVisible();
  await dlg.getByLabel('Shown as').uncheck();
  expect(await heads()).not.toContain('Shown as');
});

test('All Data: a cell is marked only while its menu is open, however many are opened', async ({ page }) => {
  await ready(page);
  await openUnclassified(page, CONTROLS);
  const cells = page.locator('.dev-fields tbody tr td:nth-child(2)');
  for (let i = 0; i < 4; i++) {
    await cells.nth(i).click();
    await expect(unMenu(page)).toBeVisible();
    await expect(page.locator('.dev-fields td.is-menu-cell')).toHaveCount(1);
    await page.keyboard.press('Escape');
    await expect(unMenu(page)).toBeHidden();
  }
  await page.waitForTimeout(1500);                                   // telemetry redraws meanwhile
  await expect(page.locator('.dev-fields td.is-menu-cell')).toHaveCount(0);
  // closed by clicking another cell: the mark moves, it is not left behind
  await cells.nth(0).click();
  const fieldCell = page.locator('.dev-fields tbody tr td:nth-child(1)').nth(6);    // clear of the menu
  await fieldCell.click();
  await expect(page.locator('.dev-fields td.is-menu-cell')).toHaveCount(1);
  await expect(fieldCell).toHaveClass(/is-menu-cell/);
});

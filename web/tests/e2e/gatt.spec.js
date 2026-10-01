// The GATT window: per-characteristic panels with only what each permits,
// formats remembered per UUID, the shared events log, UUID display, keys,
// MTU, and the events split in tabbed view.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const HRM = '7C:E9:13:60:1F:D8';      // overwritten below from the mock's list

// The mock keeps connections per websocket (a real gateway keeps them across
// page loads), so after a reload this connects again.
async function connectFirst(page) {
  const addr = await page.evaluate(async () => {
    const r = await window.__btgwApp.ws.call('scan.list');
    const d = r.devices.find((x) => x.connectable);
    await window.__btgwApp.ws.call('dev.connect', { device: d.addr });
    window.__btgwApp.show('gatt');
    return d.addr;
  });
  await expect(page.locator('.t-svc').first()).toHaveText(/Generic Access/);
  return addr || HRM;
}

async function connected(page) {
  await ready(page);
  return connectFirst(page);
}

const row = (page, id) => page.locator(`.t-chr[data-id="${id}"]`);
const panel = (page, id) => row(page, id).locator('xpath=following-sibling::*[1][contains(@class,"t-panel")]');
const writes = (page) => page.evaluate(async () => (await window.__btgwApp.ws.call('test.gatt_writes')).writes);

test('a panel offers only what the characteristic permits', async ({ page }) => {
  await connected(page);
  for (const id of ['0.0', '3.0', '3.1', '3.2']) await row(page, id).click();
  const btns = (id) => panel(page, id).locator('.tp-actions button').allTextContents();
  // 2a00: read only
  expect(await btns('0.0')).toEqual(['Read']);
  await expect(panel(page, '0.0').locator('.tp-write')).toBeHidden();
  await expect(panel(page, '0.0').locator('.tp-perms')).toHaveText('Read');
  // read + write + write without response: Read, a write box, the response choice
  expect(await btns('3.0')).toEqual(['Read']);
  await expect(panel(page, '3.0').locator('.tp-write')).toBeVisible();
  await expect(panel(page, '3.0').locator('.tp-resp')).toBeVisible();
  // write without response only: the write box, no choice to make, no Read
  expect(await btns('3.1')).toEqual([]);
  await expect(panel(page, '3.1').locator('.tp-write')).toBeVisible();
  await expect(panel(page, '3.1').locator('.tp-resp')).toBeHidden();
  // indicate only
  expect(await btns('3.2')).toEqual(['Indicate: off']);
  await expect(panel(page, '3.2').locator('.tp-perms')).toHaveText('Indicate');
});

test('Read shows the value in the remembered format, here and in GATT Events', async ({ page }) => {
  await connected(page);
  await row(page, '0.0').click();
  const p = panel(page, '0.0');
  await p.getByRole('button', { name: 'Read' }).click();
  await expect(p.locator('.tp-val')).toHaveText('mock');          // Device Name starts as a string
  await expect(p.locator('.tp-meta')).toContainText('read · 4 B');
  await expect(row(page, '0.0').locator('.t-val')).toHaveText('mock');
  await p.locator('.tp-fmt').selectOption('hex');
  await expect(p.locator('.tp-val')).toHaveText('6d 6f 63 6b');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await connectFirst(page);
  await row(page, '0.0').click();
  await panel(page, '0.0').getByRole('button', { name: 'Read' }).click();
  await expect(panel(page, '0.0').locator('.tp-val')).toHaveText('6d 6f 63 6b');   // remembered
  await page.evaluate(() => window.__btgwApp.show('gatt-events'));
  await expect(page.locator('.ge-table tbody tr').first()).toContainText('6d 6f 63 6b');
  await expect(page.locator('.ge-table tbody tr').first()).toContainText('read');
});

test('writes parse in the chosen format, check it, and honour "with response"', async ({ page }) => {
  await connected(page);
  await row(page, '3.0').click();
  const p = panel(page, '3.0');
  await p.locator('.tp-fmt').selectOption('dec');
  await p.locator('.tp-in').fill('1 300');
  await p.getByRole('button', { name: 'Write' }).click();
  await expect(p.locator('.tp-err')).toContainText('not a byte');
  await p.locator('.tp-in').fill('1 160');
  await p.getByRole('button', { name: 'Write' }).click();
  await expect.poll(async () => (await writes(page)).at(-1)).toEqual({ id: '3.0', value: '01a0', response: true });
  await p.locator('.tp-resp-cb').uncheck();
  await p.locator('.tp-in').fill('2');
  await p.getByRole('button', { name: 'Write' }).click();
  await expect.poll(async () => (await writes(page)).at(-1)).toEqual({ id: '3.0', value: '02', response: false });
  // write-without-response only: never "with response"
  await row(page, '3.1').click();
  await panel(page, '3.1').locator('.tp-in').fill('ab');
  await panel(page, '3.1').getByRole('button', { name: 'Write' }).click();
  await expect.poll(async () => (await writes(page)).at(-1)).toEqual({ id: '3.1', value: 'ab', response: false });
});

test('Notify and Indicate are on/off toggles, and values arrive', async ({ page }) => {
  await connected(page);
  await row(page, '1.0').click();
  const n = panel(page, '1.0').getByRole('button', { name: /Notify/ });
  await n.click();
  await expect(n).toHaveText('Notify: on');
  await expect(panel(page, '1.0').locator('.tp-meta')).toContainText('notify', { timeout: 5000 });
  await n.click();
  await expect(n).toHaveText('Notify: off');
  await row(page, '3.2').click();
  const ind = panel(page, '3.2').getByRole('button', { name: /Indicate/ });
  await ind.click();
  await expect(panel(page, '3.2').locator('.tp-meta')).toContainText('indicate', { timeout: 5000 });
});

test('128-bit UUIDs: abbreviated, whole on click with Copy, or always with UUID-128', async ({ page }) => {
  await connected(page);
  const svc = page.locator('.t-svc[data-key="svc:3"] .t-uuid');
  await expect(svc).toHaveText('12345678…');
  await expect(svc).toHaveAttribute('title', '12345678-1234-5678-9abc-def012345678');
  await svc.click();
  await expect(page.locator('#pane-popover .g-uuid-full')).toHaveText('12345678-1234-5678-9abc-def012345678');
  await page.keyboard.press('Escape');
  await page.click('.g-u128');
  await expect(svc).toHaveText('12345678-1234-5678-9abc-def012345678');
  await expect(page.locator('.t-svc[data-key="svc:2"] .t-uuid')).toHaveText('180f Battery');   // 16-bit unchanged
});

test('keyboard: move, open, into the panel, Esc back to the row', async ({ page }) => {
  await connected(page);
  await page.locator('.gatt-tree').focus();
  await page.keyboard.press('Space');                     // into the list
  await page.keyboard.press('KeyJ');                      // 0.0
  await expect(page.locator('.t-row[aria-selected="true"]')).toHaveAttribute('data-key', 'chr:0.0');
  await page.keyboard.press('Enter');
  await expect(row(page, '0.0')).toHaveAttribute('aria-expanded', 'true');
  await page.keyboard.press('KeyL');                      // into its controls
  await expect(panel(page, '0.0').locator('.tp-fmt')).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.locator('.gatt-tree')).toBeFocused();
  await expect(page.locator('.gatt-tree')).toHaveAttribute('data-nav-active', '');
  await page.keyboard.press('KeyH');                      // close
  await expect(row(page, '0.0')).toHaveAttribute('aria-expanded', 'false');
});

test('MTU: the connection\'s, and what new connections ask for', async ({ page }) => {
  await connected(page);
  await expect(page.locator('.g-mtu')).toHaveText('MTU 255');
  await page.click('.g-mtu');
  const m = page.locator('#pane-popover');
  await expect(m.locator('.g-mtu-now')).toContainText('This connection: 255');
  await expect(m.locator('.g-mtu-in')).toHaveValue('255');
  await m.locator('.g-mtu-in').fill('185');
  await m.locator('.g-mtu-save').click();
  await expect(m.locator('.g-mtu-msg')).toContainText('new connections ask for 185');
});

test('tabbed view: an Events split with a resize handle and an axis switch', async ({ page }) => {
  await connected(page);
  await expect(page.locator('.g-ev')).toBeHidden();             // tiled: use the GATT Events window
  await page.keyboard.press('Alt+Shift+KeyT');
  await page.evaluate(() => window.__btgwApp.show('gatt'));
  await expect(page.locator('.g-ev')).toBeVisible();
  await page.click('.g-ev');
  await expect(page.locator('.gatt-events-host')).toBeVisible();
  await expect(page.locator('.g-split')).toBeVisible();
  await expect(page.locator('.gatt-body')).toHaveAttribute('data-axis', 'x');
  const tree = page.locator('.gatt-tree');
  const w0 = (await tree.boundingBox()).width;
  const h = await page.locator('.g-split').boundingBox();
  await page.mouse.move(h.x + h.width / 2, h.y + h.height / 2);
  await page.mouse.down();
  await page.mouse.move(h.x + h.width / 2 - 100, h.y + h.height / 2, { steps: 5 });
  await page.mouse.up();
  const w1 = (await tree.boundingBox()).width;
  expect(Math.round(w0 - w1)).toBeGreaterThan(80);
  await page.click('.g-axis');
  await expect(page.locator('.gatt-body')).toHaveAttribute('data-axis', 'y');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.evaluate(() => window.__btgwApp.show('gatt'));
  await expect(page.locator('.gatt-events-host')).toBeVisible();
  await expect(page.locator('.gatt-body')).toHaveAttribute('data-axis', 'y');
});

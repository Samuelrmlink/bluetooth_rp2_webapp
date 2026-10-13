// Settings -> Ethernet: status, DHCP/Manual, validation, keyboard reach.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

async function ethernet(page) {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-ethernet'));
}

test('the section is there because sys.info lists eth, and shows the lease', async ({ page }) => {
  await ethernet(page);
  const st = page.locator('.eth-status');
  await expect(st).toContainText('up · 192.168.50.23');
  await expect(st).toContainText('100 Mb/s full duplex');
  await expect(page.locator('.eth-manual')).toBeHidden();
});

test('Manual shows the fields, rejects a gateway off the subnet, then applies', async ({ page }) => {
  await ethernet(page);
  await page.getByLabel('Manual').check();
  await expect(page.locator('.eth-manual')).toBeVisible();
  await page.getByLabel('IP address').fill('10.1.2.3');
  await page.getByLabel('Subnet mask').fill('255.255.255.0');
  await page.getByLabel('Gateway').fill('10.9.9.1');
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.locator('.eth-note')).toHaveText('gateway is not on that subnet');
  await page.getByLabel('Gateway').fill('10.1.2.1');
  await page.getByRole('button', { name: 'Apply' }).click();
  await expect(page.locator('.eth-note')).toHaveText('applied: 10.1.2.3');
  await expect(page.locator('.eth-status')).toContainText('10.1.2.3 / 255.255.255.0');
  const st = await page.evaluate(() => window.__btgwApp.ws.call('eth.status'));
  expect(st.mode).toBe('manual');
  expect(st.cfg.gw).toBe('10.1.2.1');
});

test('reachable by keyboard alone: l to Manual, Space, j into the fields, Enter applies', async ({ page }) => {
  await ethernet(page);
  await page.getByLabel('DHCP').focus();
  await page.keyboard.press('l');                   // the app's nav: next control to the right
  await expect(page.getByLabel('Manual')).toBeFocused();
  await page.keyboard.press('Space');
  await expect(page.getByLabel('Manual')).toBeChecked();
  await page.keyboard.press('j');                   // down into the form
  await expect(page.getByLabel('IP address')).toBeFocused();
  await page.keyboard.type('192.168.60.5');
  await page.keyboard.press('Tab');
  await page.keyboard.type('255.255.255.0');
  await page.keyboard.press('Enter');
  await expect(page.locator('.eth-note')).toHaveText('applied: 192.168.60.5');
});

test('no module fitted reads "not present"; unplugged reads "no link"', async ({ page }) => {
  await ethernet(page);
  await page.evaluate(() => window.__btgwApp.ws.call('test.eth', { set: { link: false } }));
  await expect(page.locator('.eth-status')).toContainText('no link (cable?)');
  await page.evaluate(() => window.__btgwApp.ws.call('test.eth', { set: { present: false } }));
  await expect(page.locator('.eth-status')).toContainText('not present');
});

test('hidden on a build without the module', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => { window.__btgwApp.sysInfo.modules = ['sys', 'bt']; for (const s of window.__btgwApp.sections) s.onConnect?.(); });
  await expect(page.locator('#sect-ethernet')).toBeHidden();
});

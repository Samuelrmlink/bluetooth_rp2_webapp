// Settings -> WiFi: the saved "Bluetooth during join" (coex) setting.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

async function wifi(page) {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-wifi'));
}

test.afterEach(async ({ page }) => {
  // one mock for the whole run: hand the default back
  await page.evaluate(() => window.__btgwApp.ws.call('wifi.mode', { coex: 'window' })).catch(() => {});
});

test('Bluetooth during join shows the device setting and saves a change', async ({ page }) => {
  await wifi(page);
  const sel = page.getByLabel('Bluetooth during join');
  await expect(sel).toHaveValue('window');
  await expect(page.locator('.w-status')).toContainText('4.2 s with 1 Bluetooth link(s) up');
  await sel.selectOption('live');
  await expect(page.locator('.w-note')).toHaveText('joins keep Bluetooth links up');
  const st = await page.evaluate(() => window.__btgwApp.ws.call('wifi.status'));
  expect(st.coex).toBe('live');
  // a reload reads it back from the device, not from the page
  await page.reload();     // not ready(): that resets the mock
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-wifi'));
  await expect(page.getByLabel('Bluetooth during join')).toHaveValue('live');
});

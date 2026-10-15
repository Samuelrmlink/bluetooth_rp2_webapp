// Settings -> Uplinks: metrics, policy, manual switch, probe target, keyboard.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

async function uplinks(page) {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-uplinks'));
}

test('lists each uplink with its metrics and marks the active one', async ({ page }) => {
  await uplinks(page);
  const list = page.locator('.up-list');
  await expect(list).toContainText('w0 (wifi) 192.168.1.50');
  await expect(list).toContainText('● ok · 21 ms ±3 · 0% loss · metric 27');
  await expect(list).toContainText('e0 (ethernet)');
  await expect(page.locator('.up-summary')).toContainText('203.0.113.7 (wireguard)');
});

test('Use e0 now switches and the history says so', async ({ page }) => {
  await uplinks(page);
  await page.getByRole('button', { name: 'Use e0 now' }).click();
  await expect(page.locator('.up-note')).toHaveText('switched to e0');
  await expect(page.locator('.up-summary')).toContainText('last w0 → e0 (manual)');
  await expect(page.locator('.up-summary')).toContainText('Active');
});

test('policy select offers fixed per interface and applies', async ({ page }) => {
  await uplinks(page);
  await page.locator('#up-policy').selectOption('failover');
  await expect(page.locator('.up-note')).toHaveText('policy failover');
  expect((await page.evaluate(() => window.__btgwApp.ws.call('uplink.status'))).policy).toBe('failover');
  await page.locator('#up-policy').selectOption('fixed:e0');
  await expect(page.locator('.up-summary')).toContainText('→ e0 (fixed)');
});

test('probe target is validated, saved, and cleared back to the endpoint', async ({ page }) => {
  await uplinks(page);
  await page.getByLabel('Probe target').fill('1.2.3');
  await page.locator('#sect-uplinks').getByRole('button', { name: 'Save' }).click();
  await expect(page.locator('.up-note')).toHaveText('probe target is not an IPv4 address');
  await page.getByLabel('Probe target').fill('9.9.9.9');
  await page.locator('#sect-uplinks').getByRole('button', { name: 'Save' }).click();
  await expect(page.locator('.up-summary')).toContainText('9.9.9.9 (configured)');
  await page.getByLabel('Probe target').fill('');
  await page.locator('#sect-uplinks').getByRole('button', { name: 'Save' }).click();
  await expect(page.locator('.up-summary')).toContainText('(wireguard)');
});

test('reachable by keyboard alone: the switch buttons and Space', async ({ page }) => {
  await uplinks(page);
  await page.getByRole('button', { name: 'Use w0 now' }).focus();
  await page.keyboard.press('l');
  await expect(page.getByRole('button', { name: 'Use e0 now' })).toBeFocused();
  await page.keyboard.press('Space');
  await expect(page.locator('.up-note')).toHaveText('switched to e0');
});

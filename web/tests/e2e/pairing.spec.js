// Pairing: the method in Settings -> Bluetooth, and the prompts that come up
// when a device needs a passkey or a comparison (js/pairing.js).
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const dlg = (page) => page.locator('#pairing-dlg');
const ask = (page, p) => page.evaluate((p) => window.__btgwApp.ws.call('test.pairing', p), p);
const answers = (page) => page.evaluate(async () => (await window.__btgwApp.ws.call('test.answers')).answers);

test('Settings -> Bluetooth sets the pairing method and MITM, and explains them', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+KeyS');
  await page.evaluate(() => window.__btgwApp.settingsModal.expand('sect-bluetooth'));
  const io = page.locator('#bt-io');
  await expect(io).toHaveValue('no_input_no_output');
  await expect(page.locator('.bt-io-note')).toContainText('Just Works');
  await io.selectOption('keyboard_display');
  await page.locator('#bt-mitm').check();
  await expect.poll(() => page.evaluate(async () => {
    const r = await window.__btgwApp.ws.call('bond.io');
    return `${r.io_capability} ${r.mitm}`;
  })).toBe('keyboard_display true');
  await expect(page.locator('.bt-io-note')).toContainText('MITM protection is requested');
});

test('a passkey request: typed and sent, and the prompt closes on success', async ({ page }) => {
  await ready(page);
  await ask(page, { action: 'input', device: 'C4:11:22:33:44:55' });
  await expect(dlg(page)).toBeVisible();
  await expect(page.locator('.pair-pin')).toBeFocused();
  await page.keyboard.type('12345a');
  await page.locator('.pair-ok').click();
  await expect(page.locator('.pair-note')).toHaveText('Six digits, please.');
  await page.locator('.pair-pin').fill('012345');
  await page.keyboard.press('Enter');
  await expect(dlg(page)).toBeHidden();
  expect(await answers(page)).toEqual([{ op: 'bond.passkey', device: 'C4:11:22:33:44:55', passkey: 12345 }]);
  await expect(page.locator('.toast')).toContainText('Paired with C4:11:22:33:44:55');
});

test('a numeric comparison: the number shown, Reject refuses', async ({ page }) => {
  await ready(page);
  await ask(page, { action: 'compare', number: 42, device: 'C4:11:22:33:44:55' });
  await expect(page.locator('.pair-number')).toHaveText('000042');
  await expect(page.locator('.pair-yes')).toBeVisible();
  await page.locator('.pair-no').click();
  await expect(dlg(page)).toBeHidden();
  expect((await answers(page)).at(-1)).toMatchObject({ op: 'bond.confirm', accept: false });
});

test('a passkey to show: closing the prompt cancels pairing', async ({ page }) => {
  await ready(page);
  await ask(page, { action: 'display', number: 654321, device: 'C4:11:22:33:44:55' });
  await expect(page.locator('.pair-number')).toHaveText('654321');
  await expect(page.locator('.pair-input')).toBeHidden();
  await page.keyboard.press('Escape');
  await expect(dlg(page)).toBeHidden();
  expect((await answers(page)).at(-1)).toMatchObject({ op: 'bond.passkey', cancel: true });
});

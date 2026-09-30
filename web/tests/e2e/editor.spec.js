// The editor's four states, Save vs Run, vim, and undo -- the walkthrough
// first done by hand on the bench gateway, against the mock device.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const state = (page) => page.locator('.ed-state');
const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);

test('grey -> modified -> saved -> loaded -> changed since -> loaded -> stopped', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.setPref('vim', false));
  page.on('dialog', (d) => d.accept('lua/zz_edit.lua'));
  await page.click('.ed-b-new');
  await page.click('.ed-input');
  await page.keyboard.type('-- v1\nwhile true do sys.sleep(1000) end\n');
  await expect(state(page)).toContainText('modified');

  await page.keyboard.press('Control+KeyS');
  await expect(state(page)).toHaveText('');
  await expect(row(page, 'lua/zz_edit.lua')).not.toHaveClass(/is-dirty/);

  await page.keyboard.press('F5');                       // draft = the saved text
  await expect(state(page)).toContainText('loaded', { timeout: 8000 });
  await expect(row(page, 'lua/zz_edit.lua').locator('.ft-arena')).toHaveClass(/ft-loaded/);

  await page.keyboard.press('Control+Home');
  await page.keyboard.type('-- v2\n');
  await expect(state(page)).toContainText('modified');
  await page.keyboard.press('Control+KeyS');             // running v1, file now v2
  await expect(state(page)).toContainText('changed since', { timeout: 8000 });
  await expect(row(page, 'lua/zz_edit.lua').locator('.ft-arena')).toHaveClass(/ft-stale/);

  await page.keyboard.press('F5');
  await expect(state(page)).toHaveText(/^\s*loaded$/, { timeout: 8000 });
  await page.keyboard.press('Shift+F5');
  await expect(state(page)).toHaveText('', { timeout: 8000 });
});

test('a script the arena is running shows as loaded when opened', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(state(page)).toContainText('loaded', { timeout: 8000 });
  await expect(page.locator('.ed-s-arena')).toContainText('arena');
});

test('vim: motions, dd then u restores exactly, and the Esc ladder', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  const ta = page.locator('.ed-input');
  await ta.focus();
  const orig = await ta.inputValue();
  await expect(page.locator('.ed-s-mode')).toHaveText('-- NORMAL --');
  await page.keyboard.press('KeyG'); await page.keyboard.press('KeyG');
  await page.keyboard.press('KeyD'); await page.keyboard.press('KeyD');
  expect((await ta.inputValue()).split('\n').length).toBe(orig.split('\n').length - 1);
  await page.keyboard.press('KeyU');
  expect(await ta.inputValue()).toBe(orig);
  await page.keyboard.press('KeyI');
  await expect(page.locator('.ed-s-mode')).toHaveText('-- INSERT --');
  await page.keyboard.type('XYZ', { delay: 80 });
  await page.keyboard.press('Escape');                   // insert -> normal
  await expect(page.locator('.ed-s-mode')).toHaveText('-- NORMAL --');
  await page.keyboard.press('KeyU');                     // the whole insert is one step
  expect(await ta.inputValue()).toBe(orig);
  await page.keyboard.press('Escape');                   // normal -> leave the text
  expect(await page.evaluate(() => document.activeElement?.classList.contains('ed-input'))).toBe(false);
});

test('undo groups a typed word even with the highlight redrawing between keys', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.setPref('vim', false));
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  const ta = page.locator('.ed-input');
  const orig = await ta.inputValue();
  await ta.focus();
  await page.keyboard.type('hello', { delay: 120 });
  await page.keyboard.press('Control+KeyZ');
  expect(await ta.inputValue()).toBe(orig);
});

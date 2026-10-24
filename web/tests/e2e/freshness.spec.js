// An open file learns about a write from another session. The check runs
// when the socket connects and about every 5s while the editor is showing.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const tab = (page, p) => page.locator(`.ed-ftab[data-path="${p}"]`);
const focusedTa = (page) => page.locator('.ed-group.is-focus .ed-input');
const notice = (page) => page.locator('.ed-notice');

async function editor(page) {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.setPref('vim', false));
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(tab(page, 'lua/anker.lua')).toBeVisible();
}

const put = (page, body) => page.evaluate(async (text) => {
  const r = await fetch('/api/fs/lua/anker.lua', { method: 'PUT', body: text });
  if (!r.ok) throw new Error('PUT ' + r.status);
}, body);

const quiet = (page) => page.evaluate(async () => {
  const ed = window.__btgwApp.windows.get('editor').win;
  if (ed._bootP) await ed._bootP;
  if (ed._freshFlight) await ed._freshFlight;
  clearInterval(ed._freshTimer);
  ed._freshTimer = 0;
});

const fresh = async (page) => {
  await quiet(page);
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win._freshAll());
};

test('a clean buffer picks up a write from another session', async ({ page }) => {
  await editor(page);
  const before = await focusedTa(page).inputValue();
  await put(page, before + '\n-- remote\n');
  await expect(notice(page)).toContainText('Updated lua/anker.lua', { timeout: 8000 });
  await expect(focusedTa(page)).toHaveValue(/-- remote/);
  await expect(tab(page, 'lua/anker.lua')).not.toHaveClass(/is-dirty/);
});

test('a dirty buffer keeps its edits until Reload is chosen', async ({ page }) => {
  await editor(page);
  await quiet(page);
  const before = await focusedTa(page).inputValue();
  await focusedTa(page).click();
  await page.keyboard.press('Control+Home');
  await page.keyboard.type('-- mine\n');
  await expect(tab(page, 'lua/anker.lua')).toHaveClass(/is-dirty/);
  await put(page, before + '\n-- remote\n');

  page.once('dialog', (d) => {
    expect(d.message()).toContain('changed on another device');
    d.dismiss();
  });
  await fresh(page);
  await expect(focusedTa(page)).toHaveValue(/^-- mine\n/);
  await expect(notice(page)).toContainText('Keeping your edits');

  let again = 0;
  const count = () => { again++; };
  page.on('dialog', count);
  await fresh(page);
  page.off('dialog', count);
  expect(again).toBe(0);

  page.once('dialog', (d) => d.accept());
  await put(page, before + '\n-- remote-2\n');
  await fresh(page);
  await expect(focusedTa(page)).toHaveValue(/-- remote-2/);
  await expect(notice(page)).toContainText('Updated lua/anker.lua');
});

test('a deleted file stays until the close is confirmed, and asks only once', async ({ page }) => {
  await editor(page);
  await quiet(page);
  await page.evaluate(async () => { await fetch('/api/fs/lua/anker.lua', { method: 'DELETE' }); });
  page.once('dialog', (d) => {
    expect(d.message()).toContain('deleted on another device');
    d.dismiss();
  });
  await fresh(page);
  await expect(tab(page, 'lua/anker.lua')).toBeVisible();

  let again = 0;
  const count = () => { again++; };
  page.on('dialog', count);
  await fresh(page);
  page.off('dialog', count);
  expect(again).toBe(0);
  await expect(tab(page, 'lua/anker.lua')).toBeVisible();
});

test('confirming the close removes a deleted file', async ({ page }) => {
  await editor(page);
  await quiet(page);
  await page.evaluate(async () => { await fetch('/api/fs/lua/anker.lua', { method: 'DELETE' }); });
  page.once('dialog', (d) => d.accept());
  await fresh(page);
  await expect(tab(page, 'lua/anker.lua')).toHaveCount(0);
});

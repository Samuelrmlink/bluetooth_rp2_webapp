// The editor's vim additions -- search with highlighting, : commands, visual
// mode with the clipboard -- plus Ctrl+H/L and the VIM on/off button.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const ta = (page) => page.locator('.ed-input');
const mode = (page) => page.locator('.ed-s-mode');
const notice = (page) => page.locator('.ed-notice');
const name = (page) => page.locator('.ed-name');

async function open(page, path) {
  await page.evaluate((p) => window.__btgwApp.openFile(p), path);
  await expect(name(page)).toHaveText(path);
  await ta(page).focus();
  await expect(mode(page)).toHaveText('-- NORMAL --');
}

async function keys(page, s) {
  for (const k of s.match(/<[^>]+>|./g)) {
    await page.keyboard.press({ '<Enter>': 'Enter', '<Esc>': 'Escape' }[k] || k);
  }
}

const fileText = async (page, path) => (await page.request.get(`/api/fs/${path}`)).text();

test('/ highlights every match as you type, n moves the current one, :noh clears', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  const text = await ta(page).inputValue();
  const count = text.split('run').length - 1;
  expect(count).toBeGreaterThan(1);

  await keys(page, '/run');
  await expect(page.locator('.ed-find mark')).toHaveCount(count);     // incremental, before Enter
  await keys(page, '<Enter>');
  await expect(mode(page)).toHaveText('-- NORMAL --');
  const first = await ta(page).evaluate((el) => el.selectionStart);
  expect(text.slice(first, first + 3)).toBe('run');
  await expect(page.locator('.ed-find mark.cur')).toHaveCount(1);

  await keys(page, 'n');
  const second = await ta(page).evaluate((el) => el.selectionStart);
  expect(second).toBe(text.indexOf('run', first + 1) >= 0 ? text.indexOf('run', first + 1) : text.indexOf('run'));
  await keys(page, 'N');
  expect(await ta(page).evaluate((el) => el.selectionStart)).toBe(first);

  await keys(page, ':noh<Enter>');
  await expect(page.locator('.ed-find mark')).toHaveCount(0);
  await keys(page, '/zzzz<Enter>');
  await expect(notice(page)).toHaveText('Pattern not found: zzzz');
});

test(':q refuses with unsaved changes, :q! closes and lands in the file list', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  await keys(page, 'x');                                    // modify
  await expect(page.locator('.ed-state')).toContainText('modified');
  await keys(page, ':q<Enter>');
  await expect(notice(page)).toContainText('E37: No write since last change');
  await expect(name(page)).toHaveText('lua/anker.lua');
  await keys(page, ':q!<Enter>');
  await expect(name(page)).toHaveText('No file open');
  expect(await page.evaluate(() => document.activeElement?.classList.contains('ed-files'))).toBe(true);
  // Discarded, not saved: the device still has the original.
  expect(await fileText(page, 'lua/anker.lua')).not.toBe('');
  await open(page, 'lua/anker.lua');
  await expect(page.locator('.ed-state')).not.toContainText('modified');
});

test(':e opens a new file, :wq writes and closes it; :w path writes a copy and stays', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  await keys(page, ':e lua/zz_new.lua<Enter>');
  await expect(name(page)).toHaveText('lua/zz_new.lua');
  await expect(notice(page)).toContainText('[New]');
  await keys(page, 'i');
  await page.keyboard.type('-- hello');
  await keys(page, '<Esc>:wq<Enter>');
  // :wq closes that file's tab; the tab it was opened from shows again
  await expect(name(page)).toHaveText('lua/anker.lua');
  await expect(page.locator('.ed-ftab[data-path="lua/zz_new.lua"]')).toHaveCount(0);
  expect(await fileText(page, 'lua/zz_new.lua')).toBe('-- hello');

  await open(page, 'lua/anker.lua');
  const orig = await ta(page).inputValue();
  await keys(page, ':w lua/zz_copy.lua<Enter>');
  await expect(notice(page)).toHaveText('"lua/zz_copy.lua" written');
  await expect(name(page)).toHaveText('lua/anker.lua');       // stays on this file
  expect(await fileText(page, 'lua/zz_copy.lua')).toBe(orig);
  await keys(page, ':nonsense<Enter>');
  await expect(notice(page)).toContainText('Not an editor command');
});

test('Ctrl+H moves into the file list, Ctrl+L back into the text', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  await page.keyboard.press('Control+KeyH');
  const files = page.locator('.ed-files');
  await expect(files).toBeFocused();
  await expect(files).toHaveAttribute('data-nav-active', '');
  const before = await page.locator('.ft-row[aria-selected="true"]').getAttribute('data-path');
  await page.keyboard.press('KeyJ');
  await expect(page.locator('.ft-row[aria-selected="true"]')).not.toHaveAttribute('data-path', before);
  await page.keyboard.press('Control+KeyL');
  await expect(ta(page)).toBeFocused();
  // and from inside the text, in insert mode too
  await keys(page, 'i');
  await page.keyboard.press('Control+KeyH');
  await expect(files).toBeFocused();
});

test('the VIM button turns vim keys off (a plain text box), and it is remembered', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  const btn = page.locator('.ed-b-keys');
  await expect(btn).toHaveAttribute('aria-pressed', 'true');
  await btn.click();
  await expect(btn).toHaveAttribute('aria-pressed', 'false');
  await expect(mode(page)).toHaveText('');
  await ta(page).focus();
  await page.keyboard.press('Control+Home');
  await page.keyboard.type('jk');
  expect((await ta(page).inputValue()).startsWith('jk')).toBe(true);   // typed, not motions
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await expect(page.locator('.ed-b-keys')).toHaveAttribute('aria-pressed', 'false');
});

test('a mouse drag is visual mode; y copies; a paste in normal mode inserts', async ({ page }) => {
  await ready(page);
  await open(page, 'lua/anker.lua');
  const box = await ta(page).boundingBox();
  // Line 2 (line 1 of anker.lua is just "--[[", too short to drag across).
  await page.mouse.move(box.x + 30, box.y + 30);
  await page.mouse.down();
  await page.mouse.move(box.x + 140, box.y + 30, { steps: 5 });
  await page.mouse.up();
  await expect(mode(page)).toHaveText('-- VISUAL --');
  await keys(page, 'y');
  await expect(mode(page)).toHaveText('-- NORMAL --');
  await expect(notice(page)).toContainText(/Yanked \d+ characters/);

  await keys(page, 'gg0');                                   // vim's cursor to the very start
  await ta(page).evaluate((el) => {
    const dt = new DataTransfer();
    dt.setData('text/plain', 'PASTED');
    el.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true }));
  });
  expect((await ta(page).inputValue()).slice(0, 8)).toBe('-PASTED-');   // after the cursor's character, as p
  await expect(mode(page)).toHaveText('-- NORMAL --');
});

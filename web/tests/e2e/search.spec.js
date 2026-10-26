// Search in files: the editor sidebar's Search tab, Ctrl+Shift+F, F4, and
// vim's :grep / :cn / :cp.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const focusedTa = (page) => page.locator('.ed-group.is-focus .ed-input');
const status = (page) => page.locator('.es-status');
const hits = (page) => page.locator('.es-hit');
const name = (page) => page.locator('.ed-name');
const selected = (page) => focusedTa(page).evaluate((t) => t.value.slice(t.selectionStart, t.selectionEnd));

async function editor(page, vim = false) {
  await ready(page);
  await page.evaluate((v) => window.__btgwApp.setPref('vim', v), vim);
  await page.evaluate(() => window.__btgwApp.openFile('lua/ac_pulse.lua'));
  await expect(focusedTa(page)).toHaveValue(/./);
}

test('Ctrl+Shift+F: the Search tab; results by file; a click opens the file with the match selected', async ({ page }) => {
  await editor(page);
  await focusedTa(page).focus();
  await page.keyboard.press('Control+Shift+KeyF');
  await expect(page.locator('.ed-search')).toBeVisible();
  await expect(page.locator('.ed-files')).toBeHidden();
  await expect(page.locator('.es-q')).toBeFocused();
  await page.keyboard.type('require');
  await page.keyboard.press('Enter');
  await expect(status(page)).toContainText(/\d+ match(es)? in \d+ files?/);
  await expect(page.locator('.es-file', { hasText: 'ac_pulse.lua' }).first()).toBeVisible();
  const hit = page.locator('.es-group', { has: page.locator('.es-file[title="lua/ac_pulse.lua"]') }).locator('.es-hit').first();
  await hit.click();
  await expect(name(page)).toHaveText('lua/ac_pulse.lua');
  await expect.poll(async () => (await selected(page)).toLowerCase()).toBe('require');
  // back to the file list with its tab
  await page.locator('.ed-tab[data-tab="files"]').click();
  await expect(page.locator('.ed-files')).toBeVisible();
});

test('F4 / Shift+F4 step through the results from the text', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.search.run({ q: 'local', dir: 'lua' }));
  await expect(status(page)).toContainText('match');
  await focusedTa(page).focus();
  await page.keyboard.press('F4');
  const first = await page.locator('.es-hit.is-cur').getAttribute('title');
  await page.keyboard.press('F4');
  const second = await page.locator('.es-hit.is-cur').getAttribute('title');
  expect(second).not.toBe(first);
  await page.keyboard.press('Shift+F4');
  expect(await page.locator('.es-hit.is-cur').getAttribute('title')).toBe(first);
  await expect.poll(async () => (await selected(page)).toLowerCase()).toBe('local');
});

test('what is searched: unsaved edits, .gz files as text, not cache/; options and a bad regex', async ({ page }) => {
  await editor(page);
  const w = (fn, a) => page.evaluate(([f, x]) => new Function('w', 'x', `return (${f})(w, x)`)(window.__btgwApp.windows.get('editor').win, x), [fn.toString(), a]);
  // an edit not saved yet is found in its file
  await focusedTa(page).focus();
  await page.keyboard.press('Control+Home');
  await page.keyboard.type('-- zebra_unsaved\n');
  await w((x) => x.sidebarTab('search'));
  await w((x) => x.search.run({ q: 'zebra_unsaved' }));
  await expect(status(page)).toHaveText(/^1 match in 1 file/);
  await expect(page.locator('.es-file')).toHaveAttribute('title', 'lua/ac_pulse.lua');
  // a compressed web file, as its text
  await w((x) => x.search.run({ q: 'answer = 42' }));
  await expect(page.locator('.es-file')).toHaveAttribute('title', 'www/demo.js.gz');
  // whole word, match case
  await page.locator('.es-word').click();
  await w((x) => x.search.run({ q: 'answer', dir: 'www' }));
  await expect(hits(page)).toHaveCount(1);
  await page.locator('.es-case').click();
  await w((x) => x.search.run({ q: 'ANSWER', dir: 'www' }));
  await expect(status(page)).toContainText('No matches');
  await page.locator('.es-case').click();
  await page.locator('.es-word').click();
  // a regex, and a broken one
  await page.locator('.es-re').click();
  await w((x) => x.search.run({ q: 'ans.er\\s=', dir: 'www' }));
  await expect(hits(page)).toHaveCount(1);
  await w((x) => x.search.run({ q: '(' }));
  await expect(status(page)).toContainText('Not a valid pattern');
});

test('vim: :grep opens the first match; :cn / :cp; :grep /regex/ folder', async ({ page }) => {
  await editor(page, true);
  await focusedTa(page).focus();
  await page.keyboard.type(':grep pinecil lua\n');
  await expect(status(page)).toContainText(/in lua\//);
  await expect(page.locator('.es-hit.is-cur')).toHaveCount(1);
  const a = await page.locator('.es-hit.is-cur').getAttribute('title');
  await focusedTa(page).focus();
  await page.keyboard.type(':cn\n');
  const b = await page.locator('.es-hit.is-cur').getAttribute('title');
  expect(b).not.toBe(a);
  await page.keyboard.type(':cp\n');
  expect(await page.locator('.es-hit.is-cur').getAttribute('title')).toBe(a);
  await page.keyboard.type(':grep /requ.re/ lua/profiles\n');
  await expect(status(page)).toContainText('in lua/profiles/');
  await expect(page.locator('.es-re')).toHaveAttribute('aria-pressed', 'true');
  await expect(name(page)).toHaveText(/^lua\/profiles\//);
});

test('the results from the keyboard: into the list, j, Enter opens', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.sidebarTab('search'));
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.search.run({ q: 'local', dir: 'lua' }));
  await expect(status(page)).toContainText('match');
  await page.locator('.es-results').focus();
  await page.keyboard.press('Space');                                // into the list
  await page.keyboard.press('KeyJ');
  await expect(page.locator('.es-hit.is-sel')).toHaveAttribute('data-i', '1');
  await page.keyboard.press('Enter');
  await expect(page.locator('.es-hit.is-cur')).toHaveAttribute('data-i', '1');
  await expect.poll(async () => (await selected(page)).toLowerCase()).toBe('local');
});

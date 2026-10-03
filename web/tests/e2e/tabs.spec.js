// Editor tabs and the split: mouse, keyboard (Alt+[ ] \ O Q), vim, touch.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const tab = (page, p, half = '.ed-group') => page.locator(`${half} .ed-ftab[data-path="${p}"]`);
const tabsOf = (page, half = 0) => page.locator('.ed-group').nth(half).locator('.ed-ftab')
  .evaluateAll((els) => els.map((e) => (e.classList.contains('is-active') ? `[${e.dataset.path}]` : e.dataset.path)));
const focusedTa = (page) => page.locator('.ed-group.is-focus .ed-input');
const name = (page) => page.locator('.ed-name');
const fileText = (page, p) => page.evaluate(async (path) => (await fetch(`/api/fs/${path}`)).text(), p);

async function editor(page, vim = false) {
  await ready(page);
  await page.evaluate((v) => window.__btgwApp.setPref('vim', v), vim);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(tab(page, 'lua/anker.lua')).toBeVisible();
}

const openFile = (page, p) => page.evaluate((x) => window.__btgwApp.openFile(x), p);

test('files open as tabs; a click switches; each tab keeps its cursor and undo', async ({ page }) => {
  await editor(page);
  await openFile(page, 'lua/pinecil.lua');
  expect(await tabsOf(page)).toEqual(['lua/anker.lua', '[lua/pinecil.lua]']);
  // an edit in pinecil, then away and back: the edit, the cursor and Ctrl+Z all survive
  const ta = focusedTa(page);
  await ta.click();
  await page.keyboard.press('Control+Home');
  await page.keyboard.type('-- tab test\n');
  await expect(tab(page, 'lua/pinecil.lua')).toHaveClass(/is-dirty/);
  await tab(page, 'lua/anker.lua').click();
  await expect(name(page)).toHaveText('lua/anker.lua');
  await expect(focusedTa(page)).not.toHaveValue(/-- tab test/);
  await tab(page, 'lua/pinecil.lua').locator('.ed-ftab-name').click();
  await expect(focusedTa(page)).toHaveValue(/^-- tab test\n/);
  const pos = await focusedTa(page).evaluate((t) => t.selectionStart);
  expect(pos).toBe('-- tab test\n'.length);
  await focusedTa(page).focus();
  for (let i = 0; i < 6; i++) await page.keyboard.press('Control+KeyZ');   // undone a word at a time
  await expect(focusedTa(page)).not.toHaveValue(/-- tab test/);
});

test('Alt+] / Alt+[ step through tabs, Alt+Shift+] moves one, Alt+Q closes (asking when modified)', async ({ page }) => {
  await editor(page);
  await openFile(page, 'lua/pinecil.lua');
  await openFile(page, 'lua/anker2.lua');
  expect(await tabsOf(page)).toEqual(['lua/anker.lua', 'lua/pinecil.lua', '[lua/anker2.lua]']);
  await focusedTa(page).focus();
  const before = await focusedTa(page).inputValue();
  await page.keyboard.press('Alt+BracketRight');                  // wraps to the first
  await expect(name(page)).toHaveText('lua/anker.lua');
  await page.keyboard.press('Alt+BracketLeft');
  await expect(name(page)).toHaveText('lua/anker2.lua');
  expect(await focusedTa(page).inputValue()).toBe(before);        // the keys typed nothing
  await page.keyboard.press('Alt+Shift+BracketLeft');
  expect(await tabsOf(page)).toEqual(['lua/anker.lua', '[lua/anker2.lua]', 'lua/pinecil.lua']);
  // a modified tab: Alt+Q asks; declining keeps it
  await page.keyboard.type('x');
  page.once('dialog', (d) => d.dismiss());
  await page.keyboard.press('Alt+KeyQ');
  expect(await tabsOf(page)).toEqual(['lua/anker.lua', '[lua/anker2.lua]', 'lua/pinecil.lua']);
  page.once('dialog', (d) => d.accept());
  await page.keyboard.press('Alt+KeyQ');
  expect(await tabsOf(page)).toEqual(['lua/anker.lua', '[lua/pinecil.lua]']);
  // the × closes too, without asking when clean
  await tab(page, 'lua/pinecil.lua').locator('.ed-ftab-x').click();
  expect(await tabsOf(page)).toEqual(['[lua/anker.lua]']);
});

test('Alt+\\ splits: the same file twice, edits mirrored; Alt+O moves between; Alt+\\ joins them back', async ({ page }) => {
  await editor(page);
  await focusedTa(page).focus();
  await page.keyboard.press('Alt+Backslash');
  await expect(page.locator('.ed-group')).toHaveCount(2);
  await expect(page.locator('.ed-groups')).toHaveClass(/is-split/);
  await expect(page.locator('.ed-b-split')).toHaveAttribute('aria-pressed', 'true');
  expect(await tabsOf(page, 0)).toEqual(['[lua/anker.lua]']);
  expect(await tabsOf(page, 1)).toEqual(['[lua/anker.lua]']);
  // the focus is the new half; typing there shows in the other
  await expect(page.locator('.ed-group').nth(1)).toHaveClass(/is-focus/);
  await focusedTa(page).focus();
  await page.keyboard.press('Control+Home');
  await page.keyboard.type('-- mirrored\n');
  await expect(page.locator('.ed-group').nth(0).locator('.ed-input')).toHaveValue(/^-- mirrored\n/);
  // another file opens in the focused half only
  await openFile(page, 'lua/pinecil.lua');
  expect(await tabsOf(page, 1)).toEqual(['lua/anker.lua', '[lua/pinecil.lua]']);
  expect(await tabsOf(page, 0)).toEqual(['[lua/anker.lua]']);
  await focusedTa(page).focus();
  await page.keyboard.press('Alt+KeyO');
  await expect(page.locator('.ed-group').nth(0)).toHaveClass(/is-focus/);
  await expect(name(page)).toHaveText('lua/anker.lua');
  // the flip toggles (a narrow editor starts stacked, a wide one side by side)
  const stacked = await page.locator('.ed-groups').evaluate((e) => e.classList.contains('ed-stacked'));
  await page.keyboard.press('Alt+Shift+Backslash');
  await expect.poll(() => page.locator('.ed-groups').evaluate((e) => e.classList.contains('ed-stacked'))).toBe(!stacked);
  await page.keyboard.press('Alt+Backslash');
  await expect(page.locator('.ed-group')).toHaveCount(1);
  expect(await tabsOf(page, 0)).toEqual(['[lua/anker.lua]', 'lua/pinecil.lua']);
  await expect(focusedTa(page)).toHaveValue(/^-- mirrored\n/);     // the edit is still there, unsaved
});

test('Alt+Shift+O moves a tab into the other half, and back (which unsplits)', async ({ page }) => {
  await editor(page);
  await openFile(page, 'lua/pinecil.lua');
  await focusedTa(page).focus();
  await page.keyboard.press('Alt+Shift+KeyO');
  expect(await tabsOf(page, 0)).toEqual(['[lua/anker.lua]']);
  expect(await tabsOf(page, 1)).toEqual(['[lua/pinecil.lua]']);
  await expect(name(page)).toHaveText('lua/pinecil.lua');
  await page.keyboard.press('Alt+Shift+KeyO');
  await expect(page.locator('.ed-group')).toHaveCount(1);
  expect(await tabsOf(page, 0)).toEqual(['lua/anker.lua', '[lua/pinecil.lua]']);
});

test('clicking into a half focuses it: the title, status and toolbar follow', async ({ page }) => {
  await editor(page);
  await page.locator('.ed-b-split').click();                       // the touch / mouse way
  await openFile(page, 'lua/pinecil.lua');
  await page.locator('.ed-group').nth(0).locator('.ed-input').click();
  await expect(name(page)).toHaveText('lua/anker.lua');
  await page.locator('.ed-group').nth(1).locator('.ed-input').click();
  await expect(name(page)).toHaveText('lua/pinecil.lua');
  // Ctrl+S saves the focused half's file
  await page.keyboard.press('Control+End');
  await page.keyboard.type('\n-- saved from the right half');
  await page.keyboard.press('Control+KeyS');
  await expect.poll(() => fileText(page, 'lua/pinecil.lua')).toContain('-- saved from the right half');
});

test('vim: gt / gT, :tabnew, :vs and :only, :tabc', async ({ page }) => {
  await editor(page, true);
  const ta = focusedTa(page);
  await ta.focus();                                               // a fresh file is in normal mode
  await page.keyboard.type(':tabnew lua/pinecil.lua\n');
  await expect(name(page)).toHaveText('lua/pinecil.lua');
  await page.keyboard.type('gt');
  await expect(name(page)).toHaveText('lua/anker.lua');
  await page.keyboard.type('gT');
  await expect(name(page)).toHaveText('lua/pinecil.lua');
  await page.keyboard.type('1gt');
  await expect(name(page)).toHaveText('lua/anker.lua');
  await page.keyboard.type(':vs lua/anker2.lua\n');
  await expect(page.locator('.ed-group')).toHaveCount(2);
  await expect(name(page)).toHaveText('lua/anker2.lua');
  await page.keyboard.type(':only\n');
  await expect(page.locator('.ed-group')).toHaveCount(1);
  await focusedTa(page).focus();
  await page.keyboard.type(':tabc\n');
  await expect(tab(page, 'lua/anker2.lua')).toHaveCount(0);
});

test('tabs and the split come back after a reload', async ({ page }) => {
  await editor(page);
  await openFile(page, 'lua/pinecil.lua');
  await page.locator('.ed-b-split').click();
  await openFile(page, 'lua/anker2.lua');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.evaluate(() => window.__btgwApp.show('editor'));
  await expect(page.locator('.ed-group')).toHaveCount(2);
  expect(await tabsOf(page, 0)).toEqual(['lua/anker.lua', '[lua/pinecil.lua]']);
  expect(await tabsOf(page, 1)).toEqual(['lua/pinecil.lua', '[lua/anker2.lua]']);
  await expect(name(page)).toHaveText('lua/anker2.lua');
  await expect(page.locator('.ed-group').nth(0).locator('.ed-input')).toHaveValue(/./);
});

test('the keys are listed in the help, and none clashes with another binding', async ({ page }) => {
  await ready(page);
  const clashes = await page.evaluate(async () => {
    const { BINDINGS } = await import('/js/keys.js');
    const seen = new Map(), out = [];
    for (const b of BINDINGS) {
      const when = Array.isArray(b.when) ? b.when : [b.when];
      for (const c of b.combos) {
        for (const w of when) {
          const k = `${c}@${w}`;
          if (seen.has(k) && seen.get(k) !== b.id) out.push(`${k}: ${seen.get(k)} / ${b.id}`);
          seen.set(k, b.id);
        }
      }
    }
    // the editor's new combos against every workspace one (the editor inherits them)
    const ws = new Set(BINDINGS.filter((b) => [b.when].flat().some((w) => ['workspace', 'always'].includes(w))).flatMap((b) => b.combos));
    // (an override must say so, and be of the binding it names)
    const help = BINDINGS.find((x) => x.id === 'help');
    for (const b of BINDINGS.filter((x) => x.id.startsWith('editor-'))) {
      for (const c of b.combos) {
        if (!ws.has(c)) continue;
        if (b.overrides === 'help' && help.combos.includes(c)) continue;
        out.push(`${c}: ${b.id} vs workspace`);
      }
    }
    return out;
  });
  expect(clashes).toEqual([]);
});

test.describe('touch', () => {
  test.use({ hasTouch: true, isMobile: true });

  test('a tap on a tab shows it; its × closes it; the Split button splits and joins', async ({ page }) => {
    await editor(page);
    await openFile(page, 'lua/pinecil.lua');
    await tab(page, 'lua/anker.lua').locator('.ed-ftab-name').tap();
    await expect(name(page)).toHaveText('lua/anker.lua');
    const x = tab(page, 'lua/pinecil.lua').locator('.ed-ftab-x');
    const box = await x.boundingBox();
    expect(box.width).toBeGreaterThanOrEqual(28);                  // finger-sized
    await x.tap();
    expect(await tabsOf(page)).toEqual(['[lua/anker.lua]']);
    await page.locator('.ed-b-split').tap();
    await expect(page.locator('.ed-group')).toHaveCount(2);
    await page.locator('.ed-b-split').tap();
    await expect(page.locator('.ed-group')).toHaveCount(1);
  });
});

test('the editor\'s own key list: Alt+/ from the text, the ? button, a filter, and the way to the main list', async ({ page }) => {
  await editor(page);
  await focusedTa(page).focus();
  await page.keyboard.press('Alt+Slash');
  const dlg = page.locator('#editor-help-dlg');
  await expect(dlg).toBeVisible();
  await expect(page.locator('#help-modal')).toBeHidden();
  for (const h of ['Save and run', 'Tabs and split', 'Files', 'Drag and drop', 'Vim']) {
    await expect(dlg.locator('h3', { hasText: h })).toBeVisible();
  }
  await expect(dlg.locator('.eh-filter')).toBeFocused();
  await page.keyboard.type('split');
  await expect(dlg.locator('tr[data-id="editor-split"]')).toBeVisible();
  await expect(dlg.locator('tr[data-id="editor-save"]')).toBeHidden();
  await page.keyboard.press('?');                                   // ? closes it, as it does the main list
  await expect(dlg).toBeHidden();
  await page.locator('.ed-b-help').click();
  await expect(dlg).toBeVisible();
  await dlg.getByRole('button', { name: 'All shortcuts…' }).click();
  await expect(dlg).toBeHidden();
  await expect(page.locator('#help-modal')).toBeVisible();
  // the main list points at the editor's, and a search for an editor key lands there
  await page.keyboard.type('alt+q');
  await expect(page.locator('#help-modal .keys tr:not([hidden])')).toHaveCount(1);
  await expect(page.locator('#help-modal .keys tr:not([hidden])')).toContainText('The editor has its own list');
});

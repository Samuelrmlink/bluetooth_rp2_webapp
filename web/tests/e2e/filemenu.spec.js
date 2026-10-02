// The editor sidebar's file menu: right-click, or m / Shift+F10 / the Menu
// key in the file list; the row it is for is marked; actions act on that
// file, open or not.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');

async function editor(page) {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(row(page, 'lua/anker.lua')).toBeVisible();
  // Let the layout settle (as filemenu-touch.spec.js): the mock's devices
  // register their panes about a second after load, and a layout change
  // closes any popover -- a menu opened just before would vanish mid-test.
  await page.waitForFunction(() => {
    const v = window.__btgwApp._layout.version;
    const s = window.__settle || (window.__settle = { v, t: performance.now() });
    if (s.v !== v) { s.v = v; s.t = performance.now(); }
    return performance.now() - s.t > 1500;
  }, null, { polling: 100 });
}

test('right-click: the file\'s menu, its row marked, Esc closes only the menu', async ({ page }) => {
  await editor(page);
  await row(page, 'lua/anker.lua').click({ button: 'right' });
  await expect(menu(page)).toBeVisible();
  await expect(menu(page).locator('.sm-title')).toHaveText('lua/anker.lua');
  await expect(menu(page).getByRole('button', { name: 'Start anker' })).toBeVisible();
  await expect(menu(page).getByRole('button', { name: 'Open' })).toHaveCount(0);   // it is the open file
  await expect(row(page, 'lua/anker.lua')).toHaveClass(/is-menu-row/);
  await page.keyboard.press('Escape');
  await expect(menu(page)).toBeHidden();
  await expect(row(page, 'lua/anker.lua')).not.toHaveClass(/is-menu-row/);
});

test('right-click a folder: New file here… starts in that folder', async ({ page }) => {
  await editor(page);
  await row(page, 'lua').click({ button: 'right' });
  await expect(menu(page).getByRole('button', { name: 'Upload here…' })).toBeVisible();
  const asked = [];
  page.once('dialog', (d) => { asked.push(d.defaultValue()); d.dismiss(); });
  await menu(page).getByRole('button', { name: 'New file here…' }).click();
  await expect.poll(() => asked[0]).toBe('lua/');
});

test('keyboard: m in the file list opens the menu; the hint bar says so; Download works', async ({ page }) => {
  await editor(page);
  await page.keyboard.press('Control+KeyH');                 // into the file list
  await expect(page.locator('.ed-files')).toHaveAttribute('data-nav-active', '');
  // (the hint bar appears once the page has seen keyboard navigation)
  await page.keyboard.press('KeyJ');
  await page.keyboard.press('KeyK');
  await expect(page.locator('#keyhint')).toContainText('menu');
  // back on anker.lua, wherever j/k left the cursor
  await page.evaluate(() => { const w = window.__btgwApp.windows.get('editor').win; w.tree.sel = 'lua/anker.lua'; w.tree.invalidate(); });
  await page.keyboard.press('KeyM');
  await expect(menu(page)).toBeVisible();
  const [dl] = await Promise.all([
    page.waitForEvent('download'),
    menu(page).getByRole('button', { name: 'Download' }).click(),
  ]);
  expect(dl.suggestedFilename()).toBe('anker.lua');
  // the keyboard is back in the list afterwards
  await expect(page.locator('.ed-files')).toHaveAttribute('data-nav-active', '');
  await page.keyboard.press('Shift+F10');
  await expect(menu(page)).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(menu(page)).toBeHidden();
});

test('rename a file that is not open, from its menu', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => fetch('/api/fs/lua/zz_menu.lua', { method: 'PUT', body: '-- x' }));
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.tree.refresh());
  await expect(row(page, 'lua/zz_menu.lua')).toBeVisible();
  page.once('dialog', (d) => d.accept('lua/zz_menu2.lua'));
  await row(page, 'lua/zz_menu.lua').click({ button: 'right' });
  await menu(page).getByRole('button', { name: 'Rename / move…' }).click();
  await expect(row(page, 'lua/zz_menu2.lua')).toBeVisible();
  await expect(page.locator('.ed-name')).toHaveText(/anker\.lua/);           // the open file is untouched
  page.once('dialog', (d) => d.accept());
  await row(page, 'lua/zz_menu2.lua').click({ button: 'right' });
  await menu(page).getByRole('button', { name: 'Delete…' }).click();
  await expect(row(page, 'lua/zz_menu2.lua')).toHaveCount(0);
});

test('another window scrolling (the Console logging) leaves the menu open; the file list scrolling closes it', async ({ page }) => {
  await editor(page);
  await row(page, 'lua/anker.lua').click({ button: 'right' });
  await expect(menu(page)).toBeVisible();
  await page.waitForTimeout(300);                                   // past the popover's 200 ms grace
  // something elsewhere scrolls, as the Console does when a line arrives
  await page.evaluate(() => {
    const other = document.createElement('div');
    other.style.cssText = 'position:fixed;left:0;bottom:0;width:50px;height:50px;overflow:auto';
    other.innerHTML = '<div style="height:500px"></div>';
    document.body.appendChild(other);
    other.scrollTop = 100;
    other.dispatchEvent(new Event('scroll'));
  });
  await page.waitForTimeout(200);
  await expect(menu(page)).toBeVisible();
  // the list the row is in scrolls: the menu would be left floating -- it closes
  await page.evaluate(() => document.querySelector('.ed-files').dispatchEvent(new Event('scroll')));
  await expect(menu(page)).toBeHidden();
});

test('sizes: a file\'s menu and the status bar say its size and flash use', async ({ page }) => {
  await editor(page);
  await row(page, 'lua/anker.lua').click({ button: 'right' });
  await expect(menu(page).locator('.sm-info')).toHaveText(/B · (.* on flash|stored inline)/);
  await page.keyboard.press('Escape');
  await expect(page.locator('.ed-s-size')).toHaveText(/B · /);
});

test('a folder: what is in it, Calculate size on request, Delete only when empty', async ({ page }) => {
  await editor(page);
  await row(page, 'lua').click({ button: 'right' });
  const info = menu(page).locator('.sm-info');
  await expect(info).toHaveText(/\d+ files?/);
  await expect(menu(page).getByRole('button', { name: 'Delete folder…' })).toHaveCount(0);   // not empty
  await menu(page).getByRole('button', { name: 'Calculate size' }).click();
  await expect(menu(page)).toBeVisible();                                                   // stays open
  await expect(info).toHaveText(/ in \d+ files.* on flash/);
  await page.keyboard.press('Escape');
  // an empty one can go
  await page.evaluate(() => window.__btgwApp.ws.call('test.mkdir', { path: 'lua/zz_empty' }));
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.tree.refresh());
  await expect(row(page, 'lua/zz_empty')).toBeVisible();
  await row(page, 'lua/zz_empty').click({ button: 'right' });
  await expect(menu(page).locator('.sm-info')).toHaveText('empty');
  page.once('dialog', (d) => d.accept());
  await menu(page).getByRole('button', { name: 'Delete folder…' }).click();
  await expect(row(page, 'lua/zz_empty')).toHaveCount(0);
});

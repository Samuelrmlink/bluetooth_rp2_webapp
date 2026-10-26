// The editor sidebar's file menu on a touch screen: a long press on a row,
// or the row's ⋯ button (coarse pointers only). Real touch input through
// CDP, so the browser makes the pointer events -- and, on a long press, its
// own contextmenu -- the way a phone's does.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');
const more = (page, p) => row(page, p).locator('.ft-more');

async function editor(page) {
  // The menu opens under the finger: a lift that clicked one of its items
  // would show up here (Delete… and Rename… ask first).
  page.__dialogs = 0;
  page.on('dialog', (d) => { page.__dialogs++; d.dismiss(); });
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/ac_pulse.lua'));
  await expect(row(page, 'lua/ac_pulse.lua')).toBeVisible();
  // Let the layout settle: the mock's devices register their panes about a
  // second after load, and any layout change closes the popover (app.js) --
  // a menu held open across that would vanish under the test.
  await page.waitForFunction(() => {
    const v = window.__btgwApp._layout.version;
    const s = window.__settle || (window.__settle = { v, t: performance.now() });
    if (s.v !== v) { s.v = v; s.t = performance.now(); }
    return performance.now() - s.t > 1500;
  }, null, { polling: 100 });
  // Count the menus opened: a long press and the browser's contextmenu
  // must make one between them, not two.
  await page.evaluate(() => {
    const w = window.__btgwApp.windows.get('editor').win;
    const real = w.openFileMenu.bind(w);
    window.__menus = 0;
    w.openFileMenu = (p) => { window.__menus++; return real(p); };
  });
}

async function centre(page, p) {
  // Measured in one go: the tree re-renders its rows freely, and a row
  // replaced between a lookup and a measurement has no box.
  await expect(row(page, p)).toBeVisible();
  return page.evaluate((path) => {
    const b = document.querySelector(`.ft-row[data-path="${path}"]`).getBoundingClientRect();
    return { x: Math.round(b.x + b.width / 3), y: Math.round(b.y + b.height / 2) };
  }, p);
}

// One finger: down at `at`, through `path`, held `ms`, then up.
async function touch(page, at, { path = [], ms = 0 } = {}) {
  const cdp = await page.context().newCDPSession(page);
  const send = (type, pts) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: pts });
  await send('touchStart', [at]);
  let last = at;
  for (const pt of path) { await send('touchMove', [pt]); last = pt; }
  if (ms) await page.waitForTimeout(ms);
  await send('touchEnd', []);
  await cdp.detach();
  return last;
}

test.describe('touch', () => {
  test.use({ hasTouch: true, isMobile: true });

  test('a long press opens the row\'s menu once, and the lift does not open the file', async ({ page }) => {
    await editor(page);
    expect(await page.evaluate(() => matchMedia('(pointer: coarse)').matches)).toBe(true);
    await touch(page, await centre(page, 'lua/pinecil.lua'), { ms: 900 });
    await expect(menu(page)).toBeVisible();
    await expect(menu(page).locator('.sm-title')).toHaveText('lua/pinecil.lua');
    await expect(row(page, 'lua/pinecil.lua')).toHaveClass(/is-menu-row/);
    await page.waitForTimeout(300);                       // a late click would land by now
    await expect(page.locator('.ed-name')).toHaveText(/ac_pulse\.lua/);
    await expect(menu(page)).toBeVisible();
    expect(await page.evaluate(() => window.__menus)).toBe(1);
    expect(page.__dialogs).toBe(0);
    // a menu item tapped next works: the lift's click was the only one eaten
    const [dl] = await Promise.all([
      page.waitForEvent('download'),
      menu(page).getByRole('button', { name: 'Download' }).tap(),
    ]);
    expect(dl.suggestedFilename()).toBe('pinecil.lua');
    await expect(menu(page)).toBeHidden();
    // the next plain tap is not eaten
    await page.touchscreen.tap(...Object.values(await centre(page, 'lua/pinecil.lua')));
    await expect(page.locator('.ed-name')).toHaveText(/pinecil\.lua/);
  });

  test('the browser\'s own long-press contextmenu, before or after the timer, makes no second menu', async ({ page }) => {
    await editor(page);
    // Android Chrome sends a contextmenu (a PointerEvent, pointerType touch)
    // during the hold; headless Chromium does not, so it is sent by hand.
    const ctx = (p) => page.evaluate((path) => {
      document.querySelector(`.ft-row[data-path="${path}"] .ft-name`)
        .dispatchEvent(new PointerEvent('contextmenu', { bubbles: true, cancelable: true, pointerType: 'touch' }));
    }, p);
    const cdp = await page.context().newCDPSession(page);
    const at = await centre(page, 'lua/pinecil.lua');
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [at] });
    await page.waitForTimeout(250);
    await ctx('lua/pinecil.lua');                         // early: it opens the menu now
    await expect(menu(page)).toBeVisible();
    await page.waitForTimeout(500);                       // the timer would have fired by now
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    expect(await page.evaluate(() => window.__menus)).toBe(1);
    await page.keyboard.press('Escape');
    // late: after the timer has opened it
    const at2 = await centre(page, 'lua/note.lua');
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [at2] });
    await expect(menu(page).locator('.sm-title')).toHaveText('lua/note.lua');
    await ctx('lua/note.lua');
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await cdp.detach();
    await page.waitForTimeout(200);
    await expect(menu(page)).toBeVisible();
    expect(await page.evaluate(() => window.__menus)).toBe(2);
    expect(page.__dialogs).toBe(0);
    await expect(page.locator('.ed-name')).toHaveText(/ac_pulse\.lua/);
  });

  test('a long press on a folder opens its menu and does not toggle it', async ({ page }) => {
    await editor(page);
    await touch(page, await centre(page, 'lua/profiles'), { ms: 900 });
    await expect(menu(page).getByRole('button', { name: 'New file here…' })).toBeVisible();
    await page.waitForTimeout(300);
    await expect(row(page, 'lua/profiles/pinecil')).toHaveCount(0);   // still closed
  });

  test('a drag (a scroll) opens nothing', async ({ page }) => {
    await editor(page);
    const at = await centre(page, 'lua/pinecil.lua');
    const path = [1, 2, 3, 4, 5].map((i) => ({ x: at.x, y: at.y - i * 8 }));
    await touch(page, at, { path, ms: 900 });
    await page.waitForTimeout(200);
    await expect(menu(page)).toBeHidden();
    expect(await page.evaluate(() => window.__menus)).toBe(0);
    await expect(page.locator('.ed-name')).toHaveText(/ac_pulse\.lua/);
  });

  test('a short tap still opens the file', async ({ page }) => {
    await editor(page);
    const at = await centre(page, 'lua/pinecil.lua');
    await page.touchscreen.tap(at.x, at.y);
    await expect(page.locator('.ed-name')).toHaveText(/pinecil\.lua/);
    await page.waitForTimeout(700);                       // past the long press
    await expect(menu(page)).toBeHidden();
  });

  test('the ⋯ button opens the menu and not the file; it has a label', async ({ page }) => {
    await editor(page);
    const b = more(page, 'lua/pinecil.lua');
    await expect(b).toBeVisible();
    await expect(b).toHaveAttribute('aria-label', 'Menu for pinecil.lua');
    const box = await b.boundingBox();
    expect(box.height).toBeGreaterThanOrEqual(32);
    await b.tap();
    await expect(menu(page)).toBeVisible();
    await expect(menu(page).locator('.sm-title')).toHaveText('lua/pinecil.lua');
    await expect(page.locator('.ed-name')).toHaveText(/ac_pulse\.lua/);
    // on a folder: the menu, and the folder stays as it was
    await page.keyboard.press('Escape');
    await more(page, 'lua/profiles').tap();
    await expect(menu(page).getByRole('button', { name: 'Upload here…' })).toBeVisible();
    await expect(row(page, 'lua/profiles/pinecil')).toHaveCount(0);
  });

  test('rows and menu items are finger-sized', async ({ page }) => {
    await editor(page);
    expect((await row(page, 'lua/pinecil.lua').boundingBox()).height).toBeGreaterThanOrEqual(40);
    await more(page, 'lua/pinecil.lua').tap();
    const item = menu(page).getByRole('button', { name: 'Download' });
    expect((await item.boundingBox()).height).toBeGreaterThanOrEqual(40);
    await page.keyboard.press('Escape');
  });
});

test.describe('desktop', () => {
  test('no ⋯ button on a fine pointer, and the rows keep their size', async ({ page }) => {
    await editor(page);
    expect(await page.evaluate(() => matchMedia('(pointer: coarse)').matches)).toBe(false);
    await expect(more(page, 'lua/pinecil.lua')).toBeHidden();
    expect((await row(page, 'lua/pinecil.lua').boundingBox()).height).toBeLessThan(30);
    // right-click still opens exactly one menu
    await row(page, 'lua/pinecil.lua').click({ button: 'right' });
    await expect(menu(page)).toBeVisible();
    expect(await page.evaluate(() => window.__menus)).toBe(1);
  });

  test('a mouse held down on a row is not a long press', async ({ page }) => {
    await editor(page);
    const at = await centre(page, 'lua/pinecil.lua');
    await page.mouse.move(at.x, at.y);
    await page.mouse.down();
    await page.waitForTimeout(800);
    await expect(menu(page)).toBeHidden();
    await page.mouse.up();
    // (no assertion on the click: the tree re-renders its rows every few
    // seconds, and a press that spans one loses its click in any browser)
    await page.waitForTimeout(200);
    await expect(menu(page)).toBeHidden();
    expect(await page.evaluate(() => window.__menus)).toBe(0);
  });
});

// The window manager taken from the km003c app, in this app: every UI is a
// window, and the keyboard reaches all of it.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

test('every built-in window is offered, grouped, and the first visit is arranged', async ({ page }) => {
  await ready(page);
  await expect(page.locator('#tiles .tile')).toHaveCount(3);
  const groups = await page.$$eval('#tiles .tile .pane-pick >> nth=0', (s) =>
    [...s[0].querySelectorAll('optgroup')].map((g) => [g.label, [...g.children].map((o) => o.textContent)]));
  expect(groups[0]).toEqual(['Tools', ['Editor', 'Scanner', 'GATT', 'GATT Events', 'Console', 'Hardware']]);
});

test('Alt+hjkl moves between windows; ? lists every binding; Alt+S opens Settings', async ({ page }) => {
  await ready(page);
  const active = () => page.evaluate(() => document.querySelector('#tiles .tile[data-active]')?.dataset.pane);
  expect(await active()).toBe('editor');
  await page.keyboard.press('Alt+KeyL');
  expect(await active()).toBe('scanner');
  await page.keyboard.press('Alt+KeyJ');
  expect(await active()).toBe('console');
  await page.keyboard.press('Alt+KeyH');
  expect(await active()).toBe('editor');

  // help is for where you are: with the editor active, ? is the editor's
  // own list; from another window, the main one -- and between them every
  // binding is listed (the main list has one row pointing at the editor's)
  await page.keyboard.press('Escape');
  await page.keyboard.press('?');
  await expect(page.locator('#editor-help-dlg')).toBeVisible();
  const counts = await page.evaluate(async () => {
    const all = (await import('/js/keys.js')).BINDINGS.filter((b) => b.group && !b.hidden);
    return { editor: all.filter((b) => b.group === 'Editor').length, other: all.filter((b) => b.group !== 'Editor').length };
  });
  expect(await page.locator('#editor-help-dlg .keys tr').count()).toBe(counts.editor);
  await page.keyboard.press('Escape');
  await expect(page.locator('#editor-help-dlg')).toBeHidden();
  await page.keyboard.press('Alt+KeyL');                           // the Scanner
  await page.keyboard.press('Shift+Slash');                         // a real ?, Shift held (the Scanner's / is unshifted)
  await expect(page.locator('#help-modal')).toBeVisible();
  expect(await page.locator('#help-modal .keys tr').count()).toBe(counts.other + 1);
  await page.keyboard.press('Escape');
  await page.keyboard.press('Alt+KeyH');

  await page.keyboard.press('Alt+KeyS');
  await expect(page.locator('#settings-modal')).toBeVisible();
  for (const id of ['sect-ui', 'sect-wifi', 'sect-wireguard', 'sect-bluetooth', 'sect-firmware']) {
    await expect(page.locator(`#${id}`)).toHaveCount(1);
  }
});

test('tabbed view survives a reload', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+Shift+KeyT');
  await expect(page.locator('#tabbar')).toBeVisible();
  await page.reload();
  await expect(page.locator('#tabbar')).toBeVisible();
});

test('tabbed view: Alt+N visits every tab in order, one at a time; Alt+Shift+N goes back', async ({ page }) => {
  await ready(page);
  await page.keyboard.press('Alt+Shift+KeyT');
  await expect(page.locator('#tabbar')).toBeVisible();
  const tabs = await page.$$eval('#tabbar .tab', (b) => b.map((x) => x.dataset.pane));
  expect(tabs.length).toBeGreaterThanOrEqual(5);
  const active = () => page.$eval('#tabbar .tab.active', (b) => b.dataset.pane);
  // From wherever it starts, n presses visit n consecutive tabs and wrap.
  const start = tabs.indexOf(await active());
  for (let k = 1; k <= tabs.length; k++) {
    await page.keyboard.press('Alt+KeyN');
    await expect.poll(active).toBe(tabs[(start + k) % tabs.length]);
  }
  for (let k = 1; k <= 2; k++) {
    await page.keyboard.press('Alt+Shift+KeyN');
    await expect.poll(active).toBe(tabs[(start - k + tabs.length) % tabs.length]);
  }
});

test('Console: Copy all copies the lines shown', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await ready(page);
  await page.evaluate(() => window.__btgwApp.show('console'));
  await page.click('.c-copy');
  await expect(page.locator('.toast')).toContainText(/Copied \d+ lines?/);
  const text = await page.evaluate(() => navigator.clipboard.readText());
  expect(text).toMatch(/^\d\d:\d\d:\d\d app connected to the gateway/m);
});

test('Console on a phone: the keyboard shrinks the app instead of hiding it; Send and Enter both run', async ({ page }) => {
  await page.setViewportSize({ width: 412, height: 880 });            // a Fold6-ish portrait screen
  await ready(page);
  await page.evaluate(() => window.__btgwApp.show('console'));
  for (let i = 0; i < 40; i++) await page.evaluate((n) => window.__btgwApp.windows.get('console').win.add('info', 'test', `line ${n}`), i);
  await expect(page.locator('.console-line').last()).toContainText('line 39');
  const box = page.locator('.c-chunk');
  await box.focus();
  const mode = () => page.evaluate(() => window.__btgwApp._layout.mode);
  const before = await mode();
  // what an on-screen keyboard does: the visible height drops, focus stays in the box
  await page.setViewportSize({ width: 412, height: 470 });
  await page.waitForTimeout(300);
  expect(await mode()).toBe(before);                                    // the arrangement does not flip
  expect(await page.evaluate(() => window.scrollY)).toBe(0);           // the page did not scroll the app away
  const inView = async (sel) => {
    const b = await page.locator(sel).last().boundingBox();
    const ok = b && b.y >= 0 && b.y + b.height <= 470 + 1;
    return ok;
  };
  await expect.poll(() => inView('.c-chunk')).toBe(true);
  await expect.poll(() => inView('.c-send')).toBe(true);
  await expect.poll(() => inView('.console-line')).toBe(true);         // the newest line is still on screen
  await expect(page.locator('.console-line').last()).toContainText('line 39');
  // Send runs it and keeps the box (and so the keyboard)
  await box.fill('1+1');
  await page.locator('.c-send').click();
  await expect(page.locator('.console-line').last()).toContainText(/2|=/);
  await expect(box).toBeFocused();
  // Enter still runs it
  await box.fill('2+2');
  await box.press('Enter');
  await expect(page.locator('.console-line', { hasText: /> 2\+2$/ })).toHaveCount(1);   // the input line: Enter ran it
});

test('Console: a script polling in short waits is not news; a long wait giving up, or a lost link, is', async ({ page }) => {
  await ready(page);
  const emit = (data) => page.evaluate((d) => window.__btgwApp.ws.call('test.emit', { evt: 'script.wake', data: d }), data);
  for (let i = 0; i < 5; i++) await emit({ script: 'anker2', reason: 'timeout', waited_ms: 250, link: 0 });
  await emit({ script: 'disc', reason: 'timeout', waited_ms: 5000, link: 1 });
  await emit({ script: 'anker2', reason: 'link_lost', waited_ms: 120, link: 1 });
  const lines = page.locator('.console-line', { hasText: 'woke:' });
  await expect(lines).toHaveCount(2);
  await expect(lines.nth(0)).toContainText('disc woke: timeout after 5000 ms');
  await expect(lines.nth(1)).toContainText('anker2 woke: link_lost after 120 ms');
});

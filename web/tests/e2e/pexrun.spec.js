// Run and Stop on a .pex file start and stop that PEX app. The same buttons
// on a Lua script still run the script, and neither path writes the other.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');

test('Run and Stop on a .pex file start and stop the app; Lua Start is unchanged', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.setPref('vim', false));

  // The Lua file's own Start still starts the script, and does not load a PEX app.
  await page.evaluate(() => window.__btgwApp.openFile('lua/pinecil.lua'));
  await expect(row(page, 'lua/pinecil.lua')).toBeVisible();
  await row(page, 'lua/pinecil.lua').click({ button: 'right' });
  await menu(page).getByRole('button', { name: 'Start pinecil' }).click();
  await expect(page.locator('.ed-notice')).toContainText('Started pinecil');
  const afterLua = await page.evaluate(async () => {
    const app = window.__btgwApp;
    const scripts = await app.ws.call('script.list');
    const pex = await app.ws.call('pex.list');
    return {
      luaRunning: scripts.scripts.find((s) => s.name === 'pinecil')?.running ?? false,
      modules: pex.modules,
    };
  });
  expect(afterLua.luaRunning).toBe(true);
  expect(afterLua.modules).toEqual([]);

  // Open the PEX file. Run / Stop are the same toolbar buttons.
  await row(page, 'pex').click();
  await row(page, 'pex/anker.m33.pex').click();
  await expect(page.locator('.ed-b-run')).toHaveAttribute('title', /Start the anker PEX app/);
  await page.click('.ed-b-run');
  await expect(page.locator('.ed-notice')).toContainText('Started anker');
  await expect(page.locator('.ed-state')).toContainText('running');
  await expect(row(page, 'pex/anker.m33.pex').locator('.ft-arena')).toHaveText('L');

  // A second Run does not load another copy.
  await page.click('.ed-b-run');
  await expect(page.locator('.ed-notice')).toContainText('already running');
  const mods = await page.evaluate(async () => (await window.__btgwApp.ws.call('pex.list')).modules);
  expect(mods).toEqual([{ name: 'anker', state: 'running' }]);

  await page.click('.ed-b-stop');
  await expect(page.locator('.ed-notice')).toContainText('Stopped and unloaded anker');
  await expect(row(page, 'pex/anker.m33.pex').locator('.ft-arena')).toHaveCount(0);

  // The Lua source was not rewritten by starting the app.
  const lua = await page.evaluate(async () => {
    const r = await fetch('/api/fs/lua/anker.lua');
    return r.text();
  });
  expect(lua.length).toBeGreaterThan(20);
});

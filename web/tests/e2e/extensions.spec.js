// Windows from other projects: a folder uploaded to js/ext/ on the gateway
// (here, the mock's filesystem) is listed, imported and registered at
// startup (js/extensions.js) -- no change to the app.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const GOOD = `
import { buildControl } from '../../controls/kit.js';
export function register(app) {
  return app.addWindow('ext-hello', { title: 'Hello Ext', group: 'Devices' }, (el) => {
    el.innerHTML = '<div class="ext-hello">hello from js/ext</div>';
    el.appendChild(buildControl({ kind: 'readout', field: 'x', label: 'kit works' }, () => {}).el);
    return {};
  });
}`;

async function put(page, path, body) {
  const r = await page.evaluate(async ([p, b]) => (await fetch('/api/fs/' + p, { method: 'PUT', body: b })).status, [path, body]);
  expect(r).toBe(200);
}

test('an uploaded js/ext module adds its window; a broken one is reported and does not stop the app', async ({ page }) => {
  await ready(page);
  expect(await page.evaluate(() => window.__btgwApp.extensions)).toEqual([]);       // none yet
  await put(page, 'js/ext/hello/index.js', GOOD);
  await put(page, 'js/ext/broken/index.js', 'export const nothing = 1;');            // no register()
  await put(page, 'js/ext/empty/readme.txt', 'no index.js here');
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  expect(await page.evaluate(() => window.__btgwApp.extensions)).toEqual(['hello']);
  expect(await page.evaluate(() => window.__btgwApp.windows.has('ext-hello'))).toBe(true);
  await page.evaluate(() => window.__btgwApp.show('ext-hello'));
  await expect(page.locator('.ext-hello')).toHaveText('hello from js/ext');
  await expect(page.locator('.ctl-label', { hasText: 'kit works' })).toBeVisible();   // its ../../ imports resolve
  // the broken one and the folder without index.js: said in the Console, nothing else
  await page.evaluate(() => window.__btgwApp.show('console'));
  await expect(page.locator('#pane-console')).toContainText('extension broken: index.js exports no register(app)');
  await expect(page.locator('#pane-console')).toContainText('extension empty:');
  // a saved arrangement keeps its place across a reload
  await page.reload();
  await page.waitForFunction(() => window.__btgwApp.windows.has('ext-hello'));
  await page.evaluate(() => window.__btgwApp.show('ext-hello'));
  await expect(page.locator('.ext-hello')).toBeVisible();
  await page.evaluate(() => window.__btgwApp.ws.call('test.reset'));
});

// A sub-app registers a toolbar button without a file in this app's tree.
// The card folder's app.json is the manifest. A js/ext module can also call
// addLauncher. Neither path has to add a window.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

async function put(page, path, body) {
  const r = await page.evaluate(async ([p, b]) => (await fetch('/api/fs/' + p, { method: 'PUT', body: b })).status, [path, body]);
  expect(r).toBe(200);
}

async function putSd(page, path, body) {
  const r = await page.evaluate(async ([p, b]) => (await fetch('/api/sd/files/' + p, { method: 'PUT', body: b })).status, [path, body]);
  expect(r).toBe(200);
}

const EXT = `
export function register(app) {
  app.addLauncher({ id: 'ext-tool', label: 'Ext tool', onClick() { window.__extTool = (window.__extTool || 0) + 1; } });
}
`;

test('a card manifest and an extension each add a toolbar button', async ({ page }) => {
  await ready(page);
  await putSd(page, 'apps/printer/app.json', '{"label":"Printer"}\n');
  await putSd(page, 'apps/notes/readme.txt', 'just notes\n');
  await putSd(page, 'apps/broken/app.json', '{');
  await put(page, 'js/ext/tool/index.js', EXT);
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  const printer = page.locator('#launchers a[data-launcher="printer"]');
  await expect(printer).toHaveText('Printer');
  await expect(printer).toHaveAttribute('href', '/sd/apps/printer/');
  await expect(page.locator('#launchers a[href="/sd/apps/notes/"]')).toHaveCount(0);
  const tool = page.locator('#launchers button[data-launcher="ext-tool"]');
  await expect(tool).toHaveText('Ext tool');
  await tool.click();
  expect(await page.evaluate(() => window.__extTool)).toBe(1);
  await page.evaluate(() => window.__btgwApp.show('console'));
  await expect(page.locator('#pane-console')).toContainText('launcher broken: bad app.json');
  await page.evaluate(() => window.__btgwApp.ws.call('test.reset'));
});

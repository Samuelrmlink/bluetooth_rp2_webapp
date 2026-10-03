// Drag and drop in the editor's file list (upload, extract a .tar, move,
// drag out), and deleting a folder with everything in it.
import { test, expect } from '@playwright/test';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { ready } from './helpers.js';

const row = (page, p) => page.locator(`.ft-row[data-path="${p}"]`);
const menu = (page) => page.locator('#pane-popover .ed-file-menu');
const notice = (page) => page.locator('.ed-notice');
const status = (page, p) => page.evaluate(async (path) => (await fetch(`/api/fs/${path}`)).status, p);
const text = (page, p) => page.evaluate(async (path) => (await fetch(`/api/fs/${path}`)).text(), p);

async function editor(page) {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(row(page, 'lua/anker.lua')).toBeVisible();
  await page.waitForFunction(() => {
    const v = window.__btgwApp._layout.version;
    const s = window.__settle || (window.__settle = { v, t: performance.now() });
    if (s.v !== v) { s.v = v; s.t = performance.now(); }
    return performance.now() - s.t > 1500;
  }, null, { polling: 100 });
}

// Files from "the computer", dropped on an element: [{ name, text } | { name, bytes }].
function drop(page, selector, files) {
  return page.evaluate(({ selector, files }) => {
    const dt = new DataTransfer();
    for (const f of files) dt.items.add(new File([f.bytes ? new Uint8Array(f.bytes) : f.text], f.name));
    const el = document.querySelector(selector);
    for (const type of ['dragenter', 'dragover', 'drop']) {
      el.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt }));
    }
  }, { selector, files });
}

test('files dropped on a folder row upload into it; on a file row, beside it', async ({ page }) => {
  await editor(page);
  await drop(page, '.ft-row[data-path="lua/profiles"]', [{ name: 'dropped.lua', text: 'return "dropped"\n' }]);
  await expect(notice(page)).toContainText('Uploaded 1 of 1 file to lua/profiles/');
  expect(await text(page, 'lua/profiles/dropped.lua')).toBe('return "dropped"\n');
  await drop(page, '.ft-row[data-path="lua/anker.lua"]', [{ name: 'beside.txt', text: 'b' }, { name: 'two.txt', text: '2' }]);
  await expect(notice(page)).toContainText('Uploaded 2 of 2 files to lua/');
  expect(await text(page, 'lua/beside.txt')).toBe('b');
  await expect(row(page, 'lua/beside.txt')).toBeVisible();
});

test('a file already there: Cancel keeps it and uploads the rest; OK replaces it', async ({ page }) => {
  await editor(page);
  const orig = await text(page, 'lua/pinecil.lua');
  page.once('dialog', (d) => d.dismiss());
  await drop(page, '.ft-row[data-path="lua"]', [{ name: 'pinecil.lua', text: '-- replaced' }, { name: 'fresh.lua', text: '-- new' }]);
  await expect(notice(page)).toContainText('Uploaded 1 of 1');
  expect(await text(page, 'lua/pinecil.lua')).toBe(orig);
  expect(await text(page, 'lua/fresh.lua')).toBe('-- new');
  page.once('dialog', (d) => d.accept());
  await drop(page, '.ft-row[data-path="lua"]', [{ name: 'pinecil.lua', text: '-- replaced' }]);
  await expect.poll(() => text(page, 'lua/pinecil.lua')).toBe('-- replaced');
});

test('a .tar dropped on a folder is extracted there, after the preview', async ({ page }) => {
  await editor(page);
  const root = mkdtempSync(join(tmpdir(), 'btgw-dnd-'));
  mkdirSync(join(root, 'kit'));
  writeFileSync(join(root, 'kit', 'one.lua'), 'return 1\n');
  const tar = execFileSync('tar', ['-cf', '-', '-C', root, 'kit']);
  rmSync(root, { recursive: true, force: true });
  await drop(page, '.ft-row[data-path="www"]', [{ name: 'kit.tar', bytes: [...tar] }]);
  const dlg = page.locator('#restore-dlg');
  await expect(dlg.locator('.rs-summary')).toContainText('into www/');
  await dlg.getByRole('button', { name: 'Restore 1 file' }).click();
  await expect(dlg.locator('.rs-status')).toHaveText('Restored 1 file.');
  expect(await text(page, 'www/kit/one.lua')).toBe('return 1\n');
  expect(await status(page, 'www/kit.tar')).toBe(404);           // extracted, not stored
});

test('a row dragged onto a folder moves there; its open tab follows; a folder moves whole', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/pinecil.lua'));
  await row(page, 'lua/pinecil.lua').dragTo(row(page, 'www'));
  await expect(notice(page)).toContainText('Moved lua/pinecil.lua to www/pinecil.lua');
  expect(await status(page, 'lua/pinecil.lua')).toBe(404);
  await expect(page.locator('.ed-ftab[data-path="www/pinecil.lua"]')).toBeVisible();
  await expect(page.locator('.ed-name')).toHaveText('www/pinecil.lua');
  // a folder, onto another
  await row(page, 'www').dragTo(row(page, 'cfg'));
  await expect(notice(page)).toContainText('Moved www to cfg/www');
  expect(await status(page, 'cfg/www/pinecil.lua')).toBe(200);
  await expect(page.locator('.ed-ftab[data-path="cfg/www/pinecil.lua"]')).toBeVisible();
  // never into itself
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.moveTo('lua', 'lua/profiles'));
  expect(await status(page, 'lua/anker.lua')).toBe(200);
});

test('dragging a row out carries a download: the file, or a folder as .tar', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.windows.get('editor').win.tree.reveal('www/demo.js.gz'));
  await expect(row(page, 'www/demo.js.gz')).toBeVisible();
  const data = await page.evaluate(() => {
    const out = {};
    for (const p of ['lua/anker.lua', 'www/demo.js.gz', 'lua/profiles']) {
      const dt = new DataTransfer();
      document.querySelector(`.ft-row[data-path="${p}"]`).dispatchEvent(new DragEvent('dragstart', { bubbles: true, cancelable: true, dataTransfer: dt }));
      out[p] = dt.getData('DownloadURL');
      document.querySelector('.ed-files').dispatchEvent(new DragEvent('dragend', { bubbles: true }));
    }
    return out;
  });
  expect(data['lua/anker.lua']).toMatch(/^application\/octet-stream:anker\.lua:http.*\/api\/fs\/lua\/anker\.lua$/);
  expect(data['www/demo.js.gz']).toMatch(/:demo\.js:/);                       // unpacked on the way
  expect(data['lua/profiles']).toMatch(/^application\/x-tar:profiles\.tar:http.*\/api\/tar\/lua\/profiles\?mtime=\d+$/);
  await expect(row(page, 'lua/anker.lua')).toHaveAttribute('draggable', 'true');
});

const confirmDlg = (page) => page.locator('#confirm-delete-dlg');

test('Delete folder and everything in it: always confirmed in a dialog that lists what goes', async ({ page }) => {
  await editor(page);
  await page.evaluate(() => window.__btgwApp.openFile('www/demo.js.gz'));
  const ask = async () => {
    await row(page, 'www').click({ button: 'right' });
    await menu(page).getByRole('button', { name: 'Delete folder and everything in it…' }).click();
    await expect(confirmDlg(page)).toBeVisible();
  };
  await ask();
  await expect(confirmDlg(page).locator('.cd-summary')).toContainText('Delete www/ and everything in it: 3 files');
  await expect(confirmDlg(page).locator('.cd-summary')).toContainText('This cannot be undone.');
  await expect(confirmDlg(page).locator('.cd-list')).toContainText('www/dot.png');
  await expect(confirmDlg(page).locator('.cd-list')).toContainText('www/logo.svg.gz');
  // Cancel is the default: Enter on it, Esc, and the Cancel button all keep everything
  await expect(confirmDlg(page).locator('.cd-cancel')).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(confirmDlg(page)).toBeHidden();
  await expect(notice(page)).toHaveText('Not deleted: www/');
  await ask();
  await page.keyboard.press('Escape');
  await expect(confirmDlg(page)).toBeHidden();
  await ask();
  await confirmDlg(page).locator('.cd-cancel').click();
  expect(await status(page, 'www/dot.png')).toBe(200);
  // and only the Delete button deletes
  await ask();
  await confirmDlg(page).getByRole('button', { name: 'Delete 3 files' }).click();
  await expect(notice(page)).toHaveText('Deleted www/ and everything in it');
  await expect(row(page, 'www')).toHaveCount(0);
  expect(await status(page, 'www/dot.png')).toBe(404);
  await expect(page.locator('.ed-ftab[data-path="www/demo.js.gz"]')).toHaveCount(0);
});

test('one of the gateway\'s own folders: Delete stays disabled until its name is typed', async ({ page }) => {
  await editor(page);
  await row(page, 'cfg').click({ button: 'right' });
  await menu(page).getByRole('button', { name: 'Delete folder and everything in it…' }).click();
  const del = confirmDlg(page).locator('.cd-delete');
  await expect(confirmDlg(page).locator('.cd-name')).toBeFocused();
  await expect(del).toBeDisabled();
  await page.keyboard.type('nope');
  await page.keyboard.press('Enter');                               // the wrong name: nothing happens
  await expect(del).toBeDisabled();
  expect(await status(page, 'cfg/wifi.json')).toBe(200);
  await confirmDlg(page).locator('.cd-name').fill('cfg');
  await expect(del).toBeEnabled();
  await del.click();
  await expect(notice(page)).toHaveText('Deleted cfg/ and everything in it');
  expect(await status(page, 'cfg/wifi.json')).toBe(404);
});

test.describe('touch', () => {
  test.use({ hasTouch: true, isMobile: true });
  test('rows are not draggable on a touch screen (a long press is the menu)', async ({ page }) => {
    await editor(page);
    await expect(row(page, 'lua/anker.lua')).not.toHaveAttribute('draggable', 'true');
  });
});

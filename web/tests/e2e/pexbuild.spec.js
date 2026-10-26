// Project builds follow the Pexfile of the open file. The destination dialog
// is not used. Two pico2_w targets: default is flash, ram compiles a different
// source. A second m33 board is listed and is not the one the button runs
// while sys.info has no board name.
import { test, expect } from '@playwright/test';
import { ready } from './helpers.js';

const FLASH = `#include "pex.h"
#ifndef PEX_TARGET_m33
#error need m33
#endif
#ifndef PEX_BOARD_PICO2_W
#error need board
#endif
#ifndef PEX_E2E_HEADER
#error need header
#endif
#ifdef BLINK_RAM
#error flash target saw BLINK_RAM
#endif
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    if (reason == PEX_INIT) pex_log(PEX_LOG_INFO, "flash-target");
    if (reason == PEX_APP_RUN) return PEX_OK;
    return PEX_OK;
}
`;

const RAM = `#include "pex.h"
#ifndef BLINK_RAM
#error ram target needs BLINK_RAM
#endif
#ifndef PEX_E2E_HEADER
#error need header
#endif
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    if (reason == PEX_INIT) pex_log(PEX_LOG_INFO, "ram-target");
    if (reason == PEX_APP_RUN) return PEX_OK;
    return PEX_OK;
}
`;

const PEXFILE = `name blink
version 0.1.0
kind app
model reloc
level info
inc src
src src/main.c
default pico2_w

target pico2_w
  board pico2_w
  out build/pico2_w
  run flash

target ram
  board pico2_w
  out build/ram
  run ram
  src src/ram_main.c
  define BLINK_RAM=1

target other
  board other_m33
  out build/other
  run flash
  src src/other.c
`;

const BOARD = `name pico2_w
arch m33
header pico2_w.h
define PEX_HAS_WIFI=1
`;

async function putSd(page, path, body) {
  const status = await page.evaluate(async ({ path, body }) => {
    const url = '/api/sd/files/' + path.split('/').map(encodeURIComponent).join('/');
    const r = await fetch(url, { method: 'PUT', body });
    return r.status;
  }, { path, body });
  expect(status).toBe(200);
}

async function seed(page) {
  await putSd(page, 'boards/pico2_w.board', BOARD);
  await putSd(page, 'boards/pico2_w.h', '#define PEX_E2E_HEADER 1\n');
  await putSd(page, 'boards/other_m33.board', 'name other_m33\narch m33\n');
  await putSd(page, 'projects/blink/Pexfile', PEXFILE);
  await putSd(page, 'projects/blink/src/main.c', FLASH);
  await putSd(page, 'projects/blink/src/ram_main.c', RAM);
  await putSd(page, 'projects/blink/src/other.c', FLASH);
}

async function openSource(page) {
  await page.evaluate(() => window.__btgwApp.openFile('sd/projects/blink/src/main.c'));
  await expect(page.locator('.ed-b-build')).toHaveAttribute('aria-label', 'Build blink · pico2_w', { timeout: 15000 });
}

test('a file with no Pexfile cannot build, and a second board is not selected', async ({ page }) => {
  await ready(page);
  await page.evaluate(() => window.__btgwApp.openFile('lua/anker.lua'));
  await expect(page.locator('.ed-b-build')).toHaveAttribute('title', /not in a project/, { timeout: 15000 });
  await expect(page.locator('.ed-b-build')).toBeDisabled();
  await expect(page.locator('.ed-b-brun')).toBeDisabled();
  await expect(page.locator('#build-dlg')).toHaveCount(0);

  await seed(page);
  await openSource(page);
  await expect(page.locator('.ed-b-build')).toBeEnabled();
  await expect(page.locator('.ed-b-build')).toHaveAttribute('title', /board name is not reported/);
  await expect(page.locator('.ed-b-brun')).toHaveAttribute('aria-label', /Build and run blink · pico2_w/);

  const labels = await page.evaluate(async () => {
    const ed = window.__btgwApp.windows.get('editor').win;
    await ed.tree.reveal('sd/projects/blink/Pexfile');
    ed.tree.render();
    await ed.openFileMenu('sd/projects/blink/Pexfile');
    return [...document.querySelectorAll('.ed-file-menu button')].map((b) => b.textContent);
  });
  assertOrder(labels);
});

function assertOrder(labels) {
  const build = labels.filter((t) => t.startsWith('Build'));
  expect(build.slice(0, 5)).toEqual([
    'Build pico2_w',
    'Build and run pico2_w',
    'Build ram',
    'Build and run ram',
    'Build other',
  ]);
  expect(build).toContain('Build all');
  expect(build.indexOf('Build other')).toBeGreaterThan(build.indexOf('Build ram'));
}

test('Build writes the default target, and Build ram compiles the other source', async ({ page }) => {
  test.setTimeout(240000);
  await ready(page);
  await seed(page);
  await openSource(page);
  await page.click('.ed-b-build');
  const log = page.locator('.ed-log-body');
  await expect(log).toContainText('sd/projects/blink/build/pico2_w/blink.m33.pex', { timeout: 180000 });
  await expect(log).toContainText('source src/main.c');
  await expect(log).toContainText('#define PEX_TARGET_m33 1');
  await expect(log).toContainText('python3 pex/tools/pexlink.py sign');
  await expect(page.locator('#build-dlg')).toHaveCount(0);
  const flash = await page.evaluate(async () => {
    const r = await fetch('/api/sd/files/projects/blink/build/pico2_w/blink.m33.pex');
    return Array.from(new Uint8Array(await r.arrayBuffer()));
  });
  const flashBuf = Buffer.from(flash);
  expect(flashBuf.subarray(0, 4).toString('latin1')).toBe('PEX\x1a');
  expect(flashBuf.includes(Buffer.from('flash-target'))).toBe(true);
  expect(flashBuf.includes(Buffer.from('ram-target'))).toBe(false);

  await page.evaluate(async () => {
    const ed = window.__btgwApp.windows.get('editor').win;
    await ed.tree.reveal('sd/projects/blink/Pexfile');
    ed.tree.render();
    await ed.openFileMenu('sd/projects/blink/Pexfile');
  });
  await page.locator('.ed-file-menu button', { hasText: 'Build ram' }).click();
  await expect(log).toContainText('sd/projects/blink/build/ram/blink.m33.pex', { timeout: 180000 });
  await expect(log).toContainText('source src/ram_main.c');
  await expect(log).toContainText('#define BLINK_RAM 1');
  const ram = await page.evaluate(async () => {
    const r = await fetch('/api/sd/files/projects/blink/build/ram/blink.m33.pex');
    return Array.from(new Uint8Array(await r.arrayBuffer()));
  });
  const ramBuf = Buffer.from(ram);
  expect(ramBuf.subarray(0, 4).toString('latin1')).toBe('PEX\x1a');
  expect(ramBuf.includes(Buffer.from('ram-target'))).toBe(true);
  expect(ramBuf.includes(Buffer.from('flash-target'))).toBe(false);
  const pinned = await page.evaluate(() => {
    const ed = window.__btgwApp.windows.get('editor').win;
    return ed._buildPin.get('sd/projects/blink');
  });
  expect(pinned.target).toBe('ram');
  await expect(page.locator('.ed-b-build')).toHaveAttribute('aria-label', 'Build blink · ram');
  await expect(page.locator('.ed-b-build')).toHaveAttribute('title', /pinned/);
  await page.evaluate(() => window.__btgwApp.openFile('sd/projects/blink/src/ram_main.c'));
  await expect(page.locator('.ed-b-build')).toHaveAttribute('aria-label', 'Build blink · ram');
});

test('Build and run asks before installing the flash target', async ({ page }) => {
  test.setTimeout(180000);
  await ready(page);
  await seed(page);
  await openSource(page);
  page.once('dialog', async (d) => {
    expect(d.message()).toMatch(/sd:\/projects\/blink\/build\/pico2_w\/blink\.m33\.pex/);
    expect(d.message()).not.toMatch(/signed module/);
    await d.accept();
  });
  await page.click('.ed-b-brun');
  const log = page.locator('.ed-log-body');
  await expect(log).toContainText('Installed blink.', { timeout: 180000 });
  await expect(log).toContainText('Started blink.');
  const listed = await page.evaluate(async () => window.__btgwApp.ws.call('pex.list'));
  expect(listed.modules).toContainEqual({ name: 'blink', state: 'running' });
  const internal = await page.evaluate(async () => {
    const r = await fetch('/api/fs/pex/blink.m33.pex');
    return r.status;
  });
  expect(internal).toBe(404);
});

// Same project Build, against a running gateway (the mcusim image).
// This file is not part of the mock suite: it does not call test.reset.
// A tiny run-ram target is loaded from the card when that image allows
// unsigned modules. The signed board is not this test.
import { test, expect } from '@playwright/test';

const SRC = `#include "pex.h"
#ifndef BLINK_RAM
#error ram
#endif
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    if (reason == PEX_INIT) pex_log(PEX_LOG_INFO, "built in the browser");
    if (reason == PEX_APP_RUN) return PEX_OK;
    return PEX_OK;
}
`;

const PEXFILE = `name tiny
version 0.1.0
kind app
model reloc
default ram

target ram
  board pico2_w
  out build/ram
  run ram
  src src/main.c
  define BLINK_RAM=1
`;

test('sim: Build and run a tiny module from RAM', async ({ page }) => {
  test.setTimeout(300000);
  await page.goto('/');
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected', null, { timeout: 60000 });
  await expect.poll(async () => {
    const r = await page.request.get('/api/sd/files/pex-cc/pexcc.wasm');
    return r.status();
  }, { timeout: 120000 }).toBe(200);
  const files = {
    'boards/pico2_w.board': 'name pico2_w\narch m33\nheader pico2_w.h\n',
    'boards/pico2_w.h': '#define PEX_E2E_HEADER 1\n',
    'projects/tiny/Pexfile': PEXFILE,
    'projects/tiny/src/main.c': SRC,
  };
  for (const [path, body] of Object.entries(files)) {
    const put = await page.request.put('/api/sd/files/' + path.split('/').map(encodeURIComponent).join('/'), { data: body });
    expect(put.ok()).toBe(true);
  }
  await page.evaluate(() => window.__btgwApp.openFile('sd/projects/tiny/src/main.c'));
  await expect(page.locator('.ed-b-brun')).toBeEnabled({ timeout: 15000 });
  await page.click('.ed-b-brun');
  await expect(page.locator('.ed-log-body')).toContainText('Started tiny.', { timeout: 240000 });
  const loaded = await page.evaluate(async () => window.__btgwApp.ws.call('pex.info', { name: 'tiny' }));
  expect(loaded.name).toBe('tiny');
  expect(loaded.state).toBe('running');
});

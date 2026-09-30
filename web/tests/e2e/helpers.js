// A fresh device and a fresh page for every test.
export async function ready(page) {
  await page.goto('/');
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
  await page.evaluate(async () => { localStorage.clear(); await window.__btgwApp.ws.call('test.reset'); });
  await page.reload();
  await page.waitForFunction(() => document.getElementById('conn-status')?.textContent === 'Connected');
}

export const app = (page, fn, arg) => page.evaluate(fn, arg);

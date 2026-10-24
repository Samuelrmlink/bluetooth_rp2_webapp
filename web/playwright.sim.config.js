// Headless Chromium against a gateway that is already up. Does not start
// dev/devserver.py. PEXCC_BASE is the USB-NCM forward, e.g. http://127.0.0.1:8096
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: 'tests/e2e',
  testMatch: 'pexbuild.sim.spec.js',
  fullyParallel: false,
  workers: 1,
  timeout: 300000,
  use: {
    baseURL: process.env.PEXCC_BASE || 'http://127.0.0.1:8096',
    viewport: { width: 1400, height: 820 },
  },
});

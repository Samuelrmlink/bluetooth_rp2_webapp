// End-to-end tests against dev/devserver.py's mock gateway -- a synthetic
// device that speaks the real envelope, with an in-memory LittleFS, a Lua
// runtime that reports script.loaded, and a C1000 streaming telemetry.
//
// One worker: the mock is one device, as a real gateway is, and every test
// starts by resetting it (the mock-only test.reset op).
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: 'tests/e2e',
  fullyParallel: false,
  workers: 1,
  timeout: 30000,
  use: { baseURL: 'http://127.0.0.1:8095', viewport: { width: 1400, height: 820 } },
  webServer: {
    command: 'python3 dev/devserver.py --port 8095',
    url: 'http://127.0.0.1:8095/',
    reuseExistingServer: false,
  },
});

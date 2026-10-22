// Extra windows that live on the gateway rather than in this app.
//
// Any folder js/ext/<name>/ on the device holding an index.js that exports
// register(app) is loaded at startup; register() adds its window(s) through
// app.addWindow(), and from then on each is an ordinary pane (it tiles, tabs,
// keeps its place in a saved arrangement -- addWindow puts a late arrival
// straight into its saved place).
//
// The manifest is the firmware's own directory listing (GET /api/fs/js/ext),
// so adding or removing a module is an upload, never a change here. Another
// project ships its window by uploading js/ext/<name>/... (as an add-on
// project does). A module sits two folders below js/, so it imports the app's
// shared code as '../../controls/kit.js' and the like.
//
// A module that fails to load or register is reported in the Console and
// skipped; it never stops the app or the other modules.

import { list, apiUrl } from './fs.js';

export const EXT_DIR = 'js/ext';
const NAME = /^[A-Za-z0-9_-]+$/;

// The module names in a listing, in load order. Pure, for the unit test.
export function extensionNames(files) {
  return (files || []).filter((f) => f && f.dir && NAME.test(f.name)).map((f) => f.name).sort();
}

export async function loadExtensions(app, { listDir = list, importModule = (u) => import(u) } = {}) {
  const r = await listDir(EXT_DIR);
  if (!r.ok) return [];                          // no js/ext/ (404) or no device: nothing to load
  const loaded = [];
  for (const name of extensionNames(r.files)) {
    const url = new URL(apiUrl(`/${EXT_DIR}/${name}/index.js`), location.href).href;
    try {
      const mod = await importModule(url);
      if (typeof mod.register !== 'function') throw new Error('index.js exports no register(app)');
      await mod.register(app);
      loaded.push(name);
    } catch (e) {
      app.log?.('!', `extension ${name}: ${e?.message || e}`);
    }
  }
  if (loaded.length) app.log?.('app', `extensions loaded: ${loaded.join(', ')}`);
  return loaded;
}

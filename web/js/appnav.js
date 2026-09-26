/*
 * The bar that links the apps together.
 *
 * Each app is its own page in its own directory -- /, /anker/, /script/ --
 * rather than one page with a pane for everything. They share the modules in
 * /js/ and the same websocket API, but a page that only draws a charger does
 * not need to carry a GATT browser, and a reload while debugging one should
 * not disturb the other.
 *
 * Absolute paths throughout, so an app works at whatever depth it is served
 * from and the nav does not have to know where it was loaded from.
 */
export const APPS = [
  { path: '/',         id: 'explorer', title: 'Explorer', hint: 'scan, connect, browse GATT' },
  { path: '/anker/',   id: 'anker',    title: 'Anker',    hint: 'A2687 telemetry' },
  { path: '/pinecil/', id: 'pinecil',  title: 'Pinecil',  hint: 'soldering iron' },
  { path: '/script/',  id: 'script',   title: 'Script',   hint: 'edit Lua on the device' },
];

export function mountAppNav(currentId, host = document.getElementById('appnav')) {
  if (!host) return;
  host.replaceChildren(...APPS.map((a) => {
    const el = document.createElement('a');
    el.className = 'applink';
    el.href = a.path;
    el.textContent = a.title;
    el.title = a.hint;
    if (a.id === currentId) el.setAttribute('aria-current', 'page');
    return el;
  }));
}

/* Every app wants the same header behaviour: a link state pill and the
 * firmware line. Kept here so three pages cannot drift apart. */
export function mountHeader(api, currentId) {
  mountAppNav(currentId);
  const link = document.getElementById('link');
  const fw = document.getElementById('fw');
  api.on('_open', async () => {
    if (link) { link.dataset.state = 'up'; link.textContent = 'connected'; }
    try {
      const info = await api.call('sys.info');
      if (fw) fw.textContent = `${info.firmware} · ${info.modules.join(',')} · ${info.transports.join(',')}`;
    } catch { /* the pill already says we are up; this is decoration */ }
  });
  api.on('_close', () => {
    if (link) { link.dataset.state = 'down'; link.textContent = 'reconnecting…'; }
  });
}

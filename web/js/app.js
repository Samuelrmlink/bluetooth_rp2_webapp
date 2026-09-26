/*
 * Wiring only. Every rule about what a device IS lives in gattdb.js, and every
 * rule about where a pane GOES lives in tiles.js; both are DOM-free and
 * unit-tested. This file is the renderer that joins them to the socket.
 */
import { WsApi } from './wsapi.js';
import { DeviceStore, uuidLabel, hexToAscii, shortUuid } from './gattdb.js';
import { Layout } from './tiles.js';
import { LayoutManager } from './layout.js';
import { mountScriptPane } from './panes/script.js';
import { mountAnkerPane } from './panes/anker.js';

const $ = (id) => document.getElementById(id);
const api = new WsApi();
const store = new DeviceStore().bind(api);

let selected = null;      // {addr, id, uuid, flags}
let scanning = false;

/* ---- log ---------------------------------------------------------------- */
const logLines = [];
function log(kind, text) {
  const t = new Date().toISOString().slice(11, 23);
  logLines.push(`${t} ${kind} ${text}`);
  if (logLines.length > 500) logLines.splice(0, logLines.length - 500);
  const el = $('log-body');
  if (el) {
    el.textContent = logLines.join('\n');
    el.parentElement.scrollTop = el.parentElement.scrollHeight;
  }
}

/* ---- scanner ------------------------------------------------------------ */
function filterParams() {
  const p = { active: $('f-active').checked };
  const n = $('f-name').value.trim(); if (n) p.name_prefix = n;
  const m = $('f-mfg').value.trim(); if (m) p.mfg_prefix = m;
  const u = $('f-uuid').value.trim(); if (u) p.service_uuid = u;
  const r = $('f-rssi').value.trim(); if (r) p.rssi_min = parseInt(r, 10);
  return p;
}

async function toggleScan() {
  try {
    if (scanning) { await api.call('scan.stop'); }
    else { await api.call('scan.start', { duration_ms: 0, ...filterParams() }); }
  } catch (e) { log('!', `scan: ${e.message}`); }
}

function renderScan() {
  const tb = $('scan-table').querySelector('tbody');
  const rows = store.list().sort((a, b) => (b.rssi ?? -127) - (a.rssi ?? -127));
  tb.replaceChildren(...rows.map(d => {
    const tr = document.createElement('tr');
    const uuids = (d.service_uuids || []).join(' ');
    for (const v of [d.addr, d.name || '', d.rssi ?? '', uuids, d.mfg_data || '']) {
      const td = document.createElement('td');
      td.textContent = v;
      if (v === d.mfg_data) td.className = 'wrap';
      tr.appendChild(td);
    }
    const td = document.createElement('td');
    const b = document.createElement('button');
    b.className = 'btn';
    b.textContent = d.connected ? 'Disconnect' : 'Connect';
    b.onclick = async () => {
      try {
        await api.call(d.connected ? 'dev.disconnect' : 'dev.connect', { device: d.addr });
      } catch (e) { log('!', `${d.addr}: ${e.message}`); }
    };
    if (!d.connectable && !d.connected) b.disabled = true;
    td.appendChild(b); tr.appendChild(td);
    return tr;
  }));
}

/* ---- devices ------------------------------------------------------------ */
function renderDevices() {
  const tb = $('dev-table').querySelector('tbody');
  tb.replaceChildren(...store.connected().map(d => {
    const tr = document.createElement('tr');
    for (const v of [d.addr, d.name || '', d.gatt?.mtu ?? '', d.gatt?.services?.length ?? '…']) {
      const td = document.createElement('td'); td.textContent = v; tr.appendChild(td);
    }
    const td = document.createElement('td');
    const b = document.createElement('button');
    b.className = 'btn'; b.textContent = 'Disconnect';
    b.onclick = () => api.call('dev.disconnect', { device: d.addr }).catch(e => log('!', e.message));
    td.appendChild(b); tr.appendChild(td);
    return tr;
  }));
}

/* ---- gatt tree ---------------------------------------------------------- */
function renderTree() {
  const sel = $('tree-dev');
  const conn = store.connected();
  const want = sel.value;
  sel.replaceChildren(...conn.map(d => {
    const o = document.createElement('option');
    o.value = d.addr; o.textContent = `${d.addr}${d.name ? ' — ' + d.name : ''}`;
    return o;
  }));
  if (conn.some(d => d.addr === want)) sel.value = want;

  const d = store.get(sel.value);
  const body = $('tree-body');
  if (!d || !d.gatt) {
    body.replaceChildren();
    $('tree-note').textContent = conn.length ? 'discovering…' : 'no device connected';
    return;
  }
  $('tree-note').textContent =
    `mtu ${d.gatt.mtu}${d.gatt.truncated ? ' — TRUNCATED, raise GATT_MAX_* caps' : ''}`;

  const frag = document.createDocumentFragment();
  for (const s of d.gatt.services || []) {
    const sd = document.createElement('div');
    sd.className = 't-svc'; sd.textContent = uuidLabel(s.uuid);
    frag.appendChild(sd);
    for (const c of s.characteristics || []) {
      const cd = document.createElement('div');
      cd.className = 't-chr';
      cd.setAttribute('aria-selected', String(selected?.addr === d.addr && selected?.id === c.id));
      const name = document.createElement('span'); name.textContent = uuidLabel(c.uuid);
      const fl = document.createElement('span'); fl.className = 't-flags';
      fl.textContent = (c.flags || []).join(',');
      cd.append(name, fl);
      if (c.value) {
        const v = document.createElement('span'); v.className = 't-val';
        v.textContent = c.value.slice(0, 32); cd.appendChild(v);
      }
      cd.onclick = () => { selected = { addr: d.addr, id: c.id, uuid: c.uuid, flags: c.flags || [] }; renderAll(); };
      frag.appendChild(cd);
      for (const x of c.descriptors || []) {
        const dd = document.createElement('div');
        dd.className = 't-dsc'; dd.textContent = uuidLabel(x.uuid);
        frag.appendChild(dd);
      }
    }
  }
  body.replaceChildren(frag);
}

/* ---- characteristic ----------------------------------------------------- */
const charRows = [];
function renderChar() {
  $('char-target').textContent = selected
    ? `${selected.addr}  ${selected.id}  ${shortUuid(selected.uuid)}  [${selected.flags.join(',')}]`
    : 'select a characteristic';
  const tb = $('c-log').querySelector('tbody');
  tb.replaceChildren(...charRows.slice(-200).reverse().map(r => {
    const tr = document.createElement('tr');
    for (const v of [r.t, r.src, r.len, r.hex, r.ascii]) {
      const td = document.createElement('td'); td.textContent = v;
      if (v === r.hex || v === r.ascii) td.className = 'wrap';
      tr.appendChild(td);
    }
    return tr;
  }));
}

async function charOp(op, extra = {}) {
  if (!selected) { log('!', 'no characteristic selected'); return; }
  try {
    const r = await api.call(op, { device: selected.addr, id: selected.id, ...extra });
    log('<', `${op} ${JSON.stringify(r)}`);
  } catch (e) { log('!', `${op}: ${e.code || ''} ${e.message}`); }
}

/* ---- layout ------------------------------------------------------------- */
const PANES = [
  { id: 'scanner', title: 'Scanner',        el: $('pane-scanner') },
  { id: 'devices', title: 'Devices',        el: $('pane-devices') },
  { id: 'tree',    title: 'GATT Tree',      el: $('pane-tree') },
  { id: 'char',    title: 'Characteristic', el: $('pane-char') },
  { id: 'anker',   title: 'Anker',          el: $('pane-anker') },
  { id: 'script',  title: 'Script',         el: $('pane-script') },
  { id: 'log',     title: 'Log',            el: $('pane-log') },
];

const STORE_KEY = 'btgw.layout';

/* The arrangement the app opens with when nothing is stored. Note the SHAPE:
 * Layout.fromJSON takes {tree, floating}, exactly what Layout.toJSON emits,
 * and a list of the pane ids that may be placed. Hand it a bare tree array
 * and it finds no .tree, falls back to one empty leaf, and every slot reads
 * "choose a pane" forever. Hand it a tree but no known-pane list and every
 * leaf is rejected as unknown, with the same result. Both of those shipped. */
const DEFAULT_LAYOUT = {
  tree: ['row', 0.42, 'scanner', ['col', 0.55, 'tree', 'char']],
  floating: [],
};

const PANE_IDS = PANES.map((p) => p.id);

function loadLayout() {
  try {
    const raw = JSON.parse(localStorage.getItem(STORE_KEY) || 'null');
    // A stored layout predating a new pane simply will not mention it; that
    // is fine and the pane stays parked until someone places it. A stored
    // layout that is corrupt must not stop the app starting.
    if (raw && raw.tree !== undefined) return Layout.fromJSON(raw, PANE_IDS);
  } catch { /* fall through to the default */ }
  return Layout.fromJSON(DEFAULT_LAYOUT, PANE_IDS);
}

const lm = new LayoutManager({
  panes: PANES,
  layout: loadLayout(),
  mode: 'auto',
  onChange: () => {
    try {
      localStorage.setItem(STORE_KEY, JSON.stringify(lm.layout.toJSON()));
    } catch { /* private mode, quota: not worth failing over */ }
  },
});

/* Panes that own their own state mount once and manage themselves; renderAll
 * is only for the ones driven by the shared device store. */
const scriptPane = mountScriptPane({ api, log });
const ankerPane = mountAnkerPane({ api, log });

/* LayoutManager's constructor builds the tab bar and wires events but does
 * NOT lay the tiles out -- apply() does, and nothing called it, so the page
 * opened with an empty workspace until a window resize happened to fire. */
lm.apply();

function renderAll() { renderScan(); renderDevices(); renderTree(); renderChar(); }

/* ---- events ------------------------------------------------------------- */
store.subscribe(renderAll);

api.on('_open', async () => {
  $('link').dataset.state = 'up'; $('link').textContent = 'connected';
  log('#', 'websocket up');
  try {
    const info = await api.call('sys.info');
    $('fw').textContent = `${info.firmware} · ${info.modules.join(',')} · ${info.transports.join(',')}`;
    const l = await api.call('scan.list');
    for (const d of l.devices || []) store.upsert(d);
    scanning = !!l.scanning;
    $('scan-toggle').setAttribute('aria-pressed', String(scanning));
    $('scan-toggle').textContent = scanning ? 'Stop' : 'Scan';
  } catch (e) { log('!', e.message); }
});
api.on('_close', () => {
  $('link').dataset.state = 'down'; $('link').textContent = 'reconnecting…';
});
api.on('scan.done', d => {
  scanning = false;
  $('scan-toggle').setAttribute('aria-pressed', 'false');
  $('scan-toggle').textContent = 'Scan';
  log('#', `scan done (${d?.reason || ''})`);
});
api.on('char.value', d => {
  charRows.push({
    t: new Date().toISOString().slice(11, 23), src: d.source, len: d.len,
    hex: d.value, ascii: hexToAscii(d.value),
  });
  renderChar();
});
api.on('*', m => {
  if ($('log-events')?.checked && m.evt !== 'scan.device') {
    log('<', `${m.evt} ${JSON.stringify(m.data).slice(0, 220)}`);
  }
});
for (const t of ['security.required', 'security.bonded', 'security.failed',
                 'security.passkey_request', 'security.numeric_comparison']) {
  api.on(t, d => log('#', `${t}: ${JSON.stringify(d)}`));
}

/* ---- controls ----------------------------------------------------------- */
$('scan-toggle').onclick = async () => {
  await toggleScan();
  scanning = !scanning;
  $('scan-toggle').setAttribute('aria-pressed', String(scanning));
  $('scan-toggle').textContent = scanning ? 'Stop' : 'Scan';
};
$('f-clear').onclick = () => { store.devices.clear(); renderAll(); };
$('tree-dev').onchange = renderTree;
$('c-read').onclick = () => charOp('gatt.read');
$('c-notify').onclick = () => charOp('gatt.subscribe', { mode: 'notify' });
$('c-indicate').onclick = () => charOp('gatt.subscribe', { mode: 'indicate' });
$('c-off').onclick = () => charOp('gatt.subscribe', { mode: 'off' });
$('c-send').onclick = () => charOp('gatt.write', {
  value: $('c-write').value.replace(/[^0-9a-fA-F]/g, ''),
  response: $('c-resp').checked,
});
$('log-clear').onclick = () => { logLines.length = 0; $('log-body').textContent = ''; };

renderAll();
api.connect();

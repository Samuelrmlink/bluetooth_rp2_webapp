// One Tools heading per other board heard on _btgw._tcp.
//
// Every board gets Editor [<name>] and Console [<name>]. Scanner, GATT, and
// GATT Events are added when that board's sys.info says it has LE. Hardware
// is added when it lists the io module. The heading is "Tools [<name>]".
// The page's own Tools heading is unchanged; those rows gain [Local] only
// while one of these windows is registered.
//
// The editor is this same page in an iframe, because a second EditorWindow
// would reuse the editor's dialog ids. The frame opens /ws and pins `node`
// to this board. Every other window shares the outer page's /ws and sends
// the same `node`. Nothing dials /ws/peer/<name>.

import { Bus } from '../bus.js';
import { DeviceStore } from '../gattdb.js';
import { GattLog } from '../gattlog.js';
import { ConsoleWindow } from './console.js';
import { ScannerWindow } from './scanner.js';
import { GattWindow } from './gatt.js';
import { GattEventsWindow } from './gattevents.js';
import { HardwareWindow } from './hardware.js';
import { peerRoles, roleTitle, scopeOf, toolGroup } from './peercaps.js';

const GONE_MS = 30000;
const POLL_MS = 2000;
const BROWSE_MS = 20000;
const LEARN_RETRY_MS = 8000;

const TOOLS = {
  console: ConsoleWindow,
  scanner: ScannerWindow,
  gatt: GattWindow,
  'gatt-events': GattEventsWindow,
  hardware: HardwareWindow,
};

export function editorTitle(name) {
  return roleTitle('Editor', name);
}

function waitOpen(ws, ms = 4000) {
  if (ws.connected) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { off(); reject(new Error('timed out')); }, ms);
    const off = ws.on('_open', () => { clearTimeout(timer); off(); resolve(); });
  });
}

// The app object a remote window sees. devices and the GATT log are that
// board's, so its scanner cannot show the page's own radio. Prefs start as
// a copy and stay here: writing them through would replace the local
// scanner's saved filters.
class PeerApp {
  constructor(session) {
    this._s = session;
    this._app = session.app;
    this.devices = session.devices;
    this.gattLog = session.gattLog;
    this.prefs = { ...session.app.prefs };
    this.popover = session.app.popover;
  }

  get tabsMode() { return this._app.tabsMode; }

  registerDialog(d) { return this._app.registerDialog(d); }

  setPref(key, value) {
    this.prefs[key] = value;
    for (const id of this._s.toolIds) {
      this._app.windows.get(id)?.win.onPrefs?.(this.prefs);
    }
  }

  showDevice(addr) {
    this._app.show(`gatt@${this._s.name}`)?.showDevice(addr);
  }

  log(src, text, level = src === '!' ? 'error' : undefined) {
    this._app.window(`console@${this._s.name}`)?.add('info', src, text, level);
  }
}

class PeerSession {
  constructor(app, name) {
    this.app = app;
    this.name = name;
    this.scope = scopeOf(name);
    this.toolIds = [];
    this._scopes = [];
    this.held = 0;
    this.ws = app.ws.scope(name);
    this.bus = new Bus(this.ws);
    this.devices = new DeviceStore().bind(this.ws);
    this.gattLog = new GattLog(500);
    this._gl = this.bus.owner();
    this._scopes.push(this._gl);
    this._gl.on('char.value', (d) => this.gattLog.add({
      addr: d.addr, id: d.id, uuid: d.uuid, src: d.source || 'notify', hex: d.value || '',
    }));
    this._gl.on('gatt.op_failed', (d) => this.gattLog.add({
      addr: d.addr,
      src: 'error',
      note: `${d.op} failed, ATT error 0x${Number(d.att_error).toString(16).padStart(2, '0')} (characteristic #${d.chr})`,
    }));
    this.facade = new PeerApp(this);
    // The page socket is often already open when this board is first heard.
    this._up = !!this.ws.connected;
    this.ws.on('_open', () => {
      this._up = true;
      this._visibleCall('onConnect');
    });
    // A handshake that never opened is a retry, not a drop the Console
    // should announce. Park does not emit this event.
    this.ws.on('_close', () => {
      if (!this._up) return;
      this._up = false;
      this._visibleCall('onDisconnect');
    });
  }

  ctx(owner) {
    const bus = this.bus.owner();
    this._scopes.push(bus);
    return { app: this.facade, ws: this.ws, bus, owner, scope: this.scope };
  }

  acquire() {
    this.held++;
    if (this.held === 1) this.ws.unpark();
  }

  release() {
    this.held = Math.max(0, this.held - 1);
    if (this.held === 0) {
      // Park is not a drop. The next open is a fresh connect.
      this._up = false;
      this.ws.park();
    }
  }

  _visibleCall(method) {
    for (const id of this.toolIds) {
      const rec = this.app.windows.get(id);
      if (!rec?.el || rec.el.closest('#pane-park')) continue;
      rec.win[method]?.();
    }
  }

  dispose() {
    for (const scope of this._scopes) scope.dispose();
    this.devices.dispose();
    this.gattLog.dispose();
    this.ws.close();
  }
}

export class PeerHost {
  constructor(app) {
    this.app = app;
    this.peers = new Map();
    this._timer = null;
    this._flight = false;
    this._learning = false;
    this._browseAt = 0;
    app.ws.on('_open', () => this._start());
    app.ws.on('_close', () => this._stop());
  }

  sessionFor(name) {
    return this.peers.get(name)?.session || null;
  }

  _start() {
    if (this._timer) return;
    this._browse();
    this._refresh();
    this._timer = setInterval(() => {
      this._refresh();
      if (Date.now() - this._browseAt >= BROWSE_MS) this._browse();
    }, POLL_MS);
  }

  _stop() {
    clearInterval(this._timer);
    this._timer = null;
  }

  async _browse() {
    this._browseAt = Date.now();
    try {
      await this.app.ws.call('mdns.browse');
    } catch {
      // An image without the op still answers mdns.peers from what it has heard.
    }
  }

  async _refresh() {
    if (this._flight) return;
    this._flight = true;
    try {
      const r = await this.app.ws.call('mdns.peers');
      const peers = Array.isArray(r?.peers) ? r.peers : [];
      const now = Date.now();
      const live = new Set();
      for (const p of peers) {
        if (!p?.name || p.self) continue;
        live.add(p.name);
        let row = this.peers.get(p.name);
        if (!row) {
          row = { ids: [], session: new PeerSession(this.app, p.name), info: null, tried: 0 };
          this.peers.set(p.name, row);
          this._addRoles(p.name, row, peerRoles(null));
        }
        row.seen = now;
      }
      for (const [name, row] of this.peers) {
        if (live.has(name) || now - (row.seen || 0) < GONE_MS) continue;
        if (this._onScreen(row)) continue;
        this._drop(name, row);
      }
      this._announce();
      this._scheduleLearn();
    } catch {
      // Not connected yet, or this image has no peer list.
    } finally {
      this._flight = false;
    }
  }

  _announce() {
    const names = [...this.peers.keys()];
    const key = names.join('\n');
    if (key === this._announced) return;
    this._announced = key;
    this.app.peersChanged(names);
  }

  _onScreen(row) {
    return row.ids.some((id) => {
      const el = this.app.windows.get(id)?.el;
      return el && !el.closest('#pane-park');
    });
  }

  _addRoles(name, row, roles) {
    const group = toolGroup(name);
    const added = [];
    for (const role of roles) {
      const id = `${role.id}@${name}`;
      if (row.ids.includes(id)) continue;
      const title = roleTitle(role.title, name);
      if (role.id === 'editor') {
        this.app.addWindow(id, { title, short: title, group }, (el) => this._view(el, name));
      } else {
        const Ctor = TOOLS[role.id];
        if (!Ctor) continue;
        this.app.addWindow(id, { title, short: title, group }, (el) => this._mount(row.session, Ctor, id, el));
        row.session.toolIds.push(id);
      }
      row.ids.push(id);
      added.push(role.title);
    }
    if (added.length) this.app.log('mdns', `${group} — ${added.join(', ')}`);
  }

  // Read sys.info on the page socket, with this board's name in `node`.
  async _learn(name, row) {
    const borrowed = row.session.held === 0;
    if (borrowed) row.session.acquire();
    try {
      await waitOpen(row.session.ws, 4000);
      const info = await row.session.ws.call('sys.info');
      if (this.peers.get(name) !== row) return;
      row.info = info || {};
      this._addRoles(name, row, peerRoles(row.info));
    } catch {
      row.tried = Date.now();
    } finally {
      if (borrowed) row.session.release();
    }
  }

  _scheduleLearn() {
    if (this._learning) return;
    const now = Date.now();
    for (const [name, row] of this.peers) {
      if (row.info) continue;
      if (row.tried && now - row.tried < LEARN_RETRY_MS) continue;
      this._learning = true;
      this._learn(name, row).finally(() => {
        this._learning = false;
        this._scheduleLearn();
      });
      return;
    }
  }

  _drop(name, row) {
    for (const id of [...row.ids]) this.app.removeWindow(id);
    row.session.dispose();
    this.peers.delete(name);
    this.app.log('mdns', `${toolGroup(name)} went quiet — its windows were removed`);
  }

  _mount(session, Ctor, owner, el) {
    const win = new Ctor(el, session.ctx(owner));
    const show = win.onShow?.bind(win);
    const hide = win.onHide?.bind(win);
    win.onShow = () => {
      const live = session.ws.connected;
      session.acquire();
      show?.();
      // Already dialed (another of this board's windows, or Settings).
      // A fresh dial notifies from _open once the socket is up.
      if (live) win.onConnect?.();
    };
    win.onHide = () => {
      hide?.();
      session.release();
    };
    return win;
  }

  // The frame is built the first time the window is shown, and kept after
  // that, so switching to another pane does not throw away the open files.
  _view(el, name) {
    el.classList.add('pane-peer-editor');
    let frame = null;
    return {
      onShow() {
        if (frame) return;
        const q = new URLSearchParams(location.search);
        q.delete('ws');
        q.set('peer', name);
        q.set('embed', '1');
        frame = document.createElement('iframe');
        frame.className = 'peer-editor-frame';
        frame.title = editorTitle(name);
        frame.src = `${location.pathname}?${q.toString()}`;
        el.appendChild(frame);
      },
    };
  }
}

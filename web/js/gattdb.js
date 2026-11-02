/*
 * A browser-side mirror of the devices the gateway knows about.
 *
 * DOM-free and unit-tested, for the same reason tiles.js is: the rules about
 * what a device IS belong somewhere a test can reach without a browser, and
 * the panes become renderers rather than a second copy of those rules.
 *
 * The firmware addresses GATT objects by path index ("2.1") and by UUID; this
 * keeps both, so a pane can show a friendly UUID while acting on a stable path.
 */

export class DeviceStore {
  constructor() {
    this.devices = new Map();   // addr -> device record
    this._subs = new Set();
  }

  subscribe(fn) { this._subs.add(fn); return () => this._subs.delete(fn); }
  dispose() { this._subs.clear(); }
  _changed() { for (const f of this._subs) { try { f(this); } catch (e) { console.error(e); } } }

  get(addr) { return this.devices.get(addr); }
  list() { return [...this.devices.values()]; }
  connected() { return this.list().filter(d => d.connected); }

  /** A scan.device event, or one entry of a scan.list reply. */
  upsert(rec) {
    if (!rec || !rec.addr) return;
    const prev = this.devices.get(rec.addr) || { addr: rec.addr, gatt: null };
    // Merge rather than replace: successive advertisements carry different
    // subsets, exactly as they do on the wire, and a later one without a name
    // must not erase the name an earlier one supplied.
    const next = { ...prev, ...rec, seen: Date.now() };
    if (rec.name === undefined && prev.name !== undefined) next.name = prev.name;
    if (rec.mfg_data === undefined && prev.mfg_data !== undefined) next.mfg_data = prev.mfg_data;
    if (rec.service_uuids === undefined && prev.service_uuids !== undefined) {
      next.service_uuids = prev.service_uuids;
    }
    // The raw advertisement and scan response (fw 1.85+) arrive separately.
    if (rec.adv === undefined && prev.adv !== undefined) next.adv = prev.adv;
    if (rec.rsp === undefined && prev.rsp !== undefined) next.rsp = prev.rsp;
    this.devices.set(rec.addr, next);
    this._changed();
  }

  /** Forget a device (the Scanner's expiry and Clear list). */
  remove(addr) {
    if (this.devices.delete(addr)) this._changed();
  }

  setConnected(addr, connected) {
    const d = this.devices.get(addr);
    if (!d) return;
    d.connected = connected;
    if (!connected) d.gatt = null;    // the tree belongs to the connection
    this._changed();
  }

  /** A gatt.discovered event: {addr, gatt:{...}}. */
  setTree(addr, gatt) {
    const d = this.devices.get(addr) || { addr };
    d.gatt = gatt;
    d.connected = true;
    this.devices.set(addr, d);
    this._changed();
  }

  /** A gatt.changed delta: {added:[], removed:[ids], updated:[{id,...}]}. */
  applyDelta(addr, delta) {
    const d = this.devices.get(addr);
    if (!d || !d.gatt) return false;
    const chars = new Map();
    for (const s of d.gatt.services || []) {
      for (const c of s.characteristics || []) chars.set(c.id, c);
    }
    for (const id of delta.removed || []) {
      for (const s of d.gatt.services || []) {
        s.characteristics = (s.characteristics || []).filter(c => c.id !== id);
      }
      d.gatt.services = (d.gatt.services || []).filter(s => s.id !== id);
    }
    for (const u of delta.updated || []) {
      const c = chars.get(u.id);
      if (c) Object.assign(c, u);
    }
    for (const a of delta.added || []) {
      const [si] = String(a.id).split('.');
      const svc = (d.gatt.services || []).find(s => s.id === si);
      if (svc) (svc.characteristics ||= []).push(a);
      else (d.gatt.services ||= []).push(a);
    }
    this._changed();
    return true;
  }

  /** The most recent value seen for a characteristic, from char.value. */
  setValue(addr, id, value, source) {
    const d = this.devices.get(addr);
    if (!d || !d.gatt) return;
    for (const s of d.gatt.services || []) {
      for (const c of s.characteristics || []) {
        if (c.id === id) {
          c.value = value;
          c.value_source = source;
          c.value_at = Date.now();
          this._changed();
          return;
        }
      }
    }
  }

  /** Wire the store to a WsApi so panes only ever read the store. */
  bind(api) {
    api.on('scan.device', d => this.upsert(d));
    api.on('dev.connected', d => { this.upsert(d); this.setConnected(d.addr, true); });
    api.on('dev.disconnected', d => this.setConnected(d.addr, false));
    api.on('gatt.discovered', d => this.setTree(d.addr, d.gatt));
    api.on('gatt.changed', d => this.applyDelta(d.device || d.addr, d));
    api.on('char.value', d => this.setValue(d.addr, d.id, d.value, d.source));
    return this;
  }
}

/** "00001801-0000-1000-8000-00805f9b34fb" and "1801" both render short. */
export function shortUuid(u) {
  if (!u) return '';
  const m = /^0000([0-9a-f]{4})-0000-1000-8000-00805f9b34fb$/i.exec(u);
  return m ? m[1].toLowerCase() : u.toLowerCase();
}

// Names for the UUIDs this gateway is likely to meet: the common Bluetooth SIG
// ones, and the 128-bit services it actually talks to (full, lowercase).
const NAMES = {
  // services
  '1800': 'Generic Access', '1801': 'Generic Attribute', '1802': 'Immediate Alert', '1803': 'Link Loss',
  '1804': 'Tx Power', '1805': 'Current Time', '180a': 'Device Information', '180d': 'Heart Rate',
  '180f': 'Battery', '1812': 'Human Interface Device', '1816': 'Cycling Speed and Cadence',
  '181a': 'Environmental Sensing', '181c': 'User Data', '1826': 'Fitness Machine',
  // characteristics
  '2a00': 'Device Name', '2a01': 'Appearance', '2a04': 'Connection Parameters', '2a05': 'Service Changed',
  '2a06': 'Alert Level', '2a07': 'Tx Power Level', '2a19': 'Battery Level', '2a23': 'System ID',
  '2a24': 'Model Number', '2a25': 'Serial Number', '2a26': 'Firmware Revision', '2a27': 'Hardware Revision',
  '2a28': 'Software Revision', '2a29': 'Manufacturer Name', '2a2b': 'Current Time', '2a37': 'Heart Rate Measurement',
  '2a38': 'Body Sensor Location', '2a4d': 'Report', '2a50': 'PnP ID', '2a6e': 'Temperature', '2a6f': 'Humidity',
  '2aa6': 'Central Address Resolution', '2b29': 'Client Supported Features', '2b2a': 'Database Hash',
  '2b3a': 'Server Supported Features',
  // descriptors
  '2900': 'Extended Properties', '2901': 'User Description', '2902': 'CCCD', '2904': 'Presentation Format',
  // advertised, vendor
  'ff09': 'Anker (advertised)',
  '8c850001-0302-41c5-b46e-cf057c562025': 'Anker',
  '8c850002-0302-41c5-b46e-cf057c562025': 'Anker command',
  '8c850003-0302-41c5-b46e-cf057c562025': 'Anker telemetry',
  '6e400001-b5a3-f393-e0a9-e50e24dcca9e': 'Nordic UART',
  '6e400002-b5a3-f393-e0a9-e50e24dcca9e': 'Nordic UART RX',
  '6e400003-b5a3-f393-e0a9-e50e24dcca9e': 'Nordic UART TX',
};

export function uuidLabel(u) {
  const s = shortUuid(u);
  return NAMES[s] ? `${s} ${NAMES[s]}` : s;
}

export function hexToAscii(hex) {
  if (!hex) return '';
  let out = '';
  for (let i = 0; i + 1 < hex.length; i += 2) {
    const b = parseInt(hex.slice(i, i + 2), 16);
    out += (b >= 0x20 && b < 0x7f) ? String.fromCharCode(b) : '.';
  }
  return out;
}

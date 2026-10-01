// Every GATT value and operation result, for every device, in one place.
//
// One log for the whole app (app.gattLog), so the GATT window's "last value"
// and every GATT Events view -- the window, and the one embedded in GATT in
// tabbed mode -- show the same thing. DOM-free (tests/unit/gattfmt.test.js).
//
//   { t: ms, addr, id, uuid, src: 'read'|'notify'|'indicate'|'write'|'error', len, hex, note }

export class GattLog {
  constructor(max = 500) {
    this.max = max;
    this.items = [];
    this._last = new Map();          // addr|id -> the last VALUE (not a write or an error)
    this._subs = new Set();
  }

  add(e) {
    const item = { t: Date.now(), len: e.hex ? e.hex.length >> 1 : 0, ...e };
    this.items.push(item);
    if (this.items.length > this.max) this.items.splice(0, this.items.length - this.max);
    if (item.src === 'read' || item.src === 'notify' || item.src === 'indicate') {
      this._last.set(`${item.addr}|${item.id}`, item);
    }
    for (const fn of this._subs) { try { fn(item); } catch (err) { console.error(err); } }
    return item;
  }

  // The last value read, notified or indicated for a characteristic.
  last(addr, id) { return this._last.get(`${addr}|${id}`) || null; }

  list(addr) { return addr ? this.items.filter((i) => i.addr === addr) : this.items.slice(); }

  subscribe(fn) { this._subs.add(fn); return () => this._subs.delete(fn); }

  clear() { this.items = []; this._last.clear(); for (const fn of this._subs) fn(null); }
}

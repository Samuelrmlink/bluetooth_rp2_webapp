/*
 * The websocket API client.
 *
 * Two jobs, kept apart: request/reply correlation by `id`, which turns an op
 * into a promise, and an event emitter for everything unsolicited. The
 * firmware guarantees a reply carries the `id` it was sent and an event never
 * does, so the split is unambiguous.
 *
 * DOM-free and unit-tested; the panes are what own the DOM.
 */

export class WsApi {
  constructor(url) {
    this._url = url || `ws://${location.host}/ws`;
    this._ws = null;
    this._nextId = 1;
    this._pending = new Map();      // id -> {resolve, reject, timer}
    this._listeners = new Map();    // topic -> Set<fn>
    this._connected = false;
    this._backoff = 500;
    this.timeoutMs = 10000;
  }

  get connected() { return this._connected; }

  connect() {
    if (this._ws) return;
    const ws = new WebSocket(this._url);
    this._ws = ws;
    ws.onopen = () => {
      this._connected = true;
      this._backoff = 500;
      this._emit('_open', {});
    };
    ws.onclose = () => {
      this._connected = false;
      this._ws = null;
      // Every in-flight request is now unanswerable; fail them rather than
      // leaving callers waiting on a socket that is gone.
      for (const [, p] of this._pending) {
        clearTimeout(p.timer);
        p.reject(new Error('disconnected'));
      }
      this._pending.clear();
      this._emit('_close', {});
      setTimeout(() => this.connect(), this._backoff);
      this._backoff = Math.min(this._backoff * 2, 10000);
    };
    ws.onerror = () => { /* onclose always follows; handled there */ };
    ws.onmessage = (ev) => this._onText(ev.data);
  }

  _onText(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }

    if (msg.evt !== undefined) {
      this._emit(msg.evt, msg.data);
      this._emit('*', { evt: msg.evt, data: msg.data });
      return;
    }
    const p = this._pending.get(msg.id);
    if (!p) return;                  // a reply to a request we gave up on
    this._pending.delete(msg.id);
    clearTimeout(p.timer);
    if (msg.ok) p.resolve(msg.result);
    else p.reject(Object.assign(new Error(msg.error?.message || 'error'),
                                { code: msg.error?.code || 'error' }));
  }

  /** Call an op. Resolves with `result`, rejects with an Error carrying .code. */
  call(op, params = {}) {
    return new Promise((resolve, reject) => {
      if (!this._connected) { reject(new Error('not connected')); return; }
      const id = this._nextId++;
      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(Object.assign(new Error(`${op} timed out`), { code: 'timeout' }));
      }, this.timeoutMs);
      this._pending.set(id, { resolve, reject, timer });
      this._ws.send(JSON.stringify({ id, op, params }));
    });
  }

  on(topic, fn) {
    if (!this._listeners.has(topic)) this._listeners.set(topic, new Set());
    this._listeners.get(topic).add(fn);
    return () => this.off(topic, fn);
  }

  off(topic, fn) { this._listeners.get(topic)?.delete(fn); }

  _emit(topic, data) {
    for (const fn of this._listeners.get(topic) || []) {
      // One bad listener must not stop the others, or a render error in one
      // pane silently kills every other pane's updates.
      try { fn(data); } catch (e) { console.error('listener for', topic, e); }
    }
  }
}

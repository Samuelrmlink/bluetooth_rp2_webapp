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
  constructor(url, node) {
    this._url = url || `ws://${location.host}/ws`;
    // A frame opened for one other board pins that board. Every call on this
    // socket names it, and events for any other board are ignored. The main
    // page leaves this empty and uses scope() instead of a second socket.
    this._node = node || '';
    this._scopes = new Map();
    this._ws = null;
    this._nextId = 1;
    this._pending = new Map();      // id -> {resolve, reject, timer}
    this._listeners = new Map();    // topic -> Set<fn>
    this._connected = false;
    this._backoff = 500;
    this._closed = false;
    this._parked = false;
    this._retry = 0;
    this.timeoutMs = 10000;
  }

  get connected() { return this._connected; }

  // The same page socket, aimed at one other board. Calls send `node` first.
  // Events for that board stay on this view; the page's own listeners do not
  // see them. Closing the view does not close the page socket.
  scope(node) {
    if (!node || node === this._node) return this;
    if (this._node) return this;
    let view = this._scopes.get(node);
    if (!view) {
      view = new NodeView(this, node);
      this._scopes.set(node, view);
    }
    return view;
  }

  // Stop reconnecting and drop listeners. A parked socket is not closed:
  // the same object dials again later, and its listeners are still there.
  close() {
    this._closed = true;
    clearTimeout(this._retry);
    this._silence();
    this._listeners.clear();
  }

  // Drop the live socket and do not dial again until unpark(). Remote tools
  // share one socket per board and park it while none of them is on screen,
  // so a file request still has a proxy slot.
  park() {
    if (this._closed) return;
    this._parked = true;
    clearTimeout(this._retry);
    this._silence();
  }

  unpark() {
    if (this._closed) return;
    this._parked = false;
    this.connect();
  }

  _silence() {
    const ws = this._ws;
    this._ws = null;
    this._connected = false;
    if (ws) {
      ws.onopen = null;
      ws.onclose = null;
      ws.onerror = null;
      ws.onmessage = null;
      try { ws.close(); } catch { /* already closed */ }
    }
    for (const [, p] of this._pending) {
      clearTimeout(p.timer);
      p.reject(new Error('disconnected'));
    }
    this._pending.clear();
  }

  connect() {
    if (this._closed || this._parked || this._ws) return;
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
      if (this._closed || this._parked) return;
      this._retry = setTimeout(() => this.connect(), this._backoff);
      this._backoff = Math.min(this._backoff * 2, 10000);
    };
    ws.onerror = () => { /* onclose always follows; handled there */ };
    ws.onmessage = (ev) => this._onText(ev.data);
  }

  _onText(text) {
    let msg;
    try { msg = JSON.parse(text); } catch { return; }

    if (msg.evt !== undefined) {
      const node = msg.node || '';
      // A pinned socket is already that board. Anything else is the entry
      // board, or a second hop, and is not delivered here.
      if (this._node) {
        if (node !== this._node) return;
        this._emit(msg.evt, msg.data);
        this._emit('*', { evt: msg.evt, data: msg.data, node });
        return;
      }
      if (node) {
        this._scopes.get(node)?._deliver(msg.evt, msg.data);
        return;
      }
      this._emit(msg.evt, msg.data);
      this._emit('*', { evt: msg.evt, data: msg.data });
      return;
    }
    if (this._node && (msg.node || '') !== this._node) return;
    const p = this._pending.get(msg.id);
    if (!p) return;                  // a reply to a request we gave up on
    this._pending.delete(msg.id);
    clearTimeout(p.timer);
    if (msg.ok) p.resolve(msg.result);
    else p.reject(Object.assign(new Error(msg.error?.message || 'error'),
                                { code: msg.error?.code || 'error' }));
  }

  /** Call an op. Resolves with `result`, rejects with an Error carrying .code.
   *  `node` is optional and names another board; this board forwards it. */
  call(op, params = {}, node) {
    return new Promise((resolve, reject) => {
      if (!this._connected) { reject(new Error('not connected')); return; }
      const id = this._nextId++;
      const timer = setTimeout(() => {
        this._pending.delete(id);
        reject(Object.assign(new Error(`${op} timed out`), { code: 'timeout' }));
      }, this.timeoutMs);
      this._pending.set(id, { resolve, reject, timer });
      /* `node` is the other board's mDNS name. It is serialized first so the
       * board that holds this socket can forward the op. Omit it for the
       * board this socket is already talking to. A pinned socket fills it in. */
      const dest = node || this._node;
      const msg = dest ? { node: dest, id, op, params } : { id, op, params };
      this._ws.send(JSON.stringify(msg));
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
    if (this._node || (topic !== '_open' && topic !== '_close')) return;
    for (const view of this._scopes.values()) view._deliver(topic, data);
  }
}

// One other board on a shared page socket. It has the WsApi methods the
// windows and Settings use, and it does not open or close that socket.
class NodeView {
  constructor(api, node) {
    this._api = api;
    this._node = node;
    this._listeners = new Map();
  }

  get connected() { return this._api.connected; }

  connect() { this._api.connect(); }

  unpark() { this._api.unpark(); }

  // The page socket stays up. Parking a board only means no window is
  // holding it; the entry board keeps the outgoing connection.
  park() {}

  close() {
    this._listeners.clear();
    this._api._scopes.delete(this._node);
  }

  call(op, params = {}, node) {
    return this._api.call(op, params, node || this._node);
  }

  on(topic, fn) {
    if (!this._listeners.has(topic)) this._listeners.set(topic, new Set());
    this._listeners.get(topic).add(fn);
    return () => this.off(topic, fn);
  }

  off(topic, fn) { this._listeners.get(topic)?.delete(fn); }

  _deliver(topic, data) {
    for (const fn of this._listeners.get(topic) || []) {
      try { fn(data); } catch (e) { console.error('listener for', topic, e); }
    }
    if (topic === '_open' || topic === '_close' || topic === '*') return;
    const wrap = { evt: topic, data, node: this._node };
    for (const fn of this._listeners.get('*') || []) {
      try { fn(wrap); } catch (e) { console.error('listener for *', e); }
    }
  }
}

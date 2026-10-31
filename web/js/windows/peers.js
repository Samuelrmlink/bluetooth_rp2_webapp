// Boards advertising _btgw._tcp. Picking one opens that board's editor in
// this pane. The frame is the same app with ?peer=&embed=1, so its files
// and websocket go through /api/peer/<name>/ and /ws/peer/<name>/.

export class PeersWindow {
  constructor(el, ctx) {
    this.ws = ctx.ws;
    this.peers = [];
    this._err = '';
    this._timer = null;
    this._open = '';
    el.innerHTML = `
      <div class="pane-bar">
        <button type="button" class="pane-btn peer-search">Search</button>
        <button type="button" class="pane-btn peer-back" hidden>Boards</button>
        <span class="peer-current"></span>
        <span class="spacer"></span>
        <span class="dim peer-status"></span>
      </div>
      <div class="peer-list"></div>
      <iframe class="peer-frame" title="Board editor" hidden></iframe>`;
    this._list = el.querySelector('.peer-list');
    this._frame = el.querySelector('.peer-frame');
    this._back = el.querySelector('.peer-back');
    this._current = el.querySelector('.peer-current');
    this._status = el.querySelector('.peer-status');
    el.querySelector('.peer-search').addEventListener('click', () => this._search());
    this._back.addEventListener('click', () => this._close());
  }

  onShow() {
    this._refresh();
    if (!this._timer) this._timer = setInterval(() => this._refresh(), 2000);
  }

  onHide() {
    clearInterval(this._timer);
    this._timer = null;
  }

  async _search() {
    this._status.textContent = 'searching';
    try {
      await this.ws.call('mdns.browse');
      this._err = '';
    } catch (e) {
      this._err = e?.message || String(e);
    }
    await this._refresh();
  }

  async _refresh() {
    if (this._open) return;
    try {
      const r = await this.ws.call('mdns.peers');
      this.peers = Array.isArray(r?.peers) ? r.peers : [];
      this._err = '';
    } catch (e) {
      this._err = e?.message || String(e);
    }
    this._draw();
  }

  _draw() {
    const others = this.peers.filter((p) => p && p.name && !p.self);
    this._status.textContent = this._err || (others.length ? `${others.length} board${others.length === 1 ? '' : 's'}` : '');
    this._list.replaceChildren();
    if (!others.length) {
      const empty = document.createElement('div');
      empty.className = 'peer-empty';
      empty.textContent = this._err
        ? this._err
        : 'No other boards yet. Search looks for _btgw._tcp on this network.';
      this._list.appendChild(empty);
      return;
    }
    for (const p of others) {
      const row = document.createElement('button');
      row.type = 'button';
      row.className = 'peer-row';
      const name = document.createElement('span');
      name.className = 'peer-name';
      name.textContent = p.name;
      const meta = document.createElement('span');
      meta.className = 'peer-meta';
      const bits = [];
      if (p.board) bits.push(p.board);
      if (p.ip) bits.push(p.port && p.port !== 80 ? `${p.ip}:${p.port}` : p.ip);
      if (p.assets === 0) bits.push('API only');
      meta.textContent = bits.join(' · ');
      row.append(name, meta);
      row.addEventListener('click', () => this._openPeer(p.name));
      this._list.appendChild(row);
    }
  }

  _openPeer(name) {
    const q = new URLSearchParams(location.search);
    q.delete('ws');
    q.set('peer', name);
    q.set('embed', '1');
    this._open = name;
    this._current.textContent = name;
    this._back.hidden = false;
    this._list.hidden = true;
    this._frame.hidden = false;
    this._frame.src = `${location.pathname}?${q.toString()}`;
    this._status.textContent = '';
  }

  _close() {
    this._open = '';
    this._frame.src = 'about:blank';
    this._frame.hidden = true;
    this._list.hidden = false;
    this._back.hidden = true;
    this._current.textContent = '';
    this._refresh();
  }
}

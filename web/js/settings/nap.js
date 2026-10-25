// Settings -> Phone network. classic.nap in either direction.
// The section is hidden until sys.info lists the bnep transport. With no
// address, classic.nap returns the link and, once an address exists, the
// mask and gateway. Passing addr makes this board call the phone's NAP.
// Passing listen makes the board discoverable so the phone can call it.
// Both are stored in cfg/nap.json.
import { makeSection, kvRows, button, PolledSection } from './common.js';

export function bdAddrOk(s) {
  return /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(String(s || '').trim());
}

export function forwardIfaces(uplinks, extra) {
  const list = (uplinks || []).filter((u) => u && u.name && u.name !== 'b0');
  for (const u of extra || []) {
    if (!u || !u.name || u.name === 'b0') { continue; }
    if (!list.some((x) => x.name === u.name)) { list.push(u); }
  }
  return list;
}

export function napRows(st) {
  const ip = st.ip && st.ip !== '0.0.0.0' ? st.ip : '';
  const mask = st.mask && st.mask !== '0.0.0.0' ? st.mask : '';
  const gw = st.gw && st.gw !== '0.0.0.0' ? st.gw : '';
  const fail = st.state === 'down' && st.fail ? String(st.fail) : '';
  const link = st.state === 'up'
    ? (ip ? `up · ${ip}` : 'up, waiting for an address')
    : st.state === 'connecting' ? 'connecting…'
    : fail ? `down · last try failed (${fail})` : 'down';
  const side = st.role === 'nap' ? 'phone started it'
    : st.role === 'panu' ? 'board started it' : null;
  return [
    ['Link', link],
    ['Side', side],
    ['Phone', st.addr || null],
    ['Address', ip ? (mask ? `${ip} / ${mask}` : ip) : null],
    ['Gateway', gw || null],
    ['Share internet', st.forward
      ? `on, out ${st.forward_iface || 'w0'}${st.forward_why ? ` · ${st.forward_why}` : ''}`
      : 'off'],
  ];
}

export class NapSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-nap', 2000);
    const d = makeSection(this.id, 'Phone network', `
      <div class="kv-list nap-status"></div>
      <div class="row"><label><input id="nap-listen" type="checkbox" checked> Discoverable</label></div>
      <div class="row">
        <label><input id="nap-fwd" type="checkbox"> Share internet</label>
        <select id="nap-fwd-if"></select>
      </div>
      <form class="row nap-form">
        <label for="nap-addr">Phone address</label>
        <input id="nap-addr" type="text" placeholder="AA:BB:CC:DD:EE:FF" spellcheck="false" autocomplete="off">
        <button type="submit" class="pane-btn">Connect</button>
      </form>
      <div class="row nap-scan"></div>
      <div class="kv-list nap-devs"></div>
      <div class="dim nap-note"></div>
      <div class="dim">Scan finds phones. Connect joins the phone's Bluetooth tethering: this board places the call. Discoverable lets the phone find the board, pair, and start the link. The phone opens this web app at 192.168.44.1 when its own Wi-Fi and mobile data are off. Share internet sends the phone's other traffic out the interface you pick. The WireGuard tunnel is listed there when it is configured, and it stays off the board's default route. Turn the phone's Internet access off and on after changing it, so the phone picks up the new DNS. The address is kept on the device. A link this board placed shows an address here once the phone's DHCP server answers, and joins Uplinks as b0. A failed try stays on the Link line. Connect tries again.</div>`);
    this._d = d;
    d.hidden = true;
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _status: q('.nap-status'), _addr: q('#nap-addr'), _devs: q('.nap-devs'),
      _note: q('.nap-note'), _scan: q('.nap-scan'), _listen: q('#nap-listen'),
      _fwd: q('#nap-fwd'), _fwdIf: q('#nap-fwd-if'),
    });
    this._dirty = false;
    this._addr.addEventListener('input', () => { this._dirty = true; });
    this._listen.addEventListener('change', () => this._setListen());
    this._fwd.addEventListener('change', () => this._share());
    this._fwdIf.addEventListener('change', () => this._share());
    q('.nap-form').addEventListener('submit', (e) => { e.preventDefault(); this._connect(); });
    this._scan.append(button('Scan', () => this._scanNow()));
  }

  _fillIfaces(uplinks, selected, extra) {
    const ifs = forwardIfaces(uplinks, extra);
    const names = ifs.map((u) => u.name);
    if (selected && !names.includes(selected)) { names.unshift(selected); }
    if (!names.length) { names.push('w0'); }
    const cur = this._fwdIf.value;
    const have = [...this._fwdIf.options].map((o) => o.value).join(',');
    if (have !== names.join(',')) {
      this._fwdIf.replaceChildren(...names.map((name) => {
        const o = document.createElement('option');
        const u = ifs.find((x) => x.name === name);
        o.value = name;
        o.textContent = u ? `${u.name} (${u.kind})` : name;
        return o;
      }));
    }
    const want = document.activeElement === this._fwdIf ? cur : (selected || cur || names[0]);
    if ([...this._fwdIf.options].some((o) => o.value === want)) { this._fwdIf.value = want; }
  }

  async _share() {
    const iface = this._fwdIf.value || 'w0';
    const on = this._fwd.checked;
    try {
      await this.ws.call('classic.nap', { forward: on, iface });
      this._setNote(on
        ? `sharing out ${iface}. Turn the phone's Internet access off and on.`
        : 'not sharing');
    } catch (e) {
      this._setNote(e.message);
    }
    this.soon();
  }

  async _setListen() {
    const on = this._listen.checked;
    try {
      await this.ws.call('classic.nap', { listen: on });
      this._setNote(on
        ? 'discoverable: a phone can start the link'
        : 'not discoverable: use Scan and Connect');
    } catch (e) {
      this._setNote(e.message);
    }
    this.soon();
  }

  onConnect() {
    const t = this.app.sysInfo?.transports || [];
    this._d.hidden = !t.includes('bnep');
  }

  async _connect() {
    const addr = this._addr.value.trim();
    if (!bdAddrOk(addr)) {
      this._setNote('address is AA:BB:CC:DD:EE:FF');
      return;
    }
    try {
      await this.ws.call('classic.nap', { addr });
      this._dirty = false;
      this._setStatus(`connecting to ${addr}`);
    } catch (e) {
      this._setNote(e.message);
    }
    this.soon();
  }

  async _scanNow() {
    try {
      await this.ws.call('classic.scan', {});
      this._setNote('scanning…');
    } catch (e) {
      this._setNote(e.message);
    }
    this.soon();
  }

  _use(addr) {
    this._addr.value = addr;
    this._dirty = true;
    this._setNote(addr);
  }

  _setNote(text) {
    this._note.textContent = text;
    delete this._note.dataset.nap;
  }

  _setStatus(text) {
    this._note.textContent = text;
    this._note.dataset.nap = 'status';
  }

  /* The "connecting to …" note is ours. Once the link leaves that state the
   * Link row carries the result, including a failed try, and Connect stays
   * usable because the board is down. */
  _syncNote(st) {
    if (this._note.dataset.nap !== 'status' && this._note.textContent) { return; }
    if (st.state === 'connecting') {
      this._setStatus(st.addr ? `connecting to ${st.addr}` : 'connecting…');
      return;
    }
    if (this._note.dataset.nap === 'status') { this._setNote(''); }
  }

  async poll() {
    const [st, cl, up, wg] = await Promise.all([
      this.ws.call('classic.nap', {}).catch((e) => ({ error: e.message })),
      this.ws.call('classic.state', {}).catch(() => ({})),
      this.ws.call('uplink.status', {}).catch(() => ({})),
      this.ws.call('wg.status', {}).catch(() => ({})),
    ]);
    if (st.error) { kvRows(this._status, [['Link', st.error]]); return; }
    kvRows(this._status, napRows(st));
    this._syncNote(st);
    if (document.activeElement !== this._addr && !this._dirty && st.addr) this._addr.value = st.addr;
    if (document.activeElement !== this._listen && typeof st.listen === 'boolean') {
      this._listen.checked = st.listen;
    }
    const tunnel = wg && wg.configured ? [{ name: 'wg', kind: 'wireguard' }] : [];
    this._fillIfaces(up.uplinks, st.forward_iface, tunnel);
    if (document.activeElement !== this._fwd && typeof st.forward === 'boolean') {
      this._fwd.checked = st.forward;
    }
    const devs = cl.devices || [];
    this._devs.replaceChildren(...devs.flatMap((dev) => {
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = dev.name || dev.address;
      const v = document.createElement('div');
      v.className = 'v';
      v.append(button('Use', () => this._use(dev.address)));
      return [k, v];
    }));
    if (!devs.length) this._devs.textContent = cl.state === 'scanning' ? 'scanning…' : '';
  }
}

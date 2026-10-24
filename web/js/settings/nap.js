// Settings -> Phone network. A PANU link to a phone's NAP (classic.nap).
// The section is hidden until sys.info lists the bnep transport. With no
// address, classic.nap returns the link and, once DHCP has answered, the
// address, mask and gateway. Passing addr starts the connection and the
// firmware stores that address in cfg/nap.json.
import { makeSection, kvRows, button, PolledSection } from './common.js';

export function bdAddrOk(s) {
  return /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/.test(String(s || '').trim());
}

export function napRows(st) {
  const ip = st.ip && st.ip !== '0.0.0.0' ? st.ip : '';
  const mask = st.mask && st.mask !== '0.0.0.0' ? st.mask : '';
  const gw = st.gw && st.gw !== '0.0.0.0' ? st.gw : '';
  const link = st.state === 'up'
    ? (ip ? `up · ${ip}` : 'up, waiting for an address')
    : st.state === 'connecting' ? 'connecting…' : 'down';
  return [
    ['Link', link],
    ['Phone', st.addr || null],
    ['Address', ip ? (mask ? `${ip} / ${mask}` : ip) : null],
    ['Gateway', gw || null],
  ];
}

export class NapSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-nap', 2000);
    const d = makeSection(this.id, 'Phone network', `
      <div class="kv-list nap-status"></div>
      <form class="row nap-form">
        <label for="nap-addr">Phone address</label>
        <input id="nap-addr" type="text" placeholder="AA:BB:CC:DD:EE:FF" spellcheck="false" autocomplete="off">
        <button type="submit" class="pane-btn">Connect</button>
      </form>
      <div class="row nap-scan"></div>
      <div class="kv-list nap-devs"></div>
      <div class="dim nap-note"></div>
      <div class="dim">The phone shares its connection as a Bluetooth network (NAP). The address is kept on the device. The link shows an IP address here once the phone's DHCP server answers, and it joins Uplinks as b0.</div>`);
    this._d = d;
    d.hidden = true;
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _status: q('.nap-status'), _addr: q('#nap-addr'), _devs: q('.nap-devs'),
      _note: q('.nap-note'), _scan: q('.nap-scan'),
    });
    this._dirty = false;
    this._addr.addEventListener('input', () => { this._dirty = true; });
    q('.nap-form').addEventListener('submit', (e) => { e.preventDefault(); this._connect(); });
    this._scan.append(button('Scan', () => this._scanNow()));
  }

  onConnect() {
    const t = this.app.sysInfo?.transports || [];
    this._d.hidden = !t.includes('bnep');
  }

  async _connect() {
    const addr = this._addr.value.trim();
    if (!bdAddrOk(addr)) {
      this._note.textContent = 'address is AA:BB:CC:DD:EE:FF';
      return;
    }
    try {
      await this.ws.call('classic.nap', { addr });
      this._dirty = false;
      this._note.textContent = `connecting to ${addr}`;
    } catch (e) {
      this._note.textContent = e.message;
    }
    this.soon();
  }

  async _scanNow() {
    try {
      await this.ws.call('classic.scan', {});
      this._note.textContent = 'scanning…';
    } catch (e) {
      this._note.textContent = e.message;
    }
    this.soon();
  }

  _use(addr) {
    this._addr.value = addr;
    this._dirty = true;
    this._note.textContent = addr;
  }

  async poll() {
    const [st, cl] = await Promise.all([
      this.ws.call('classic.nap', {}).catch((e) => ({ error: e.message })),
      this.ws.call('classic.state', {}).catch(() => ({})),
    ]);
    if (st.error) { kvRows(this._status, [['Link', st.error]]); return; }
    kvRows(this._status, napRows(st));
    if (document.activeElement !== this._addr && !this._dirty && st.addr) this._addr.value = st.addr;
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

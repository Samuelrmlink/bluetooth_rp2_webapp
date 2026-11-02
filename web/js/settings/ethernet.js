// Settings -> Ethernet: the optional W5500 uplink. Shown only when the
// firmware was built with it (sys.info lists the `eth` module); a build with
// the module but no chip fitted says "not present".
//
// DHCP is the default: the device asks for a lease, and with no DHCP server on
// the segment it takes a link-local 169.254.x.x address beside it (mDNS keeps
// it findable). Manual takes ip + mask, and optionally a gateway and a DNS
// server (stored for later; the firmware has no resolver yet). DHCP server
// makes e0 serve addresses on its segment -- only ever by choosing it here;
// offers carry no router/DNS unless "Advertise as router" is ticked, so a PC
// on the cable keeps its own internet. Apply sends eth.config, which the
// device validates again and applies from its main loop -- so the status
// poll, not the reply, says when it took.
import { makeSection, kvRows, setNote, PolledSection } from './common.js';

export function ipv4ok(s) {
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(String(s ?? '').trim());
  return !!m && m.slice(1).every((o) => Number(o) <= 255);
}

const u32 = (s) => s.trim().split('.').reduce((a, o) => (a * 256) + Number(o), 0);

export function maskOk(s) {
  if (!ipv4ok(s)) return false;
  const v = u32(s);
  if (v === 0) return false;
  const inv = (~v) >>> 0;
  return ((inv + 1) & inv) >>> 0 === 0;          // ones then zeros
}

// Mirrors the firmware's checks, so a bad form is caught before the round
// trip; the device stays the authority.
export function ethFormError(f) {
  if (f.mode === 'dhcp') return null;
  if (f.mode === 'server') {
    if (f.ip && (!ipv4ok(f.ip) || u32(f.ip) === 0)) return 'IP address is not valid';
    if (f.mask && !maskOk(f.mask)) return 'subnet mask is not valid';
    if (f.mask && ((u32(f.mask) & 0xff) !== 0)) return 'a DHCP server needs a /24 or larger subnet';
    const last = Number((f.ip || '192.168.9.1').split('.')[3]);
    if (last === 0 || last === 255 || (last >= 16 && last < 24)) return 'server address must not be .0, .255 or .16-.23 (the lease range)';
    return null;
  }
  if (f.mode !== 'manual') return 'mode is DHCP, Manual or DHCP server';
  if (!ipv4ok(f.ip) || u32(f.ip) === 0) return 'IP address is not valid';
  if (!maskOk(f.mask)) return 'subnet mask is not valid';
  if (f.gw) {
    if (!ipv4ok(f.gw)) return 'gateway is not valid';
    const m = u32(f.mask);
    if (((u32(f.gw) & m) >>> 0) !== ((u32(f.ip) & m) >>> 0)) return 'gateway is not on that subnet';
  }
  if (f.dns && !ipv4ok(f.dns)) return 'DNS server is not valid';
  return null;
}

export function ethRows(s) {
  if (!s.present) return [['State', 'not present (no W5500 answered on SPI)'], ['MAC', null]];
  const dhcp = s.mode === 'dhcp';
  const src = { dhcp: 'DHCP lease', 'link-local': 'link-local (no DHCP server answered; still asking)', manual: 'manual', server: 'static, serving DHCP' }[s.addr_source];
  return [
    ['State', !s.link ? 'no link (cable?)' : s.ip && s.ip !== '0.0.0.0' ? `up · ${s.ip}` : dhcp ? 'link up, waiting for DHCP…' : 'link up'],
    ['Link', s.link ? `${s.speed} Mb/s ${s.duplex} duplex` : 'down'],
    ['Addressing', s.mode === 'server' ? `DHCP server${s.dhcp_router ? ' (advertising itself as router)' : ''}` : dhcp ? 'DHCP client' : 'manual'],
    ['Address from', src || null],
    ['Address', s.ip && s.ip !== '0.0.0.0' ? `${s.ip} / ${s.mask}` : null],
    ['Gateway', s.gw && s.gw !== '0.0.0.0' ? s.gw : null],
    ['DNS', s.dns || null],
    ['MAC', s.mac],
    ['Frames', `${s.rx_frames} in · ${s.tx_frames} out`],
    ['Dropped', `${(s.rx_dropped || 0) + (s.rx_nomem || 0)} in · ${(s.tx_dropped || 0) + (s.txq_full || 0)} out` +
      (s.resyncs ? ` · ${s.resyncs} resync(s)` : '')],
    ['Default route', s.is_default_route ? 'yes (active uplink)' : 'no'],
  ];
}

export class EthernetSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-ethernet', 2000);
    const d = makeSection(this.id, 'Ethernet', `
      <div class="kv-list eth-status"></div>
      <hr>
      <form class="eth-form">
        <div class="row" role="radiogroup" aria-label="Addressing">
          <label><input type="radio" name="eth-mode" value="dhcp" checked> DHCP</label>
          <label><input type="radio" name="eth-mode" value="manual"> Manual</label>
          <label><input type="radio" name="eth-mode" value="server"> DHCP server</label>
        </div>
        <div class="eth-manual" hidden>
          <div class="row"><label for="eth-ip">IP address</label><input id="eth-ip" type="text" inputmode="decimal" placeholder="192.168.50.2" spellcheck="false" autocomplete="off"></div>
          <div class="row"><label for="eth-mask">Subnet mask</label><input id="eth-mask" type="text" inputmode="decimal" placeholder="255.255.255.0" spellcheck="false" autocomplete="off"></div>
          <div class="row eth-only-manual"><label for="eth-gw">Gateway</label><input id="eth-gw" type="text" inputmode="decimal" placeholder="optional" spellcheck="false" autocomplete="off"></div>
          <div class="row eth-only-manual"><label for="eth-dns">DNS server</label><input id="eth-dns" type="text" inputmode="decimal" placeholder="optional" spellcheck="false" autocomplete="off"></div>
          <div class="row eth-only-server"><label><input id="eth-router" type="checkbox"> Advertise as router and DNS</label></div>
          <div class="dim eth-only-server">Hands out .16–.23 on this subnet. Without "router", a computer on the cable keeps its own internet.</div>
        </div>
        <div class="row"><button type="submit" class="pane-btn eth-apply">Apply</button></div>
      </form>
      <div class="dim eth-note"></div>`);
    this._d = d;
    d.hidden = true;                    // until sys.info says the module is there
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _status: q('.eth-status'), _form: q('.eth-form'), _manual: q('.eth-manual'), _note: q('.eth-note'),
      _ip: q('#eth-ip'), _mask: q('#eth-mask'), _gw: q('#eth-gw'), _dns: q('#eth-dns'), _router: q('#eth-router'),
    });
    this._dirty = false;
    for (const r of d.querySelectorAll('input[name="eth-mode"]')) {
      r.addEventListener('change', () => { this._dirty = true; this._showManual(); });
    }
    for (const i of [this._ip, this._mask, this._gw, this._dns, this._router]) i.addEventListener('input', () => { this._dirty = true; });
    this._form.addEventListener('submit', (e) => { e.preventDefault(); this._apply(); });
  }

  _mode() { return this._d.querySelector('input[name="eth-mode"]:checked')?.value || 'dhcp'; }

  _showManual() {
    const m = this._mode();
    this._manual.hidden = m === 'dhcp';
    // style, not the hidden attribute: .row's display wins over [hidden].
    for (const e of this._d.querySelectorAll('.eth-only-manual')) e.style.display = m === 'manual' ? '' : 'none';
    for (const e of this._d.querySelectorAll('.eth-only-server')) e.style.display = m === 'server' ? '' : 'none';
    this._ip.placeholder = m === 'server' ? '192.168.9.1' : '192.168.50.2';
  }

  _formValue() {
    const f = { mode: this._mode(), ip: this._ip.value.trim(), mask: this._mask.value.trim(), gw: this._gw.value.trim(), dns: this._dns.value.trim() };
    if (f.mode === 'server') { f.dhcp_router = this._router.checked; delete f.gw; delete f.dns; }
    return f;
  }

  async _apply() {
    const f = this._formValue();
    const err = ethFormError(f);
    if (err) { setNote(this._note, err, true); return; }
    try {
      await this.ws.call('eth.config', f);
      setNote(this._note, f.mode === 'dhcp' ? 'applied: DHCP' : f.mode === 'server' ? `applied: DHCP server on ${f.ip || '192.168.9.1'}` : `applied: ${f.ip}`);
      this._dirty = false;
    } catch (e) {
      setNote(this._note, `eth.config: ${e.message}`, true);
    }
    this.soon();
  }

  async poll() {
    const s = await this.ws.call('eth.status').catch((e) => ({ error: e.message }));
    if (s.error) { kvRows(this._status, [['State', s.error]]); return; }
    kvRows(this._status, ethRows(s));
    // Fill the form from the device unless the user is mid-edit.
    if (!this._dirty && !this._d.contains(document.activeElement)) {
      for (const r of this._d.querySelectorAll('input[name="eth-mode"]')) r.checked = r.value === s.mode;
      const c = s.cfg || {};
      this._ip.value = c.ip || ''; this._mask.value = c.mask || ''; this._gw.value = c.gw || ''; this._dns.value = c.dns || '';
      this._router.checked = !!s.dhcp_router;
      this._showManual();
    }
  }
}

// Settings -> Uplinks: which interface carries the WireGuard tunnel and new
// connections, how each one is measuring, and the policy that picks.
//
// The device pings the WireGuard endpoint (or the configured probe IP) on
// every interface that has a gateway; an interface with none is shown but
// not measured. "Use now" switches at once -- make-before-break, the old
// path keeps receiving -- and the policy carries on from there.
import { makeSection, kvRows, button, setNote, PolledSection } from './common.js';
import { ipv4ok } from './ethernet.js';

const STATE_TEXT = {
  ok: 'ok', measuring: 'measuring…', failed: 'FAILED (3 probes lost)', no_gateway: 'no gateway (not checked)',
  unprobed: 'up, not measured (no probe target)',
};

export function uplinkLabel(u) {
  let s = `${u.name} (${u.kind})`;
  if (u.ip && u.ip !== '0.0.0.0') {
    s += ` ${u.ip}`;
    if (u.mask && u.mask !== '0.0.0.0') s += ` / ${u.mask}`;
  }
  if (u.gw && u.gw !== '0.0.0.0') s += ` gw ${u.gw}`;
  return s;
}

export function uplinkLine(u) {
  const parts = [STATE_TEXT[u.state] || u.state];
  if (u.rtt_ms >= 0) parts.push(`${u.rtt_ms} ms ±${u.jitter_ms}`);
  if (u.state !== 'no_gateway') parts.push(`${u.loss_pct}% loss`);
  if (u.metric >= 0) parts.push(`metric ${u.metric}`);
  return `${u.active ? '● ' : ''}${parts.join(' · ')}`;
}

export function policyOptions(uplinks) {
  return [
    ['best', 'Best (switch to a clearly better uplink)'],
    ['failover', 'Failover (switch only when the active one fails)'],
    ...uplinks.map((u) => [`fixed:${u.name}`, `Always ${u.name} (${u.kind}) when it has a gateway`]),
  ];
}

export function switchText(s) {
  if (!s.switches) return 'none yet';
  const ago = Math.round((s.last_switch_ms_ago ?? 0) / 1000);
  const ls = s.last_switch || {};
  return `${s.switches} · last ${ls.from || 'none'} → ${ls.to || 'none'} (${ls.reason}) ${ago} s ago`;
}

export class UplinksSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-uplinks', 2000);
    const d = makeSection(this.id, 'Uplinks', `
      <div class="kv-list up-summary"></div>
      <div class="kv-list up-list"></div>
      <div class="row up-switch"></div>
      <hr>
      <div class="row"><label for="up-policy">Policy</label><select id="up-policy"></select></div>
      <form class="row up-probe">
        <label for="up-probe-ip">Probe target</label>
        <input id="up-probe-ip" type="text" inputmode="decimal" placeholder="WireGuard endpoint" spellcheck="false" autocomplete="off">
        <button type="submit" class="pane-btn">Save</button>
      </form>
      <div class="dim up-note"></div>
      <div class="dim">Pings go outside the tunnel, from each interface's own address. Leave the probe
        target empty to use the WireGuard endpoint; set one if that server drops ICMP.</div>`);
    this._d = d;
    d.hidden = true;
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _summary: q('.up-summary'), _list: q('.up-list'), _switch: q('.up-switch'),
      _policy: q('#up-policy'), _probe: q('#up-probe-ip'), _note: q('.up-note'),
    });
    this._names = '';
    this._policy.addEventListener('change', () => this._set({ policy: this._policy.value }, `policy ${this._policy.value}`));
    q('.up-probe').addEventListener('submit', (e) => {
      e.preventDefault();
      const ip = this._probe.value.trim();
      if (ip && !ipv4ok(ip)) { setNote(this._note, 'probe target is not an IPv4 address', true); return; }
      this._set({ probe_ip: ip }, ip ? `probing ${ip}` : 'probing the WireGuard endpoint');
    });
  }

  async _set(params, note) {
    try {
      await this.ws.call('uplink.policy', params);
      setNote(this._note, note);
    } catch (e) {
      setNote(this._note, `uplink.policy: ${e.message}`, true);
    }
    this.soon();
  }

  async _use(name) {
    try {
      await this.ws.call('uplink.switch', { iface: name });
      setNote(this._note, `switched to ${name}`);
    } catch (e) {
      setNote(this._note, `uplink.switch: ${e.message}`, true);
    }
    this.soon();
  }

  async poll() {
    const s = await this.ws.call('uplink.status').catch((e) => ({ error: e.message }));
    if (s.error) { kvRows(this._summary, [['State', s.error]]); return; }
    const ups = s.uplinks || [];
    kvRows(this._summary, [
      ['Active', s.active ? `${s.active}` : 'none (no interface has a gateway)'],
      ['Probe target', s.probe_ip ? `${s.probe_ip} (${s.probe_source})` : 'none — configure WireGuard or a probe target'],
      ['Switches', switchText(s)],
    ]);
    kvRows(this._list, ups.map((u) => [uplinkLabel(u), uplinkLine(u)]));
    // "Use now" buttons and the policy options follow the interfaces this
    // build has; rebuilt only when that set changes so focus is kept.
    const names = ups.map((u) => u.name).join(',');
    if (names !== this._names) {
      this._names = names;
      this._switch.replaceChildren(...ups.map((u) => button(`Use ${u.name} now`, () => this._use(u.name))));
      this._policy.replaceChildren(...policyOptions(ups).map(([v, t]) => {
        const o = document.createElement('option');
        o.value = v; o.textContent = t;
        return o;
      }));
    }
    if (document.activeElement !== this._policy) this._policy.value = s.policy;
    if (document.activeElement !== this._probe) this._probe.value = s.probe_source === 'configured' ? s.probe_ip : '';
  }
}

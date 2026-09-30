// Settings -> WiFi. Every wifi.* op is asynchronous on the device -- nothing
// touches the radio from inside a request -- so a join or a scan is
// "accepted", and this section learns the outcome by polling wifi.status.
// Networks heard are merged across sweeps and forgotten after 30 s unseen,
// because wifi.scan_results only holds the latest sweep.
import { makeSection, kvRows, button, PolledSection } from './common.js';

const AP_TTL_MS = 30000;

function bars(rssi) {
  if (typeof rssi !== 'number') return '';
  return rssi >= -55 ? '▂▄▆█' : rssi >= -65 ? '▂▄▆_' : rssi >= -75 ? '▂▄__' : '▂___';
}

export class WifiSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-wifi', 2000);
    this.seen = new Map();
    this.saved = [];
    const d = makeSection(this.id, 'WiFi', `
      <div class="kv-list w-status"></div>
      <div class="err w-error" hidden></div>
      <hr>
      <div class="row"><label for="w-mode">Mode</label>
        <select id="w-mode">
          <option value="auto">Auto (join known, else hotspot)</option>
          <option value="sta">Station only</option>
          <option value="ap">Hotspot only</option>
          <option value="both">Station and hotspot</option>
          <option value="off">Off</option>
        </select></div>
      <hr>
      <div class="row"><strong>Networks in range</strong><span class="spacer"></span><button type="button" class="pane-btn w-scan">Scan</button></div>
      <div class="kv-list w-aps"></div>
      <form class="row w-join">
        <input class="w-ssid" type="text" placeholder="SSID" spellcheck="false" autocomplete="off">
        <input class="w-psk" type="password" placeholder="password" autocomplete="off">
        <button type="submit" class="pane-btn">Join</button>
      </form>
      <div class="dim w-note"></div>
      <hr>
      <div class="row"><strong>Saved networks</strong></div>
      <div class="kv-list w-saved"></div>
      <hr>
      <div class="row"><strong>Fallback hotspot</strong></div>
      <form class="row w-ap">
        <input class="w-ap-ssid" type="text" placeholder="hotspot SSID" spellcheck="false" autocomplete="off">
        <input class="w-ap-psk" type="text" placeholder="hotspot password" spellcheck="false" autocomplete="off">
        <button type="submit" class="pane-btn">Save</button>
      </form>`);
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _status: q('.w-status'), _error: q('.w-error'), _mode: q('#w-mode'), _aps: q('.w-aps'),
      _saved: q('.w-saved'), _note: q('.w-note'), _ssid: q('.w-ssid'), _psk: q('.w-psk'),
      _apSsid: q('.w-ap-ssid'), _apPsk: q('.w-ap-psk'),
    });
    this._mode.addEventListener('change', () => this._call('wifi.mode', { mode: this._mode.value }, `mode ${this._mode.value}`));
    q('.w-scan').addEventListener('click', () => this._call('wifi.scan', {}, 'scanning…'));
    q('.w-join').addEventListener('submit', (e) => { e.preventDefault(); this._join(this._ssid.value.trim(), this._psk.value); });
    q('.w-ap').addEventListener('submit', (e) => {
      e.preventDefault();
      this._call('wifi.ap', { ssid: this._apSsid.value, psk: this._apPsk.value }, 'hotspot saved');
    });
  }

  async _call(op, params, note) {
    try {
      await this.ws.call(op, params);
      this._note.textContent = note;
    } catch (e) {
      this._note.textContent = `${op}: ${e.message}`;
    }
    this.soon();
  }

  _join(ssid, psk) {
    if (!ssid) { this._note.textContent = 'enter an SSID'; return; }
    // "Joining", not "joined": the device answers before the radio has done
    // anything; the status poll says how it went.
    this._call('wifi.connect', psk ? { ssid, psk } : { ssid }, `asked to join ${ssid}…`);
  }

  async poll() {
    const [st, res, nets] = await Promise.all([
      this.ws.call('wifi.status'),
      this.ws.call('wifi.scan_results').catch(() => ({ aps: [] })),
      this.ws.call('wifi.networks').catch(() => ({ networks: [] })),
    ]);
    const up = st.mode === 'sta' && st.ip && st.ip !== '0.0.0.0';
    kvRows(this._status, [
      ['State', up ? `joined ${st.ssid}` : (st.ap ? 'hotspot only' : st.mode || 'idle')],
      ['Address', up ? st.ip : null],
      ['Mode', `${st.mode}${st.want && st.want !== st.mode ? ` (want ${st.want})` : ''}`],
      ['Radio', st.driver ? 'up' : 'down'],
      ['Hotspot', st.ap ? `up · ${st.ap_ip} · ${st.ap_clients ?? 0} client(s)` : 'down'],
    ]);
    this._error.hidden = !st.error;
    this._error.textContent = st.error ? `last error: ${st.error}` : '';
    if (document.activeElement !== this._mode && st.want) this._mode.value = st.want;
    if (document.activeElement !== this._apSsid) this._apSsid.value = st.ap_ssid || '';
    if (document.activeElement !== this._apPsk) this._apPsk.value = st.ap_psk || '';

    const now = Date.now();
    for (const ap of res.aps || []) this.seen.set(ap.ssid, { ...ap, seen: now });
    for (const [k, ap] of this.seen) if (now - ap.seen > AP_TTL_MS) this.seen.delete(k);
    this.saved = nets.networks || [];
    // The device says whether it knows a network (`known`); a name match
    // against the saved list is only the fallback for a reply without it.
    const isSaved = (ssid) => this.seen.get(ssid)?.known ?? this.saved.some((n) => n.ssid === ssid);

    const aps = [...this.seen.values()].sort((a, b) => (b.rssi ?? -99) - (a.rssi ?? -99));
    this._aps.replaceChildren(...aps.flatMap((ap) => {
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = `${bars(ap.rssi)} ${ap.ssid}`;
      const v = document.createElement('div');
      v.className = 'v';
      // scan_results names them chan/auth; print only what the reply has.
      const meta = [`${ap.rssi} dBm`, ap.chan != null ? `ch ${ap.chan}` : null, ap.auth && ap.auth !== 'unknown' ? ap.auth : null,
        isSaved(ap.ssid) ? 'saved' : null].filter(Boolean).join(' · ');
      v.append(`${meta} `,
        button(isSaved(ap.ssid) ? 'Join' : 'Use', () => {
          this._ssid.value = ap.ssid;
          if (isSaved(ap.ssid)) this._join(ap.ssid);
          else this._psk.focus();
        }));
      return [k, v];
    }));
    if (!aps.length) this._aps.textContent = st.scanning ? 'scanning…' : 'nothing heard yet — press Scan';

    this._saved.replaceChildren(...this.saved.flatMap((n) => {
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = n.ssid;
      const v = document.createElement('div');
      v.className = 'v';
      v.append(button('Join', () => this._join(n.ssid)), ' ',
        button('Forget', () => this._call('wifi.forget', { ssid: n.ssid }, `forgot ${n.ssid}`)));
      return [k, v];
    }));
    if (!this.saved.length) this._saved.textContent = 'no stored networks';
  }
}

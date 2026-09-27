/*
 * The WiFi pane: scan, join, forget, and pick what the radio does.
 *
 * Two things here are shaped by how the firmware actually behaves rather than
 * by what would be tidy:
 *
 * 1. Scan results ACCUMULATE. The radio rescans continuously while idle and
 *    wifi.scan_results returns only the latest sweep, so a network that was
 *    visible two seconds ago vanishes and comes back. A list that flickered
 *    like that would be unusable, so results are merged by SSID and aged out
 *    on a timer instead of being replaced wholesale.
 *
 * 2. Nothing here is synchronous. wifi.h's rule is that no entry point
 *    touches the radio from the caller's context -- every one sets a flag the
 *    main loop drains -- so "Join" means "asked to join" and the pane learns
 *    what happened by polling wifi.status, exactly as the device does.
 */

const $ = (id) => document.getElementById(id);

const AP_TTL_MS = 30000;   /* drop a network unseen for this long */
const POLL_MS = 2000;

export function mountWifiPane({ api, log }) {
  const el = {
    state: $('w-state'), ident: $('w-ident'), error: $('w-error'),
    grid: $('w-grid'), aps: $('w-aps'), saved: $('w-saved'),
    scanning: $('w-scanning'), mode: $('w-mode'), scan: $('w-scan'),
    ssid: $('w-ssid'), psk: $('w-psk'), join: $('w-join'), joinNote: $('w-join-note'),
    apSsid: $('w-ap-ssid'), apPsk: $('w-ap-psk'), apSave: $('w-ap-save'),
    apNote: $('w-ap-note'),
  };

  /* ssid -> { ssid, rssi, channel, security, seen } */
  const seen = new Map();
  let status = null;
  let saved = [];
  let modeDirty = false;      /* don't fight the user mid-selection */
  let apDirty = false;

  function kv(parent, label, value) {
    const row = document.createElement('div');
    row.className = 'kv';
    const k = document.createElement('div'); k.className = 'k'; k.textContent = label;
    const v = document.createElement('div'); v.className = 'v'; v.textContent = value;
    row.append(k, v);
    parent.appendChild(row);
  }

  function button(label, onClick, cls = 'btn') {
    const b = document.createElement('button');
    b.className = cls;
    b.textContent = label;
    b.addEventListener('click', onClick);
    return b;
  }

  /* -1 dBm steps are meaningless to a person; bars are not. */
  function bars(rssi) {
    if (typeof rssi !== 'number') return '';
    if (rssi >= -55) return '▂▄▆█';
    if (rssi >= -65) return '▂▄▆_';
    if (rssi >= -75) return '▂▄__';
    return '▂___';
  }

  function isSaved(ssid) { return saved.some((n) => n.ssid === ssid); }

  function renderStatus() {
    if (!status) {
      el.state.textContent = 'unknown';
      el.state.dataset.state = 'none';
      el.ident.textContent = 'no answer from wifi.status';
      el.grid.replaceChildren();
      return;
    }
    // `mode` is the STATION's state; the SoftAP is reported separately as
    // `ap`, because in auto and both the two run at once. A pill that only
    // knew "sta or ap" would have to lie about one of them.
    const up = status.mode === 'sta' && status.ip && status.ip !== '0.0.0.0';
    const ap = status.ap === true;
    el.state.textContent = up ? (ap ? 'connected + ap' : 'connected')
                              : (ap ? 'ap' : (status.mode || 'idle'));
    el.state.dataset.state = up ? 'good' : (ap ? 'warn' : 'none');
    el.ident.textContent = [status.ssid, up ? status.ip : null, ap ? `AP ${status.ap_ip}` : null]
      .filter(Boolean).join(' · ') || 'not joined';

    el.error.hidden = !status.error;
    el.error.textContent = status.error ? `last error: ${status.error}` : '';

    const g = document.createElement('div');
    kv(g, 'mode', `${status.mode}${status.want && status.want !== status.mode
      ? ` (want ${status.want})` : ''}`);
    kv(g, 'ssid', status.ssid || '—');
    kv(g, 'ip', status.ip || '—');
    kv(g, 'radio', status.driver ? 'up' : 'down');
    kv(g, 'ap', ap ? `up · ${status.ap_ip}` : 'down');
    kv(g, 'ap ssid', status.ap_ssid || '—');
    if (ap) kv(g, 'ap clients', String(status.ap_clients ?? 0));
    el.grid.replaceChildren(g);

    if (!modeDirty && status.want) el.mode.value = status.want;
    if (!apDirty) {
      if (document.activeElement !== el.apSsid) el.apSsid.value = status.ap_ssid || '';
      if (document.activeElement !== el.apPsk) el.apPsk.value = status.ap_psk || '';
    }
    el.scanning.textContent = status.scanning ? '· scanning…' : '';
  }

  function renderAps() {
    const now = Date.now();
    for (const [ssid, ap] of seen) {
      if (now - ap.seen > AP_TTL_MS) seen.delete(ssid);
    }
    const list = [...seen.values()].sort((a, b) => (b.rssi ?? -99) - (a.rssi ?? -99));
    if (!list.length) {
      const p = document.createElement('div');
      p.className = 'muted';
      p.textContent = 'nothing seen yet — the radio scans on its own, or press Scan';
      el.aps.replaceChildren(p);
      return;
    }
    el.aps.replaceChildren(...list.map((ap) => {
      const row = document.createElement('div');
      row.className = 'kv';
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = `${bars(ap.rssi)} ${ap.ssid}`;
      const v = document.createElement('div');
      v.className = 'v';
      const meta = document.createElement('span');
      meta.className = 'muted';
      meta.textContent = [
        typeof ap.rssi === 'number' ? `${ap.rssi} dBm` : null,
        ap.channel ? `ch ${ap.channel}` : null,
        ap.security || null,
        isSaved(ap.ssid) ? 'saved' : null,
      ].filter(Boolean).join(' · ');
      v.append(meta, ' ', button(isSaved(ap.ssid) ? 'Join' : 'Use', () => {
        el.ssid.value = ap.ssid;
        /* A saved network rejoins with no password: the device still has it. */
        if (isSaved(ap.ssid)) { el.psk.value = ''; doJoin(ap.ssid, undefined); }
        else { el.psk.focus(); }
      }));
      row.append(k, v);
      return row;
    }));
  }

  function renderSaved() {
    if (!saved.length) {
      const p = document.createElement('div');
      p.className = 'muted';
      p.textContent = 'no stored networks';
      el.saved.replaceChildren(p);
      return;
    }
    el.saved.replaceChildren(...saved.map((n) => {
      const row = document.createElement('div');
      row.className = 'kv';
      const k = document.createElement('div'); k.className = 'k'; k.textContent = n.ssid;
      const v = document.createElement('div'); v.className = 'v';
      v.append(
        button('Join', () => doJoin(n.ssid, undefined)),
        ' ',
        button('Forget', async () => {
          try {
            await api.call('wifi.forget', { ssid: n.ssid });
            log('wifi', `forgot ${n.ssid}`);
            await refreshSaved();
          } catch (e) { log('error', `forget: ${e.message}`); }
        }),
      );
      row.append(k, v);
      return row;
    }));
  }

  async function doJoin(ssid, psk) {
    if (!ssid) { el.joinNote.textContent = 'enter an ssid'; return; }
    el.joinNote.textContent = `asking to join ${ssid}…`;
    try {
      const params = psk === undefined || psk === '' ? { ssid } : { ssid, psk };
      await api.call('wifi.connect', params);
      /* "joining", not "joined": the radio is driven from the main loop and
         the outcome shows up in wifi.status a few seconds later. */
      el.joinNote.textContent = `joining ${ssid}… watch the status above`;
      el.psk.value = '';
      await refreshSaved();
    } catch (e) {
      el.joinNote.textContent = `join refused: ${e.message}`;
    }
  }

  async function refreshSaved() {
    try {
      const r = await api.call('wifi.networks');
      saved = r.networks || [];
      renderSaved();
      renderAps();
    } catch (e) { /* the module may be absent on a minimal build */ }
  }

  async function poll() {
    try {
      status = await api.call('wifi.status');
      renderStatus();
    } catch (e) { status = null; renderStatus(); return; }
    try {
      const r = await api.call('wifi.scan_results');
      for (const ap of r.aps || []) {
        if (!ap.ssid) continue;      /* hidden networks have nothing to show */
        seen.set(ap.ssid, { ...ap, seen: Date.now() });
      }
      renderAps();
    } catch (e) { /* scanning is optional; status already rendered */ }
  }

  el.scan.addEventListener('click', async () => {
    try { await api.call('wifi.scan'); log('wifi', 'scan requested'); }
    catch (e) { log('error', `scan: ${e.message}`); }
  });

  el.mode.addEventListener('change', async () => {
    modeDirty = true;
    try {
      await api.call('wifi.mode', { mode: el.mode.value });
      log('wifi', `mode ${el.mode.value}`);
    } catch (e) { log('error', `mode: ${e.message}`); }
    modeDirty = false;
  });

  el.join.addEventListener('click', () => doJoin(el.ssid.value.trim(), el.psk.value));
  el.psk.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') doJoin(el.ssid.value.trim(), el.psk.value);
  });

  for (const i of [el.apSsid, el.apPsk]) {
    i.addEventListener('input', () => { apDirty = true; });
  }
  el.apSave.addEventListener('click', async () => {
    try {
      await api.call('wifi.ap', { ssid: el.apSsid.value.trim(), psk: el.apPsk.value });
      el.apNote.textContent = 'saved';
      apDirty = false;
    } catch (e) { el.apNote.textContent = `refused: ${e.message}`; }
  });

  api.on('_open', async () => { await refreshSaved(); await poll(); });
  setInterval(poll, POLL_MS);

  renderStatus();
  renderAps();
  renderSaved();
}

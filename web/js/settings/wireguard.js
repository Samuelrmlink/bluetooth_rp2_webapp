// Settings -> WireGuard: whether the tunnel is up and proven, and a way to
// bounce it. The configuration itself is cfg/wireguard.json, which opens in
// the Editor like any other file -- keys included, since this device serves
// cfg/ deliberately (it is git-ignored on the development side instead).
import { makeSection, kvRows, button, PolledSection } from './common.js';

export class WireguardSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-wireguard', 3000);
    const d = makeSection(this.id, 'WireGuard', `
      <div class="kv-list wg-status"></div>
      <div class="row wg-actions"></div>
      <div class="dim">"Connected" means a session key exists, which outlives a dead server by
        minutes; "confirmed" means the server answered inside the last keepalive window.</div>`);
    this._status = d.querySelector('.wg-status');
    d.querySelector('.wg-actions').append(
      button('Reconnect', async () => { await this.ws.call('wg.up').catch((e) => this.app.log('!', e.message)); this.soon(); }),
      button('Disconnect', async () => { await this.ws.call('wg.down').catch((e) => this.app.log('!', e.message)); this.soon(); }),
      button('Edit config', () => app.openFile?.('cfg/wireguard.json')),
    );
  }

  async poll() {
    const s = await this.ws.call('wg.status').catch((e) => ({ error: e.message }));
    if (s.error) { kvRows(this._status, [['State', s.error]]); return; }
    kvRows(this._status, [
      ['State', !s.configured ? 'not configured' : s.confirmed ? 'confirmed' : s.connected ? 'connected' : 'connecting…'],
      ['Address', s.address],
      ['Endpoint', s.endpoint ? `${s.current_endpoint || s.endpoint}:${s.current_port || s.endpoint_port}` : null],
      ['Last handshake', s.handshakes ? `${Math.round(s.last_handshake_age_ms / 1000)} s ago` : 'none yet'],
      ['Traffic', `${s.rx_bytes} B in · ${s.tx_bytes} B out`],
      ['Boot', s.boot_count ? `#${s.boot_count}` : null],
      ['Default route', s.is_default_route ? 'YES — this is a bug' : 'no (as it should be)'],
    ]);
  }
}

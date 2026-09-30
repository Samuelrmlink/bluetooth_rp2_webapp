// Settings -> Firmware: what is running, from which slot, and the other
// slot's version. Read-only -- updates go over MCUmgr/SMP (see OTA.md),
// not through this page.
import { makeSection, kvRows, PolledSection } from './common.js';

export class FirmwareSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-firmware', 5000);
    const d = makeSection(this.id, 'Firmware', `
      <div class="kv-list fw-status"></div>
      <div class="dim">Updates are uploaded over MCUmgr/SMP (UDP 1337) with smpmgr; see OTA.md.</div>`);
    this._status = d.querySelector('.fw-status');
  }

  async poll() {
    const [o, s] = await Promise.all([
      this.ws.call('ota.status').catch(() => null),
      this.ws.call('sys.info').catch(() => null),
    ]);
    const up = s?.uptime_ms != null ? Math.round(s.uptime_ms / 1000) : null;
    kvRows(this._status, [
      ['Version', o ? `${o.version} (slot ${o.active})` : s?.firmware],
      ['Other slot', o?.other_version],
      ['Update', o?.buy_pending ? 'running on trial — not yet confirmed' : o ? 'confirmed' : null],
      ['Uptime', up != null ? `${Math.floor(up / 3600)} h ${Math.floor((up % 3600) / 60)} min` : null],
      ['Clients', s?.clients],
    ]);
  }
}

// Settings -> Bluetooth: the radio, the bonding policy, and stored bonds.
import { makeSection, kvRows, button, PolledSection } from './common.js';

export class BluetoothSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-bluetooth', 4000);
    const d = makeSection(this.id, 'Bluetooth', `
      <div class="kv-list bt-status"></div>
      <div class="row"><label for="bt-policy">Bonding</label>
        <select id="bt-policy">
          <option value="never">Never</option>
          <option value="on_demand">When a device asks</option>
          <option value="always">Always</option>
        </select></div>
      <div class="row bt-mtu-row"><label for="bt-mtu">Preferred MTU</label>
        <input id="bt-mtu" type="number" min="23" step="1" inputmode="numeric">
        <button type="button" class="pane-btn bt-mtu-save">Save</button></div>
      <div class="dim bt-mtu-note"></div>
      <div class="row"><strong>Bonded devices</strong></div>
      <div class="kv-list bt-bonds"></div>`);
    this._status = d.querySelector('.bt-status');
    this._bonds = d.querySelector('.bt-bonds');
    this._policy = d.querySelector('#bt-policy');
    // The most a connection asks for; the peer's answer decides what each
    // link actually gets (bt.mtu, fw 1.84+).
    this._mtu = d.querySelector('#bt-mtu');
    this._mtuNote = d.querySelector('.bt-mtu-note');
    d.querySelector('.bt-mtu-save').addEventListener('click', () => this._saveMtu());
    this._mtu.addEventListener('keydown', (e) => { if (e.key === 'Enter') this._saveMtu(); });
    this._policy.addEventListener('change', async () => {
      await this.ws.call('bond.policy', { policy: this._policy.value }).catch((e) => this.app.log('!', e.message));
      this.soon();
    });
  }

  async _saveMtu() {
    const v = Number(this._mtu.value);
    try {
      const r = await this.ws.call('bt.mtu', { max_mtu: v });
      this._mtuSaid = r.reboot_needed
        ? `Saved ${v}. Raising it back to the maximum takes a reboot of the gateway.`
        : `Saved: new connections ask for ${r.max_mtu}; each link gets what its device agrees to. Reconnect a device to use it now.`;
    } catch (e) {
      this._mtuSaid = e.message;
    }
    this._mtuNote.textContent = this._mtuSaid;
    this.soon();
  }

  async poll() {
    const [st, bonds, mtu] = await Promise.all([
      this.ws.call('bt.state').catch(() => ({})),
      this.ws.call('bond.list').catch(() => ({ bonds: [] })),
      this.ws.call('bt.mtu').catch(() => null),
    ]);
    if (mtu) {
      this._mtu.max = String(mtu.ceiling);
      if (document.activeElement !== this._mtu) this._mtu.value = String(mtu.max_mtu);
      const links = (mtu.links || []).map((l) => `${l.addr.slice(-5)}: ${l.mtu}`).join(', ');
      this._mtuNote.textContent = this._mtuSaid
        || `23–${mtu.ceiling} on this firmware. Each connection negotiates up to this${links ? ` — now ${links}` : ''}.`;
    } else {
      this._mtu.disabled = true;
      this._mtuNote.textContent = 'This firmware cannot set the MTU (1.84 or later can).';
    }
    kvRows(this._status, [
      ['Radio', st.ready ? 'ready' : 'not ready'],
      ['Address', st.address],
      ['Links', st.max_links != null ? `up to ${st.max_links}` : null],
    ]);
    if (document.activeElement !== this._policy && bonds.policy) this._policy.value = bonds.policy;
    const list = bonds.bonds || [];
    this._bonds.replaceChildren(...list.flatMap((b) => {
      const addr = b.addr || b.device || String(b);
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = addr;
      const v = document.createElement('div');
      v.className = 'v';
      v.append(button('Remove', async () => {
        await this.ws.call('bond.remove', { device: addr }).catch((e) => this.app.log('!', e.message));
        this.soon();
      }));
      return [k, v];
    }));
    if (!list.length) this._bonds.textContent = 'none';
  }
}

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
      <div class="row"><strong>Bonded devices</strong></div>
      <div class="kv-list bt-bonds"></div>`);
    this._status = d.querySelector('.bt-status');
    this._bonds = d.querySelector('.bt-bonds');
    this._policy = d.querySelector('#bt-policy');
    this._policy.addEventListener('change', async () => {
      await this.ws.call('bond.policy', { policy: this._policy.value }).catch((e) => this.app.log('!', e.message));
      this.soon();
    });
  }

  async poll() {
    const [st, bonds] = await Promise.all([
      this.ws.call('bt.state').catch(() => ({})),
      this.ws.call('bond.list').catch(() => ({ bonds: [] })),
    ]);
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

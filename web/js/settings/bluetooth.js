// Settings -> Bluetooth: the radio, the bonding policy, how this gateway
// pairs (its SMP IO capability and MITM request), and stored bonds.
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
      <div class="row"><label for="bt-io">Pairing method</label>
        <select id="bt-io">
          <option value="no_input_no_output">No input, no output (Just Works)</option>
          <option value="display_only">Display only</option>
          <option value="display_yes_no">Display, yes/no</option>
          <option value="keyboard_only">Keyboard only</option>
          <option value="keyboard_display">Keyboard and display</option>
        </select></div>
      <div class="row"><label for="bt-mitm">Ask for MITM protection</label>
        <input id="bt-mitm" type="checkbox"></div>
      <div class="dim bt-io-note"></div>
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
    // What the gateway declares when pairing (bond.io, fw 1.89+). The web UI
    // is its display and keyboard: passkeys and comparisons come up as prompts.
    this._io = d.querySelector('#bt-io');
    this._mitm = d.querySelector('#bt-mitm');
    this._ioNote = d.querySelector('.bt-io-note');
    const saveIo = async () => {
      try {
        await this.ws.call('bond.io', { io_capability: this._io.value, mitm: this._mitm.checked });
      } catch (e) { this.app.log('!', e.message); }
      this.soon();
    };
    this._io.addEventListener('change', saveIo);
    this._mitm.addEventListener('change', saveIo);
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
    const [st, bonds, mtu, io] = await Promise.all([
      this.ws.call('bt.state').catch(() => ({})),
      this.ws.call('bond.list').catch(() => ({ bonds: [] })),
      this.ws.call('bt.mtu').catch(() => null),
      this.ws.call('bond.io').catch(() => null),
    ]);
    if (io) {
      if (document.activeElement !== this._io) this._io.value = io.io_capability;
      if (document.activeElement !== this._mitm) this._mitm.checked = !!io.mitm;
      this._ioNote.textContent = ioNote(io.io_capability, io.mitm);
    } else {
      this._io.disabled = this._mitm.disabled = true;
      this._ioNote.textContent = 'This firmware pairs as No input, no output only (1.89 or later can choose).';
    }
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

// What a choice means in practice: which questions can come up, and where.
export function ioNote(io, mitm) {
  if (io === 'no_input_no_output' || !io) {
    return 'Every pairing is Just Works: no passkey, no comparison. Enough for most devices; no protection against an eavesdropper during pairing.';
  }
  const can = {
    display_only: 'show a passkey for you to type into the device',
    display_yes_no: 'show a number to compare with the device and accept or reject it',
    keyboard_only: 'ask you for the passkey the device shows',
    keyboard_display: 'ask for, show, or compare a passkey, whichever the device supports',
  }[io];
  return `${mitm ? 'MITM protection is requested, so pairing' : 'Pairing'} can ${can} -- here, as a prompt, or from a Lua script (bt.on_pairing, profiles/pairing.lua).`
    + (mitm ? '' : ' Without MITM it only happens if the device asks for it; otherwise it stays Just Works.');
}

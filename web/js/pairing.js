// Pairing prompts: the web UI as the gateway's display and keyboard.
//
// The firmware asks whenever pairing needs a person (Settings -> Bluetooth
// decides how often that is): security.passkey_request (type the passkey the
// device shows), security.numeric_comparison (do the two numbers match?),
// security.passkey_display (type this number into the device). Every open
// browser shows the question; whoever answers first -- here, another browser,
// or a Lua script (bt.on_pairing, profiles/pairing.lua) -- closes it
// everywhere through security.answered, and security.bonded / .failed end it.

import { Dialog } from './modals/dialog.js';
import { toast } from './toast.js';

export const six = (n) => String(n ?? '').padStart(6, '0');

// What the prompt says and offers for one question. Pure, tested.
export function promptFor(topic, ev) {
  const who = ev?.addr || 'a device';
  if (topic === 'security.passkey_request') {
    return { kind: 'input', title: 'Passkey', text: `${who} asks for its passkey. Type the 6 digits it shows.` };
  }
  if (topic === 'security.numeric_comparison') {
    return { kind: 'compare', title: 'Confirm pairing', number: six(ev.number),
             text: `Does ${who} show this number?` };
  }
  if (topic === 'security.passkey_display') {
    return { kind: 'display', title: 'Passkey', number: six(ev.number),
             text: `Type this number into ${who}.` };
  }
  return null;
}

export class PairingPrompt {
  constructor(app) {
    this.app = app;
    this.addr = null;
    const body = document.createElement('div');
    body.className = 'pair-body';
    body.innerHTML = `
      <p class="pair-text"></p>
      <div class="pair-number" hidden></div>
      <form class="pair-input" novalidate hidden>
        <input class="pair-pin" inputmode="numeric" pattern="[0-9]{1,6}" maxlength="6" autocomplete="one-time-code" placeholder="000000">
        <button type="submit" class="pane-btn pair-ok">Pair</button>
      </form>
      <div class="pair-actions">
        <button type="button" class="pane-btn pair-yes" hidden>Match</button>
        <button type="button" class="pane-btn pair-no">Cancel</button>
      </div>
      <div class="pair-note dim"></div>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'pairing-dlg', title: 'Pairing', body }));
    this.q = (s) => body.querySelector(s);
    this.q('.pair-input').addEventListener('submit', (e) => { e.preventDefault(); this.answer('passkey'); });
    this.q('.pair-yes').addEventListener('click', () => this.answer('yes'));
    this.q('.pair-no').addEventListener('click', () => this.answer('no'));
    // Closing it without answering refuses, rather than leaving the device waiting.
    this.dlg.onClose = () => { if (this.addr && !this.closing) this.answer('no'); };

    const bus = app.bus.owner();
    for (const t of ['security.passkey_request', 'security.numeric_comparison', 'security.passkey_display']) {
      bus.on(t, (ev) => this.ask(t, ev));
    }
    bus.on('security.answered', (ev) => {
      if (this.addr && ev?.addr === this.addr && ev.by !== 'here') this.done(ev.by === 'script' ? 'Answered by a script.' : null);
    });
    bus.on('security.passkey_cancel', (ev) => { if (ev?.addr === this.addr) this.done(); });
    bus.on('security.bonded', (ev) => {
      if (ev?.addr === this.addr) this.done();
      toast(`Paired with ${ev?.addr || 'device'}`);
    });
    bus.on('security.failed', (ev) => {
      if (ev?.addr === this.addr) this.done();
      toast(`Pairing with ${ev?.addr || 'device'} failed (status ${ev?.status}, reason ${ev?.reason})`);
    });
  }

  ask(topic, ev) {
    const p = promptFor(topic, ev);
    if (!p) return;
    this.addr = ev.addr;
    this.kind = p.kind;
    this.dlg._overlay.querySelector('.dlg-title').textContent = p.title;
    this.q('.pair-text').textContent = p.text;
    const num = this.q('.pair-number');
    num.hidden = !p.number;
    num.textContent = p.number || '';
    this.q('.pair-input').hidden = p.kind !== 'input';
    this.q('.pair-yes').hidden = p.kind !== 'compare';
    this.q('.pair-no').textContent = p.kind === 'compare' ? 'Reject' : p.kind === 'display' ? 'Cancel pairing' : 'Cancel';
    this.q('.pair-note').textContent = '';
    this.q('.pair-pin').value = '';
    this.dlg.open();
    // Focus inside it, so its keys (Enter, Esc) are its own.
    setTimeout(() => (p.kind === 'input' ? this.q('.pair-pin').focus() : this.dlg.focusPanel()), 0);
  }

  async answer(how) {
    const device = this.addr;
    if (!device) return;
    try {
      if (how === 'passkey') {
        const v = this.q('.pair-pin').value.trim();
        if (!/^\d{1,6}$/.test(v)) { this.q('.pair-note').textContent = 'Six digits, please.'; return; }
        await this.app.ws.call('bond.passkey', { device, passkey: Number(v) });
      } else if (how === 'yes') {
        await this.app.ws.call('bond.confirm', { device, accept: true });
      } else if (this.kind === 'compare') {
        await this.app.ws.call('bond.confirm', { device, accept: false });
      } else {
        await this.app.ws.call('bond.passkey', { device, cancel: true });
      }
      this.done();
    } catch (e) {
      this.q('.pair-note').textContent = e.message;
    }
  }

  done(note) {
    this.addr = null;
    if (note) toast(note);
    this.closing = true;
    this.dlg.close();
    this.closing = false;
  }
}

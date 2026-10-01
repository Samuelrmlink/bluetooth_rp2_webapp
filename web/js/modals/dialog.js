// A dialog shaped like Settings: a floating window you drag by its header on
// a wide screen, a bottom sheet you drag or tap away on a phone. Taken out of
// modals/settings.js so the Scanner's Filter and Columns dialogs are the same
// thing rather than a copy of it; SettingsModal extends this.
//
//   new Dialog({ id, title, body })      builds its own markup
//   new Dialog({ overlay })               adopts markup already in the page
//
// The app treats an open dialog with focus as the 'settings' key context:
// Alt+Shift+HJKL nudges it, Alt+-/= resizes it, Esc closes it.

import { makeDraggable } from '../draggable.js';
import { makeSheet } from '../sheet.js';

// Must match the max-width in the dialog mobile rules in shell.css.
const MOBILE_MAX = 767;

const clamp = (v, lo, hi) => Math.max(lo, Math.min(v, hi));

let openSeq = 0;

export class Dialog {
  constructor({ overlay, id, title, body, closeBtn } = {}) {
    if (!overlay) {
      overlay = document.createElement('div');
      overlay.id = id;
      overlay.className = 'modal-overlay app-dialog';
      overlay.hidden = true;
      overlay.innerHTML = `
        <div class="modal" role="dialog" tabindex="-1">
          <div class="sheet-grab" aria-hidden="true"></div>
          <div class="modal-header"><span class="dlg-title"></span>
            <button class="modal-close" aria-label="Close">&times;</button></div>
          <div class="modal-body"></div>
        </div>`;
      overlay.querySelector('.dlg-title').textContent = title || '';
      overlay.querySelector('.modal').setAttribute('aria-label', title || '');
      if (body) overlay.querySelector('.modal-body').appendChild(body);
      document.body.appendChild(overlay);
      closeBtn = overlay.querySelector('.modal-close');
    }
    this._overlay = overlay;
    this._panel = overlay.querySelector('.modal');
    const header = overlay.querySelector('.modal-header');
    const grab = overlay.querySelector('.sheet-grab');

    this._mq = window.matchMedia(`(max-width: ${MOBILE_MAX}px)`);
    const phone = () => this._mq.matches;
    this._phone = phone;
    // Where focus came from, so closing hands it back.
    this._restoreTo = null;
    this.openedAt = 0;

    // Two behaviours on one panel, each gated on the viewport: a free-floating
    // window on a desktop, a draggable bottom sheet on a phone.
    makeDraggable(this._panel, header, { enabled: () => !phone() });
    this._sheet = makeSheet(this._panel, { handles: [grab, header], enabled: phone, onDismiss: () => this.close() });
    // Crossing the breakpoint must not leave a sheet's transform on the window.
    this._mq.addEventListener?.('change', () => this._sheet.reset());

    closeBtn?.addEventListener('click', () => this.close());
    // Tap the dimmed backdrop to dismiss -- a phone only; on a desktop the
    // overlay is pointer-events:none and never sees this.
    overlay.addEventListener('click', (e) => { if (e.target === overlay) this.close(); });
  }

  get isOpen() { return !this._overlay.hidden; }
  get panel() { return this._panel; }
  get body() { return this._overlay.querySelector('.modal-body'); }
  get hasFocus() { return this.isOpen && this._panel.contains(document.activeElement); }

  open() {
    this._sheet.reset();
    if (!this.isOpen) {
      const ae = document.activeElement;
      this._restoreTo = ae && ae !== document.body && !this._panel.contains(ae) ? ae : null;
    }
    this._overlay.hidden = false;
    this.openedAt = ++openSeq;
    if (!this._panel.contains(document.activeElement)) this._panel.focus({ preventScroll: true });
    this.onOpen?.();
  }

  close() {
    if (!this.isOpen) return;
    const inside = this._panel.contains(document.activeElement);
    this._overlay.hidden = true;
    const to = this._restoreTo;
    this._restoreTo = null;
    if (inside) {
      if (to && to.isConnected && to.getClientRects().length) to.focus({ preventScroll: true });
      else document.activeElement?.blur?.();
    }
    this.onClose?.();
  }

  toggle() { if (this.isOpen) this.close(); else this.open(); }

  focusPanel() { if (this.isOpen) this._panel.focus({ preventScroll: true }); }

  // Pins the panel at its current spot, as the first drag does, so the
  // keyboard can move it from there.
  _pin() {
    const rect = this._panel.getBoundingClientRect();
    const st = this._panel.style;
    if (st.position !== 'fixed') {
      st.position = 'fixed';
      st.left = `${rect.left}px`;
      st.top = `${rect.top}px`;
      st.right = 'auto';
      st.margin = '0';
    }
    return rect;
  }

  // Keyboard move, clamped like the mouse drag. Not on a phone: a sheet is not a window.
  nudge(dx, dy) {
    if (!this.isOpen || this._phone()) return false;
    const rect = this._pin();
    const minVisible = 40;
    this._panel.style.left = `${clamp(rect.left + dx, minVisible - rect.width, window.innerWidth - minVisible)}px`;
    this._panel.style.top = `${clamp(rect.top + dy, 0, window.innerHeight - minVisible)}px`;
    return true;
  }

  // Keyboard resize.
  resizeBy(dw, dh) {
    if (!this.isOpen || this._phone()) return false;
    const rect = this._pin();
    const st = this._panel.style;
    if (dw) st.width = `${clamp(rect.width + dw, 280, window.innerWidth - 20)}px`;
    if (dh) {
      st.maxHeight = 'none';
      st.height = `${clamp(rect.height + dh, 200, window.innerHeight - 20)}px`;
    }
    return true;
  }
}

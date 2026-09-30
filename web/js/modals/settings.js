// The one Settings modal. Replaces what used to be three separate entry
// points — a Debug button, a Logging button and a Perf checkbox — plus the
// row of graph controls that lived in its own toolbar under the main one.
//
// Everything is a collapsible <details> section, Graph first, and new sections
// just append. The modules that drive the individual sections (DebugModal,
// LoggingModal, PerfPanel) still own their own contents and their own
// render(); this owns only the shell, and tells them whether they are visible.
//
// Visibility is two conditions, not one: the modal has to be open AND the
// section expanded. That matters because every render() is called from the
// app's rAF loop and early-returns on `isOpen` — a collapsed Debug section
// should cost nothing even while the modal is up.
//
// Two presentations of the same panel:
//   * wide screen — a floating, draggable, undimmed window, so the graph stays
//     visible and live behind it (see the #settings-modal rules in style.css)
//   * phone — a dimmed bottom sheet you drag between full height, half height
//     and gone, or dismiss by tapping the backdrop (see sheet.js)
// The mobile CSS uses !important so it beats the inline left/top makeDraggable
// pins on, which means a window crossing the breakpoint mid-session lands in
// the right geometry rather than keeping whatever the drag left behind.

import { makeDraggable } from '../draggable.js';
import { makeSheet } from '../sheet.js';

// Must match the max-width in the #settings-modal mobile rules in style.css.
// One number in two languages is a hazard, but the alternative is reading a
// custom property back out of the computed style on every pointerdown.
const MOBILE_MAX = 767;

export class SettingsModal {
  constructor(app) {
    this.app = app;
    this._overlay = document.getElementById('settings-modal');
    this._panel = this._overlay.querySelector('.modal');
    const header = this._overlay.querySelector('.modal-header');
    const grab = this._overlay.querySelector('.sheet-grab');

    this._mq = window.matchMedia(`(max-width: ${MOBILE_MAX}px)`);
    const phone = () => this._mq.matches;
    this._phone = phone;
    // Where focus came from, so closing hands it back — a keyboard user who
    // pressed Alt+S from a tile lands on that tile again.
    this._restoreTo = null;

    // Two behaviours on one panel, each gated on the viewport: a free-floating
    // window on a desktop, a draggable bottom sheet on a phone. Both stay
    // bound; `enabled` decides which one claims a given gesture.
    makeDraggable(this._panel, header, { enabled: () => !phone() });
    this._sheet = makeSheet(this._panel, {
      handles: [grab, header],
      enabled: phone,
      onDismiss: () => this.close(),
    });

    // Crossing the breakpoint must not leave a half-dragged sheet's transform
    // on the desktop panel, where it would simply offset the window.
    this._mq.addEventListener?.('change', () => this._sheet.reset());

    document.getElementById('settings-btn').addEventListener('click', () => this.toggle());
    document.getElementById('settings-close-btn').addEventListener('click', () => this.close());

    // Tap the dimmed backdrop to dismiss — standard for a sheet, and free on
    // desktop where .modal-overlay is pointer-events:none and never sees this.
    this._overlay.addEventListener('click', (e) => {
      if (e.target === this._overlay) this.close();
    });
  }

  get isOpen() {
    return !this._overlay.hidden;
  }

  // `sectionId` expands that section on the way in — how the per-section
  // modules' own open() methods land the user on their own content.
  open(sectionId) {
    // Always back to full height: a sheet left at half, or mid-dismiss, would
    // otherwise reopen exactly where it was last shoved.
    this._sheet.reset();
    if (!this.isOpen) {
      const ae = document.activeElement;
      this._restoreTo = ae && ae !== document.body && !this._panel.contains(ae) ? ae : null;
    }
    this._overlay.hidden = false;
    if (sectionId) this.expand(sectionId);
    // Real focus onto the panel (tabindex=-1), so the keyboard is "in
    // Settings" from the first keypress and Alt+J enters the controls at the
    // top. Not a control: nothing toggles by accident.
    if (!this._panel.contains(document.activeElement)) this._panel.focus({ preventScroll: true });
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
  }

  get panel() {
    return this._panel;
  }

  // True when keyboard focus is inside the panel.
  get hasFocus() {
    return this.isOpen && this._panel.contains(document.activeElement);
  }

  focusPanel() {
    if (!this.isOpen) return;
    this._panel.focus({ preventScroll: true });
  }

  // Pins the panel to a fixed position at its current spot, the way the
  // first pointerdown of a drag does, so the keyboard can move it from there.
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

  // Keyboard move (Alt+Shift+direction), clamped like the mouse drag so the
  // header can always be reached again. Not on a phone: the sheet is not a
  // window.
  nudge(dx, dy) {
    if (!this.isOpen || this._phone()) return false;
    const rect = this._pin();
    const minVisible = 40;
    const left = clamp(rect.left + dx, minVisible - rect.width, window.innerWidth - minVisible);
    const top = clamp(rect.top + dy, 0, window.innerHeight - minVisible);
    this._panel.style.left = `${left}px`;
    this._panel.style.top = `${top}px`;
    return true;
  }

  // Keyboard resize (Alt+-/= and the Shift forms).
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

  toggle() {
    if (this.isOpen) this.close();
    else this.open();
  }

  expand(sectionId) {
    const section = document.getElementById(sectionId);
    if (section) section.open = true;
  }

  // True only when this section's contents are actually on screen — see the
  // two-condition note above.
  sectionVisible(sectionId) {
    if (!this.isOpen) return false;
    const section = document.getElementById(sectionId);
    return !!section && section.open;
  }
}

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(v, hi));
}

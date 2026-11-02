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

import { Dialog } from './dialog.js';
import { sectionShown } from '../settings/caps.js';

// The window/sheet behaviour is js/modals/dialog.js's (shared with the
// Scanner's dialogs); this adds the sections and the toolbar button.
export class SettingsModal extends Dialog {
  constructor(app) {
    super({
      overlay: document.getElementById('settings-modal'),
      closeBtn: document.getElementById('settings-close-btn'),
    });
    this.app = app;
    document.getElementById('settings-btn').addEventListener('click', () => this.toggle());
    this._device = document.getElementById('settings-device');
    this._pick = document.getElementById('settings-device-pick');
    this._note = document.createElement('p');
    this._note.className = 'dim settings-device-note';
    this._note.hidden = true;
    document.getElementById('settings-body').prepend(this._note);
    this._pick.addEventListener('change', () => {
      this._note.hidden = true;
      this.app.selectSettingsDevice(this._pick.value);
    });
  }

  // Remote boards that have a window. No boards: the control stays hidden
  // and every title omits [Local].
  setDevices(names) {
    const selected = this._pick.value;
    const keep = names.includes(selected) ? selected : '';
    this._pick.replaceChildren();
    const local = document.createElement('option');
    local.value = '';
    local.textContent = 'Local';
    this._pick.appendChild(local);
    for (const name of names) {
      const o = document.createElement('option');
      o.value = name;
      o.textContent = name;
      this._pick.appendChild(o);
    }
    this._pick.value = keep;
    this._device.hidden = names.length === 0;
  }

  setDeviceNote(text, bad) {
    this._note.textContent = text || '';
    this._note.hidden = !text;
    this._note.classList.toggle('err', !!bad && !!text);
  }

  // Hide sections this board's sys.info does not list.
  applyCapabilities(info) {
    for (const s of this.app.sections || []) {
      const el = document.getElementById(s.id);
      if (!el) continue;
      const show = sectionShown(s.id, info);
      el.hidden = !show;
      s.onDevice?.(info);
      if (show) s.soon?.();
    }
  }

  // `sectionId` expands that section on the way in -- how the per-section
  // modules' own open() methods land the user on their own content.
  open(sectionId) {
    super.open();
    if (sectionId) this.expand(sectionId);
    // The socket was dropped when this closed, so a remote choice connects again.
    if (this.app.settingsPeer()) this.app.selectSettingsDevice(this._pick.value);
  }

  close() {
    super.close();
    this.app.dropSettingsPeer();
  }

  expand(sectionId) {
    const section = document.getElementById(sectionId);
    if (section) section.open = true;
  }

  // True only when this section's contents are actually on screen -- see the
  // two-condition note above.
  sectionVisible(sectionId) {
    if (!this.isOpen) return false;
    const section = document.getElementById(sectionId);
    return !!section && !section.hidden && section.open;
  }
}

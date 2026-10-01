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
  }

  // `sectionId` expands that section on the way in -- how the per-section
  // modules' own open() methods land the user on their own content.
  open(sectionId) {
    super.open();
    if (sectionId) this.expand(sectionId);
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
    return !!section && section.open;
  }
}

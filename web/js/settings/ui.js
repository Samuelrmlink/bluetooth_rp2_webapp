// Settings -> UI: how the workspace is arranged, and editor preferences.
// The arrangement is also on the toolbar; app.syncLayoutButtons() keeps the
// two in step through syncLayout().
const SECTION = 'sect-ui';

export class UiSection {
  constructor(app, settings) {
    this.app = app;
    this.settings = settings;
    const d = document.createElement('details');
    d.className = 'sect';
    d.id = SECTION;
    d.innerHTML = `
      <summary>UI</summary>
      <div class="sect-body">
        <div class="row">
          <label for="layout-mode">Arrangement</label>
          <select id="layout-mode">
            <option value="auto">Auto</option>
            <option value="side">Side by side — root split horizontal</option>
            <option value="stack">Top / bottom — root split vertical</option>
            <option value="tabs">One at a time</option>
          </select>
        </div>
        <div class="dim">Auto follows the screen: side by side in landscape, top/bottom in
          portrait, one at a time when there isn't room for two.</div>
        <hr>
        <div class="row">
          <label><input type="checkbox" id="pref-vim"> Vim keys in the editor</label>
        </div>
        <div class="dim">Normal, insert and visual modes with the everyday motions and
          operators. Off, the editor behaves like a plain text editor.</div>
      </div>`;
    document.getElementById('settings-body').appendChild(d);
    this._mode = d.querySelector('#layout-mode');
    this._mode.addEventListener('change', () => app.setLayoutMode(this._mode.value));
    this._vim = d.querySelector('#pref-vim');
    this._vim.checked = app.prefs.vim;
    this._vim.addEventListener('change', () => app.setPref('vim', this._vim.checked));
  }

  syncLayout() {
    if (this._mode.value !== this.app.layoutMode) this._mode.value = this.app.layoutMode;
  }
}

// Where a browser build is written, and whether it is installed.
//
// The compiler in the page emits one unsigned <name>.<arch>.pex. This asks
// where that file goes: a folder on the SD card, internal storage, or both.
// Install reads the internal copy (pex/<name>.<arch>.pex) into the PEX
// partition and does not start it. A card-only file is not installed from
// here: the firmware reads an sd: file into the PEX pool first, and a
// module larger than that pool cannot be installed that way.

import { Dialog } from '../modals/dialog.js';
import { badPath } from '../tar.js';

const PREF = 'btgw.build.dest';
const SD_PATH_MAX = 96;
const FAT = /[\\:*?"<>|]/;
const DEFAULTS = { sd: true, internal: false, install: false, sdDir: 'sd/pex' };

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export function loadBuildPref() {
  try {
    const raw = JSON.parse(localStorage.getItem(PREF) || 'null');
    if (!raw || typeof raw !== 'object') return { ...DEFAULTS };
    return {
      sd: raw.sd !== false,
      internal: !!raw.internal,
      install: !!raw.install,
      sdDir: typeof raw.sdDir === 'string' && raw.sdDir ? raw.sdDir : DEFAULTS.sdDir,
    };
  } catch {
    return { ...DEFAULTS };
  }
}

export function saveBuildPref(pref) {
  try { localStorage.setItem(PREF, JSON.stringify(pref)); } catch { /* private mode */ }
}

// Why a card path cannot be written, or null. `path` is the editor form
// (sd/...), and the limit is the card's own 96 characters after the sd/ prefix.
export function badSdPath(path) {
  const p = String(path || '');
  if (!p.startsWith('sd/')) return 'a card path starts with sd/';
  const rel = p.slice(3);
  if (rel.length > SD_PATH_MAX) return `longer than ${SD_PATH_MAX} characters on the card`;
  if (FAT.test(rel) || /[\0-\x1f]/.test(rel)) return 'a character the card cannot name';
  return badPath(rel);
}

// The files this build will write. Install forces the internal copy,
// because that is the file pex.install reads.
export function resolveBuildChoice({ name, arch, sd, sdDir, internal, install }) {
  const file = `${name}.${arch}.pex`;
  const useInternal = !!(internal || install);
  let sdPath = null;
  if (sd) {
    const dir = String(sdDir || '').replace(/\/+$/, '');
    sdPath = dir ? `${dir}/${file}` : file;
  }
  let error = null;
  if (sdPath) error = badSdPath(sdPath);
  if (!sdPath && !useInternal) error = error || 'Choose the SD card, internal storage, or both.';
  return {
    file,
    sdPath: error ? null : sdPath,
    internalPath: useInternal ? `pex/${file}` : null,
    install: !!install,
    error,
  };
}

export class BuildDialog {
  constructor(app) {
    const body = document.createElement('div');
    body.className = 'bd-body';
    body.innerHTML = `
      <p class="bd-what"></p>
      <p class="bd-sign dim"></p>
      <label class="bd-row"><input type="checkbox" class="bd-sd"> Save on the SD card</label>
      <label class="bd-folder">Folder
        <input class="bd-dir" spellcheck="false" autocomplete="off" autocapitalize="off"></label>
      <p class="bd-full dim"></p>
      <label class="bd-row"><input type="checkbox" class="bd-internal"> Copy to internal storage</label>
      <p class="bd-lfs dim"></p>
      <label class="bd-row"><input type="checkbox" class="bd-install"> Install into the PEX partition</label>
      <p class="bd-note dim">Install writes the internal copy into the partition and does not start the module. A signed-only board refuses an unsigned file.</p>
      <p class="bd-err" hidden></p>
      <div class="bd-actions">
        <button type="button" class="pane-btn bd-cancel">Cancel</button>
        <button type="button" class="pane-btn bd-go">Build</button>
      </div>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'build-dlg', title: 'Build module', body }));
    this.q = (s) => body.querySelector(s);
    this.q('.bd-cancel').addEventListener('click', () => this._end(null));
    this.q('.bd-go').addEventListener('click', () => this._accept());
    this.q('.bd-sd').addEventListener('change', () => this._sync());
    this.q('.bd-internal').addEventListener('change', () => {
      if (!this.q('.bd-install').checked) this._internalWanted = this.q('.bd-internal').checked;
      this._sync();
    });
    this.q('.bd-install').addEventListener('change', () => {
      const internal = this.q('.bd-internal');
      if (this.q('.bd-install').checked) {
        if (!internal.disabled) this._internalWanted = internal.checked;
        internal.checked = true;
      } else {
        internal.checked = !!this._internalWanted;
      }
      this._sync();
    });
    this.q('.bd-dir').addEventListener('input', () => this._sync());
    this.q('.bd-dir').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); this._accept(); }
    });
    this.dlg.onClose = () => this._end(null);
  }

  ask({ name, arch }) {
    this._end(null);
    this.name = name;
    this.arch = arch;
    const pref = loadBuildPref();
    this.q('.bd-what').innerHTML = `Build unsigned <b>${esc(name)}.${esc(arch)}.pex</b>. The open file is compiled with the other sources of that module. The .c is not written.`;
    this.q('.bd-sign').textContent = 'Sign on the host before a signed-only board will load it: python3 pex/tools/pexlink.py sign';
    this._internalWanted = pref.internal;
    this.q('.bd-sd').checked = pref.sd;
    this.q('.bd-install').checked = pref.install;
    this.q('.bd-internal').checked = pref.install || pref.internal;
    this.q('.bd-dir').value = pref.sdDir;
    this._sync();
    return new Promise((resolve) => {
      this._resolve = resolve;
      this.dlg.open();
      setTimeout(() => this.q('.bd-cancel').focus(), 0);
    });
  }

  _choice() {
    return resolveBuildChoice({
      name: this.name,
      arch: this.arch,
      sd: this.q('.bd-sd').checked,
      sdDir: this.q('.bd-dir').value.trim(),
      internal: this.q('.bd-internal').checked,
      install: this.q('.bd-install').checked,
    });
  }

  _sync() {
    const install = this.q('.bd-install').checked;
    const internal = this.q('.bd-internal');
    if (install) internal.checked = true;
    internal.disabled = install;
    const sd = this.q('.bd-sd').checked;
    this.q('.bd-dir').disabled = !sd;
    const choice = this._choice();
    this.q('.bd-full').textContent = choice.sdPath ? choice.sdPath : 'Not written to the card.';
    this.q('.bd-lfs').textContent = choice.internalPath
      ? `${choice.internalPath}${install ? ' — this is the file that gets installed' : ' — replaces that file if it exists. The partition is unchanged until you install.'}`
      : 'Not copied to internal storage.';
    const err = this.q('.bd-err');
    err.hidden = !choice.error;
    err.textContent = choice.error || '';
    this.q('.bd-go').disabled = !!choice.error;
  }

  _accept() {
    const choice = this._choice();
    if (choice.error) { this._sync(); return; }
    saveBuildPref({
      sd: this.q('.bd-sd').checked,
      internal: !!this._internalWanted,
      install: choice.install,
      sdDir: this.q('.bd-dir').value.trim() || DEFAULTS.sdDir,
    });
    this._end(choice);
  }

  _end(choice) {
    const r = this._resolve;
    if (!r) return;
    this._resolve = null;
    if (this.dlg.isOpen) this.dlg.close();
    r(choice);
  }
}

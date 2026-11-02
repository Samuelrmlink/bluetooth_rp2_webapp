// Settings -> Startup: the one boot list (cfg/launch.json, launch.set).
// Order is launch order. A row can be off without leaving the list. The
// running firmware starts the list; this page only edits it.
import { makeSection, button, setNote, PolledSection } from './common.js';
import { luaShown } from './caps.js';

export const LAUNCH_WHENS = [
  { id: 'wifi', label: 'After Wi-Fi' },
  { id: 'wireguard', label: 'After WireGuard is confirmed' },
  { id: 'meshtastic', label: 'After the Meshtastic relay' },
];

const NAME = /^[A-Za-z0-9_-]{1,32}$/;

export function whenNote(when) {
  if (when === 'wireguard') {
    return 'The list waits until WireGuard is confirmed and an uplink ping has come back. It does not time out. Launch now starts it on this boot.';
  }
  if (when === 'meshtastic') {
    return 'The Meshtastic relay is not implemented, so the list stays held. Launch now starts it on this boot.';
  }
  return 'The list waits until Wi-Fi is up, turned off, or its grace window has passed.';
}

export function bootSummary(st) {
  if (!st || st.error) return 'This firmware has no startup list yet.';
  if (st.phase === 'skipped') {
    if (st.reason === 'watchdog') return 'This boot followed a watchdog reset. Nothing was started.';
    if (st.reason === 'safe') return 'This boot is safe mode, from a second reset before the first one had settled. Nothing was started.';
    return 'Nothing was started.';
  }
  if (st.phase === 'waiting') {
    if (st.when === 'meshtastic') return 'Held. The Meshtastic relay is not implemented. Launch now starts this list on this boot.';
    if (st.when === 'wireguard') return 'Waiting until WireGuard is confirmed and an uplink ping has come back.';
    if (st.when === 'wifi') return 'Waiting until Wi-Fi is up, turned off, or its grace window has passed.';
    return `Waiting. "${st.when}" is not a gate this firmware knows.`;
  }
  if (st.phase === 'running') {
    return `Started ${st.index} of ${st.count}. The next one waits until this connection and its negotiation have finished. A device that never connects gives up the wait after about a minute.`;
  }
  if (st.phase === 'done') return st.count ? 'The list has been started.' : 'The list is empty.';
  return '';
}

export function moveEntry(entries, index, dir) {
  const j = index + dir;
  if (index < 0 || index >= entries.length || j < 0 || j >= entries.length) return entries;
  const next = entries.slice();
  const [row] = next.splice(index, 1);
  next.splice(j, 0, row);
  return next;
}

export function removeEntry(entries, index) {
  if (index < 0 || index >= entries.length) return entries;
  return entries.filter((_, i) => i !== index);
}

// PEX modules from pex.installed, plus Lua scripts only when this board
// runs them. `files` is that board's lua/ listing. A board without the
// script module must not be offered another board's scripts.
export function startupChoices(info, installed, files) {
  const rows = [];
  for (const m of installed || []) {
    if (m?.name && NAME.test(m.name)) rows.push({ kind: 'pex', name: m.name });
  }
  if (!luaShown(info)) return rows;
  for (const f of files || []) {
    if (f.dir || !/^[^/]+\.lua(\.gz)?$/i.test(f.name || '')) continue;
    const name = f.name.replace(/\.lua(\.gz)?$/i, '');
    if (NAME.test(name)) rows.push({ kind: 'lua', name });
  }
  return rows;
}

// A new row, or { error }. The same name once, whatever its kind.
export function addEntry(entries, kind, name) {
  if (kind !== 'pex' && kind !== 'lua') return { error: 'Pick a PEX app or a Lua script.' };
  if (!NAME.test(name || '')) return { error: 'The name is letters, digits, underscore or dash.' };
  if (entries.some((e) => e.name === name)) return { error: `${name} is already in the list.` };
  if (entries.length >= 16) return { error: 'The list holds 16 entries.' };
  return { entries: entries.concat([{ kind, name, enabled: true }]) };
}

export class StartupSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-startup', 2000);
    const opts = LAUNCH_WHENS.map((w) => `<option value="${w.id}">${w.label}</option>`).join('');
    const d = makeSection(this.id, 'Startup', `
      <div class="launch-sum"></div>
      <div class="dim launch-note"></div>
      <div class="row"><label for="launch-when">Start the list</label>
        <select id="launch-when">${opts}</select></div>
      <div class="dim launch-when-note"></div>
      <div class="row launch-actions"></div>
      <div class="launch-rows"></div>
      <div class="row launch-add">
        <select class="launch-pick"></select>
      </div>
      <div class="dim">Saving writes cfg/launch.json. It takes effect on the next reboot. The older autostart files stay on disk and are left unused once this file exists.</div>`);
    this._sum = d.querySelector('.launch-sum');
    this._note = d.querySelector('.launch-note');
    this._whenNote = d.querySelector('.launch-when-note');
    this._when = d.querySelector('#launch-when');
    this._rows = d.querySelector('.launch-rows');
    this._pick = d.querySelector('.launch-pick');
    this._entries = [];
    this._dirty = false;
    this._got = false;
    this._info = null;
    this._pickGen = 0;
    const acts = d.querySelector('.launch-actions');
    acts.append(
      button('Launch now', () => this.now()),
      button('Save', () => this.save()),
    );
    d.querySelector('.launch-add').append(button('Add', () => this.add()));
    this._when.addEventListener('change', () => {
      this._dirty = true;
      this._whenNote.textContent = whenNote(this._when.value);
    });
    this._whenNote.textContent = whenNote(this._when.value);
  }

  onConnect() {
    this._got = false;
    this.soon();
  }

  // The device changed. Drop the previous board's rows and its Add list.
  onDevice(info) {
    this._info = info;
    this._got = false;
    this._dirty = false;
    this._pickGen += 1;
    this._pick.replaceChildren();
    this.soon();
  }

  _paint() {
    this._rows.replaceChildren();
    this._entries.forEach((e, i) => {
      const row = document.createElement('div');
      row.className = 'row';
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = !!e.enabled;
      box.addEventListener('change', () => {
        this._entries[i] = { ...this._entries[i], enabled: box.checked };
        this._dirty = true;
      });
      const label = document.createElement('span');
      label.textContent = `${e.kind === 'lua' ? 'Lua' : 'PEX'} ${e.name}`;
      row.append(
        box,
        label,
        button('Up', () => this._edit(moveEntry(this._entries, i, -1))),
        button('Down', () => this._edit(moveEntry(this._entries, i, 1))),
        button('Remove', () => this._edit(removeEntry(this._entries, i))),
      );
      this._rows.append(row);
    });
  }

  _edit(entries) {
    this._entries = entries;
    this._dirty = true;
    this._paint();
  }

  async _fillPick() {
    const gen = this._pickGen;
    const fs = await import('../fs.js');
    const showLua = luaShown(this._info);
    const [inst, lua] = await Promise.all([
      this.ws.call('pex.installed').catch(() => null),
      showLua ? fs.list('lua').catch(() => null) : null,
    ]);
    if (gen !== this._pickGen) return;
    const prev = this._pick.value;
    this._pick.replaceChildren();
    const rows = startupChoices(this._info, inst?.modules, lua?.files);
    for (const row of rows) {
      const o = document.createElement('option');
      o.value = `${row.kind}:${row.name}`;
      o.textContent = `${row.kind === 'lua' ? 'Lua' : 'PEX'} ${row.name}`;
      this._pick.append(o);
    }
    if (!rows.length) {
      const o = document.createElement('option');
      o.value = '';
      o.textContent = 'Nothing on this board to add';
      this._pick.append(o);
    }
    if (prev) this._pick.value = prev;
  }

  add() {
    const raw = this._pick.value || '';
    const i = raw.indexOf(':');
    const res = addEntry(this._entries, raw.slice(0, i), raw.slice(i + 1));
    if (res.error) { setNote(this._note, res.error, true); return; }
    setNote(this._note, '');
    this._edit(res.entries);
  }

  async save() {
    try {
      await this.ws.call('launch.set', { when: this._when.value, entries: this._entries });
      this._dirty = false;
      setNote(this._note, 'Saved. The list starts on the next reboot.');
    } catch (e) {
      setNote(this._note, e.message, true);
    }
  }

  async now() {
    try {
      const r = await this.ws.call('launch.now');
      setNote(this._note, r.started ? 'Starting the list on this boot.'
        : r.reason === 'done' ? 'The list has already been started this boot.'
        : r.reason === 'watchdog' ? 'This boot followed a watchdog reset, so the list stays idle.'
        : r.reason === 'safe' ? 'This boot is safe mode, so the list stays idle.'
        : 'The list stays idle.');
    } catch (e) {
      setNote(this._note, e.message, true);
    }
    this.soon();
  }

  async poll() {
    const st = await this.ws.call('launch.status').catch((e) => ({ error: e.message }));
    this._sum.textContent = bootSummary(st);
    if (st.error) return;
    if (!this._dirty && !this._got && Array.isArray(st.entries)) {
      this._entries = st.entries.map((e) => ({ kind: e.kind, name: e.name, enabled: e.enabled !== false }));
      if (LAUNCH_WHENS.some((w) => w.id === st.when)) this._when.value = st.when;
      this._whenNote.textContent = whenNote(this._when.value);
      this._paint();
      this._got = true;
      if (st.source === 'fallback') {
        setNote(this._note, 'This boot is using the previous autostart files. Saving writes cfg/launch.json.');
      }
    }
    if (!this._pick.options.length) this._fillPick().catch(() => {});
  }
}

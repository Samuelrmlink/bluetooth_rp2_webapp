// Shared bits for Settings sections: the <details> shell, key/value rows,
// and polling that runs only while a section is actually on screen.

export function makeSection(id, title, bodyHtml) {
  const d = document.createElement('details');
  d.className = 'sect';
  d.id = id;
  d.innerHTML = `<summary>${title}</summary><div class="sect-body">${bodyHtml}</div>`;
  document.getElementById('settings-body').appendChild(d);
  return d;
}

// Rows of label/value, as text -- values come off the air or the device.
export function kvRows(el, rows) {
  const frag = document.createDocumentFragment();
  for (const [k, v] of rows) {
    const kd = document.createElement('div');
    kd.className = 'k';
    kd.textContent = k;
    const vd = document.createElement('div');
    vd.className = 'v';
    vd.textContent = v ?? '—';
    frag.append(kd, vd);
  }
  el.replaceChildren(frag);
}

// A status line. `bad` draws it in red; a later ordinary note clears that.
export function setNote(el, text, bad) {
  if (!el) return;
  el.textContent = text || '';
  el.classList.toggle('err', !!bad && !!text);
}

export function button(label, onClick) {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'pane-btn';
  b.textContent = label;
  b.addEventListener('click', onClick);
  return b;
}

// A section base: `poll()` runs on open and every `every` ms while the
// section is expanded in an open Settings panel, never otherwise.
export class PolledSection {
  constructor(app, settings, id, every) {
    this.app = app;
    this.settings = settings;
    this.id = id;
    this.every = every;
    this._last = -Infinity;
    this._busy = false;
  }

  // The board Settings is aimed at. Local uses the page socket. A remote
  // board uses its own, opened when that board is picked.
  get ws() {
    return this.app.settingsSocket();
  }

  render(t) {
    if (!this.settings.sectionVisible(this.id) || this._busy || !this.ws.connected) return;
    if (t - this._last < this.every) return;
    this._last = t;
    this._busy = true;
    Promise.resolve(this.poll()).catch(() => {}).finally(() => { this._busy = false; });
  }

  // After an action, show its effect without waiting out the interval.
  soon() { this._last = -Infinity; }
}

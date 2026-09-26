// A keyboard-driven popup for <select> elements.
//
// The browser's own dropdown is a separate window the page cannot see
// into: once it is open, no key we bind reaches it, so h/j/k/l cannot walk
// it. This draws the option list in the page instead — Space or Enter on a
// focused <select> opens it, j/k or the arrows move the highlight, Enter,
// Space or l pick, Esc or h cancel, and a click on an option picks it too.
// The mouse still gets the native dropdown: clicking a <select> is
// untouched, and this only ever opens from the keyboard.
//
// Picking sets the <select>'s value and fires `input` and `change`, so the
// code listening to that <select> cannot tell the difference.

export class Dropdown {
  constructor() {
    this._select = null;
    this._items = [];
    this._index = 0;
    this._el = document.createElement('div');
    this._el.id = 'dropdown';
    this._el.className = 'dropdown';
    this._el.setAttribute('role', 'listbox');
    this._el.hidden = true;
    document.body.appendChild(this._el);

    // A click anywhere else, a scroll or a resize: the popup no longer
    // lines up with its <select>, so it goes.
    document.addEventListener('pointerdown', (e) => {
      if (this.isOpen && !this._el.contains(e.target)) this.close(false);
    }, true);
    window.addEventListener('resize', () => this.close(false));
    window.addEventListener('scroll', () => this.close(false), true);
    this._el.addEventListener('click', (e) => {
      const item = e.target.closest('.dropdown-item');
      if (!item) return;
      this._index = Number(item.dataset.index);
      this.close(true);
    });
  }

  get isOpen() {
    return !this._el.hidden;
  }

  open(select) {
    if (!select || !select.options.length) return false;
    this.close(false);
    this._select = select;
    this._items = [];
    const frag = document.createDocumentFragment();
    [...select.options].forEach((o, i) => {
      const item = document.createElement('div');
      item.className = 'dropdown-item';
      item.setAttribute('role', 'option');
      item.dataset.index = String(i);
      item.textContent = o.textContent.trim() || ' ';
      if (o.disabled) item.setAttribute('aria-disabled', 'true');
      frag.appendChild(item);
      this._items.push(item);
    });
    this._el.replaceChildren(frag);
    this._index = Math.max(0, select.selectedIndex);
    this._el.hidden = false;
    this._place();
    this._highlight();
    return true;
  }

  // Positions the list under its <select>, or above it when the space
  // below is short; never wider than the viewport.
  _place() {
    const r = this._select.getBoundingClientRect();
    const st = this._el.style;
    st.minWidth = `${Math.max(120, r.width)}px`;
    st.maxWidth = `${window.innerWidth - 16}px`;
    const h = this._el.offsetHeight;
    const below = window.innerHeight - r.bottom;
    const top = below >= h + 8 || r.top < h + 8 ? r.bottom + 2 : r.top - h - 2;
    st.top = `${Math.max(4, top)}px`;
    st.left = `${Math.max(4, Math.min(r.left, window.innerWidth - this._el.offsetWidth - 8))}px`;
  }

  _highlight() {
    this._items.forEach((it, i) => {
      if (i === this._index) it.setAttribute('aria-selected', 'true');
      else it.removeAttribute('aria-selected');
    });
    this._items[this._index]?.scrollIntoView?.({ block: 'nearest' });
  }

  _move(delta) {
    const n = this._items.length;
    let i = this._index;
    for (let k = 0; k < n; k++) {
      i = (((i + delta) % n) + n) % n;
      if (this._items[i].getAttribute('aria-disabled') !== 'true') break;
    }
    this._index = i;
    this._highlight();
  }

  // Keys while open. Returns true when consumed.
  handleKey(e) {
    if (!this.isOpen) return false;
    switch (e.code) {
      case 'KeyJ': case 'ArrowDown': this._move(1); return true;
      case 'KeyK': case 'ArrowUp': this._move(-1); return true;
      case 'Home': this._index = 0; this._highlight(); return true;
      case 'End': this._index = this._items.length - 1; this._highlight(); return true;
      case 'Enter': case 'Space': case 'KeyL': case 'ArrowRight': this.close(true); return true;
      case 'Escape': case 'KeyH': case 'ArrowLeft': this.close(false); return true;
      default: return false;
    }
  }

  close(commit) {
    if (!this.isOpen) return;
    const sel = this._select;
    const i = this._index;
    this._el.hidden = true;
    this._el.replaceChildren();
    this._select = null;
    this._items = [];
    if (!sel) return;
    if (commit && i !== sel.selectedIndex && !sel.options[i]?.disabled) {
      sel.selectedIndex = i;
      sel.dispatchEvent(new Event('input', { bubbles: true }));
      sel.dispatchEvent(new Event('change', { bubbles: true }));
    }
    // Focus back where it was — unless picking re-rendered the <select>
    // (the pane picker does), in which case its replacement has it.
    if (sel.isConnected) sel.focus({ preventScroll: true });
  }
}

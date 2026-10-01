// The panel a pane's own settings live in.
//
// Every pane carries a thin bar with a gear; the gear opens this. One host
// element for the whole page, on the body — the content belongs to the
// pane and is only lent to the host while it is open, so each pane keeps
// its own controls, their state and their listeners for the life of the
// session.
//
// Why the host is on the body rather than inside the pane: .pane-events is
// `container-type: inline-size` (the events header collapses on a narrow
// pane), and container-type implies layout containment, which makes the
// pane a containing block for position:fixed descendants. A panel anchored
// inside it would be positioned against the pane and clipped by it. On the
// body it is positioned against the viewport, like the hover tooltip and
// the keyboard dropdown.
//
// Dismissal follows js/dropdown.js: a pointer outside, a scroll, a resize
// or Esc puts it away. So does any layout change — the gear it is anchored
// to has moved, and a panel left hanging over a rearranged workspace is
// worse than one that closed.

const GAP = 6;
const MARGIN = 6;

export class Popover {
  constructor() {
    this._el = document.createElement('div');
    this._el.id = 'pane-popover';
    this._el.className = 'pane-popover';
    this._el.hidden = true;
    document.body.appendChild(this._el);

    this._anchor = null;
    this._content = null;
    this._home = null;
    this._openedAt = 0;

    // Capture phase: the charts and the tiles stop propagation on their own
    // elements, and a click outside must close this wherever it landed.
    document.addEventListener(
      'pointerdown',
      (e) => {
        if (!this.isOpen) return;
        if (this._el.contains(e.target)) return;
        if (this._anchor && this._anchor.contains(e.target)) return; // the gear toggles itself
        this.close();
      },
      true,
    );
    window.addEventListener('resize', () => this.close());
    // Not a scroll in the first moments after opening: scroll events arrive a
    // frame late, so the one that scrolled the anchor into view (or a
    // finger's momentum scroll, just before the tap that opened this) would
    // otherwise close it as soon as it opened.
    window.addEventListener('scroll', () => { if (performance.now() - this._openedAt > 200) this.close(); }, true);
  }

  get isOpen() {
    return !this._el.hidden;
  }

  get el() {
    return this._el;
  }

  // The pane id this panel belongs to, or '' — tests assert on it, and it
  // is what tells a caller whether the gear it just pressed is already up.
  get paneId() {
    return this._el.dataset.pane || '';
  }

  /* anchor:  the gear button to hang under
     content: the pane's own .pane-settings element
     paneId:  for data-pane, and for toggle() */
  open(anchor, content, paneId = '') {
    if (this.isOpen) this.close();
    this._anchor = anchor;
    this._content = content;
    // Where to put it back: its own pane, which is where it lives.
    this._home = content.parentElement;
    content.hidden = false;
    this._el.replaceChildren(content);
    this._el.dataset.pane = paneId;
    this._el.hidden = false;
    this._openedAt = performance.now();
    this.reposition();
    // The first real control, so the keyboard lands somewhere useful.
    const first = this._el.querySelector('input, select, button, [tabindex]:not([tabindex="-1"])');
    first?.focus({ preventScroll: true });
    return true;
  }

  close() {
    if (!this.isOpen) return false;
    const anchor = this._anchor;
    if (this._content && this._home) {
      this._content.hidden = true;
      this._home.appendChild(this._content);
    }
    this._el.replaceChildren();
    this._el.hidden = true;
    this._el.dataset.pane = '';
    this._anchor = null;
    this._content = null;
    this._home = null;
    // Put the keyboard back on the gear, not on the body, so h/j/k/l
    // carries on from where it was.
    if (anchor && anchor.offsetParent !== null) anchor.focus({ preventScroll: true });
    return false;
  }

  toggle(anchor, content, paneId = '') {
    if (this.isOpen && this.paneId === paneId) return this.close();
    return this.open(anchor, content, paneId);
  }

  // Under the gear, flipped to stay on screen and then clamped — the same
  // discipline as the hover tooltip, and for the same reason: a panel that
  // overflows widens the document and lets the page scroll into dead space.
  reposition() {
    if (!this.isOpen || !this._anchor) return;
    const a = this._anchor.getBoundingClientRect();
    const w = this._el.offsetWidth;
    const h = this._el.offsetHeight;
    const vw = document.documentElement.clientWidth;
    const vh = document.documentElement.clientHeight;

    let x = a.left;
    if (x + w > vw - MARGIN) x = a.right - w;
    x = Math.max(MARGIN, Math.min(x, vw - MARGIN - w));

    let y = a.bottom + GAP;
    if (y + h > vh - MARGIN) y = a.top - GAP - h;
    y = Math.max(MARGIN, Math.min(y, vh - MARGIN - h));

    this._el.style.left = `${x}px`;
    this._el.style.top = `${y}px`;
  }
}

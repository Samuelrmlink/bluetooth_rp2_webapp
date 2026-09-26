// Plain-key navigation inside a window: h/j/k/l and the arrow keys move
// focus between the controls of whatever you are in — the active tile
// (its header buttons and the pane's own fields), the Settings panel, a
// floating window — without a modifier. Alt+keys are for the windows
// themselves (see keys.js); these are for what is inside one.
//
// Spatial, not a list: a direction picks the nearest control that way,
// judged by geometry (the same neighbour() the tiles use), so a row of
// header buttons reads left/right, a form reads up/down, and up from the
// top of a pane lands on its header. Where geometry finds nothing, DOM
// order is the fallback, so no control is ever unreachable.
//
// Controls that use a direction themselves keep it: on a range slider,
// left and right step it. A <select> keeps none — Tab / Shift+Tab step
// its value in place (keys.js) and Space/Enter open the dropdown popup
// (dropdown.js) — so the direction keys always move on. Text fields see
// none of this: you are typing, and Esc leaves the field.
//
// Section headings (<summary>) take right to expand and left to collapse;
// left from inside a section with nothing further left collapses it and
// returns to the heading.
//
// Lists (the PD event tables) are one focusable control from outside:
// Space or Enter "enters" one, and then j/k move its selection, G goes
// to the bottom, gg to the top, and Esc leaves it with the selection kept.
// A list registers a small controller (registerList) that says how to
// move; the table keeps the selection itself, so it survives re-renders.

import { neighbour } from './tiles.js';

const CONTROL = 'summary, input:not([type="hidden"]), select, button, textarea, a[href], [tabindex]:not([tabindex="-1"])';

// Inside a collapsed <details>? Its heading is not; everything below it is.
// Asked explicitly: Chromium keeps the contents of a closed <details> laid
// out (content-visibility), so they still report rects.
function inClosedDetails(el) {
  for (let d = el.closest('details'); d; d = d.parentElement?.closest('details')) {
    if (!d.open && el !== d.querySelector(':scope > summary')) return true;
  }
  return false;
}

// The focusable controls under `roots`, in DOM order, as currently shown.
export function controlsIn(roots) {
  const out = [];
  for (const root of roots) {
    if (!root) continue;
    for (const el of root.querySelectorAll(CONTROL)) {
      if (el.disabled || el.closest('[hidden]') || inClosedDetails(el)) continue;
      // A modal's title-bar close button is what Esc does; keep it out of
      // the walk so entering a panel lands on its first real control.
      if (el.closest('.modal-header')) continue;
      const r = el.getBoundingClientRect();
      if (r.width === 0 && r.height === 0) continue;
      out.push(el);
    }
  }
  return out;
}

export function isTextField(el) {
  if (!el) return false;
  if (el.isContentEditable) return true;
  if (el.tagName === 'TEXTAREA') return true;
  if (el.tagName === 'INPUT') return !['checkbox', 'radio', 'range', 'button', 'submit', 'reset', 'color', 'file'].includes(el.type);
  return false;
}

const DIRS = {
  KeyH: 'left', ArrowLeft: 'left',
  KeyJ: 'down', ArrowDown: 'down',
  KeyK: 'up', ArrowUp: 'up',
  KeyL: 'right', ArrowRight: 'right',
};

// The direction a plain keydown asks for, or null.
export function directionOf(e) {
  if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return null;
  return DIRS[e.code] || null;
}

function rectOf(el) {
  const r = el.getBoundingClientRect();
  return { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
}

function focusTo(el) {
  el.focus({ preventScroll: true });
  el.scrollIntoView?.({ block: 'nearest', inline: 'nearest' });
  return document.activeElement === el;
}

// Nearest scrollable ancestor (within `stop`) with room to move that way.
function scrollable(el, dir, stop) {
  const down = dir === 'down';
  for (let n = el; n && n !== stop?.parentElement; n = n.parentElement) {
    const cs = getComputedStyle(n);
    if (!/(auto|scroll)/.test(cs.overflowY)) continue;
    if (n.scrollHeight <= n.clientHeight + 1) continue;
    if (down ? n.scrollTop + n.clientHeight < n.scrollHeight - 1 : n.scrollTop > 0) return n;
  }
  return null;
}

// Fires the events a real change would, so listeners bound to either see it.
function announce(el) {
  el.dispatchEvent(new Event('input', { bubbles: true }));
  el.dispatchEvent(new Event('change', { bubbles: true }));
}

// Step a <select> to its previous/next enabled option without opening it.
export function stepSelect(sel, delta) {
  const opts = [...sel.options];
  let i = sel.selectedIndex;
  for (let k = 0; k < opts.length; k++) {
    i = (((i + delta) % opts.length) + opts.length) % opts.length;
    if (!opts[i].disabled) break;
  }
  if (i === sel.selectedIndex) return false;
  sel.selectedIndex = i;
  announce(sel);
  return true;
}

function stepRange(el, delta) {
  const step = Number(el.step) || 1;
  const before = el.value;
  el.stepUp?.(0); // no-op that validates the element supports stepping
  el.value = String(Number(el.value) + delta * step);
  if (el.value === before) return false;
  announce(el);
  return true;
}

export class Nav {
  /*
    getRoots(): the elements whose controls are in scope right now (the
                active tile, the Settings panel, ...); the first one is the
                scope's container.
  */
  constructor({ getRoots }) {
    this._getRoots = getRoots;
    // Where focus last was inside each scope, so leaving a text field with
    // Esc and pressing j resumes from there rather than from the top.
    this._last = new WeakMap();
    // [data-navlist] element -> { enter, move, first, last, count }
    this._lists = new Map();
    this._pendingG = 0; // time of a first `g`, for `gg`
    document.addEventListener('focusin', (e) => {
      const roots = this._getRoots();
      const root = roots.find((r) => r && r.contains(e.target));
      if (root && e.target !== root) this._last.set(root, e.target);
    });
  }

  // Makes `el` (focusable, [data-navlist]) a keyboard list driven by `ctl`.
  registerList(el, ctl) {
    if (!el) return;
    el.dataset.navlist = '';
    if (!el.hasAttribute('tabindex')) el.tabIndex = 0;
    this._lists.set(el, ctl);
    // Focus leaving the list leaves list mode too, so a click elsewhere
    // never strands the keyboard "inside" a list it is not on.
    el.addEventListener('focusout', () => el.removeAttribute('data-nav-active'));
  }

  unregisterList(el) {
    if (!el) return;
    this._lists.delete(el);
    el.removeAttribute('data-navlist');
    el.removeAttribute('data-nav-active');
  }

  // Keys while a list is entered. Returns true when consumed.
  _listKey(e, el, ctl) {
    const shift = e.shiftKey;
    switch (e.code) {
      case 'KeyJ': case 'ArrowDown': if (!shift) ctl.move(1); return true;
      case 'KeyK': case 'ArrowUp': if (!shift) ctl.move(-1); return true;
      case 'End': ctl.last(); return true;
      case 'Home': ctl.first(); return true;
      case 'KeyG': {
        if (shift) {
          ctl.last(); // G
        } else if (performance.now() - this._pendingG < 700) {
          this._pendingG = 0;
          ctl.first(); // gg
        } else {
          this._pendingG = performance.now();
        }
        return true;
      }
      case 'Escape': el.removeAttribute('data-nav-active'); return true;
      // Space/Enter/h/l do nothing inside a list, and must not scroll the
      // page or move focus out of it by surprise.
      case 'Space': case 'Enter': case 'KeyH': case 'KeyL': case 'ArrowLeft': case 'ArrowRight': return true;
      default: return false;
    }
  }

  // Handles a keydown with no Alt/Ctrl/Meta while focus is not in a text
  // field; true when it did something (the caller then preventDefaults).
  handle(e) {
    const roots = this._getRoots().filter(Boolean);
    if (!roots.length) return false;
    const scope = roots[0];
    const dir = directionOf(e);
    const active = document.activeElement;
    const inScope = active && active !== document.body && roots.some((r) => r.contains(active));
    const origin = inScope && active !== scope ? active : null;

    // A list: Space/Enter enter it; once entered it takes the keys.
    const listEl = origin?.closest?.('[data-navlist]');
    const ctl = listEl && this._lists.get(listEl);
    if (ctl) {
      if (listEl.hasAttribute('data-nav-active')) return this._listKey(e, listEl, ctl);
      if ((e.code === 'Space' || e.code === 'Enter') && !e.shiftKey) {
        if (!ctl.count()) return true; // nothing to walk yet
        listEl.setAttribute('data-nav-active', '');
        ctl.enter();
        return true;
      }
    }

    // A read-only overlay (help): up/down scroll it, nothing to focus.
    if (scope.dataset.navScroll !== undefined) {
      if (dir === 'up' || dir === 'down') {
        const body = scope.querySelector(scope.dataset.navScroll || '.modal-body') || scope;
        body.scrollBy({ top: dir === 'down' ? 80 : -80 });
        return true;
      }
      return false;
    }

    if (!dir) return false;

    // Controls that own a direction.
    if (origin) {
      if (origin.tagName === 'INPUT' && origin.type === 'range' && (dir === 'left' || dir === 'right')) {
        stepRange(origin, dir === 'right' ? 1 : -1);
        return true;
      }
      if (origin.tagName === 'SUMMARY') {
        const details = origin.parentElement;
        if (dir === 'right') {
          if (!details.open) {
            details.open = true;
            return true;
          }
          // Already open: right goes to its first control.
          const first = controlsIn([details]).find((c) => c !== origin);
          if (first) return focusTo(first);
          return true;
        }
        if (dir === 'left') {
          if (details.open) details.open = false;
          return true;
        }
      }
    }

    const controls = controlsIn(roots).filter((c) => c !== scope);
    if (!controls.length) return this._scroll(dir, origin || scope, scope);

    // From the container itself: resume where focus last was. Otherwise
    // down/right enter the contents (the first control that is not in the
    // tile's header — landing on the pane picker would make the next j
    // change the pane), up/left land on the header's last button, or the
    // last control when there is no header.
    let from = origin;
    if (!from) {
      const last = this._last.get(scope);
      if (last && controls.includes(last)) from = last;
      else if (dir === 'down' || dir === 'right') {
        return focusTo(controls.find((c) => !c.closest('.tile-head')) || controls[0]);
      } else {
        const head = controls.filter((c) => c.closest('.tile-head'));
        return focusTo(head.length ? head[head.length - 1] : controls[controls.length - 1]);
      }
    }

    const cands = controls.filter((c) => c !== from).map((c, i) => ({ id: i, rect: rectOf(c), el: c }));
    const hit = neighbour(rectOf(from), cands, dir);
    if (hit != null) return focusTo(cands[hit].el);

    // Nothing that way. Left from inside a section collapses it (a way
    // "out" that matches right being the way in); otherwise DOM order for
    // the sideways keys, scrolling for up/down when there is room.
    if (dir === 'left') {
      const details = from.closest('details');
      if (details && details.open && scope.contains(details)) {
        details.open = false;
        return focusTo(details.querySelector(':scope > summary'));
      }
    }
    if (this._scroll(dir, from, scope)) return true;
    const i = controls.indexOf(from);
    const next = dir === 'right' || dir === 'down' ? controls[i + 1] : controls[i - 1];
    if (next) return focusTo(next);
    return true; // at an edge: swallowed rather than scrolling the page
  }

  _scroll(dir, from, scope) {
    if (dir !== 'up' && dir !== 'down') return false;
    const s = scrollable(from, dir, scope);
    if (!s) return false;
    s.scrollBy({ top: dir === 'down' ? 80 : -80 });
    return true;
  }
}

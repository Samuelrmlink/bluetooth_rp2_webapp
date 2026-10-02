// A table cell's menu: a title, the caller's items, and the cell's whole
// value as a button that copies it. Shared by the Scanner and GATT Events.
//
// Opened from the keyboard, the table was in its selection mode, which ends
// when focus moves into the menu; closing the menu (Esc included) must close
// only the menu, so onClose puts the table back -- unless an item moved the
// keyboard elsewhere on purpose (Show in GATT).
import { copyText } from '../clipboard.js';
import { toast } from '../toast.js';

export async function copyWithToast(text, done = 'Copied') {
  toast((await copyText(text)) ? done : 'Could not copy');
}

// items: [{ label, fn, cls?, keep? }] -- keep: the item answers inside the
// menu, which stays open. info: a line under the title. onClose runs however
// the menu closes (the window un-highlights the cell it was for). Returns
// false when there was nothing to show, else { setInfo(text) }.
export function openCellMenu(app, { anchor, menuEl, title, info = '', items = [], value = '', wrap, paneId = 'cell-menu', onReturn, onClose }) {
  if (!anchor) return false;
  const m = menuEl;
  m.replaceChildren();
  const t = document.createElement('div');
  t.className = 'sm-title';
  t.textContent = title || '';
  m.appendChild(t);
  const infoEl = document.createElement('div');
  infoEl.className = 'dim sm-info';
  infoEl.textContent = info;
  infoEl.hidden = !info;
  m.appendChild(infoEl);
  const add = (label, fn, cls = '', keep = false) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = `pane-btn sm-item ${cls}`;
    b.textContent = label;
    b.addEventListener('click', () => { if (!keep) app.popover.close(); fn(); });
    m.appendChild(b);
    return b;
  };
  for (const it of items) add(it.label, it.fn, it.cls, it.keep);
  if (value) {
    add(value, () => copyWithToast(value), 'sm-value').title = 'Copy';
    const hint = document.createElement('div');
    hint.className = 'dim sm-hint';
    hint.textContent = 'Tap the value to copy it';
    m.appendChild(hint);
  }
  if (m.querySelectorAll('.sm-item').length === 0) return false;
  const fromTable = wrap?.hasAttribute('data-nav-active');
  app.popover.open(anchor, m, paneId, {
    onClose: () => {
      onClose?.();
      if (!fromTable || !wrap.isConnected) return;
      const ae = document.activeElement;
      if (ae && ae !== document.body && !wrap.contains(ae) && !anchor.contains(ae)) return;
      wrap.focus({ preventScroll: true });
      wrap.setAttribute('data-nav-active', '');
      onReturn?.();
    },
  });
  return {
    setInfo: (text) => { infoEl.textContent = text; infoEl.hidden = !text; app.popover.reposition(); },
  };
}

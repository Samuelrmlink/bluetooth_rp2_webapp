// The Columns dialog any grid table can have: a checkbox per column, grouped
// by each column's `group`, and "Reset columns". Shared by the Scanner and
// GATT Events, so the two behave the same.
import { Dialog } from '../modals/dialog.js';

export function columnsDialog(app, grid, { id, title = 'Columns' }) {
  const body = document.createElement('div');
  body.className = 'scan-dlg cols-dlg';
  const groups = new Map();
  for (const c of grid.columns) {
    if (c.pinned) continue;
    const g = c.group || '';
    if (!groups.has(g)) groups.set(g, []);
    groups.get(g).push(c);
  }
  for (const [g, cols] of groups) {
    if (g) {
      const h = document.createElement('div');
      h.className = 'cd-group';
      h.textContent = g;
      body.appendChild(h);
    }
    for (const c of cols) {
      const l = document.createElement('label');
      l.className = 'sd-check';
      l.innerHTML = `<input type="checkbox" data-col="${c.key}"> <span></span>`;
      l.querySelector('span').textContent = c.label;
      l.querySelector('input').addEventListener('change', (e) => grid.setHidden(c.key, !e.target.checked));
      body.appendChild(l);
    }
  }
  const act = document.createElement('div');
  act.className = 'sd-actions';
  act.innerHTML = '<button type="button" class="pane-btn cd-reset">Reset columns</button><span class="dim">Drag a heading to move it; drag its edge to resize.</span>';
  act.querySelector('.cd-reset').addEventListener('click', () => grid.reset());
  body.appendChild(act);
  const dlg = new Dialog({ id, title, body });
  dlg.sync = () => {
    for (const cb of body.querySelectorAll('input[data-col]')) cb.checked = !grid.isHidden(cb.dataset.col);
  };
  dlg.onOpen = () => dlg.sync();
  return app.registerDialog(dlg);
}

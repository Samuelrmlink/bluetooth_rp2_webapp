// A yes / no question in the app's own dialog -- for a control that asks
// before acting (a charger port off, the C1000's AC outlet off). Not the
// browser's confirm(): that can be silently suppressed ("prevent this page
// from creating additional dialogs"), and then the answer is always no.
// Cancel is the default: Enter and Esc both keep things as they are.
//
//   if (await askConfirm(app, { title, message, ok: 'Turn off' })) ...

import { Dialog } from '../modals/dialog.js';

let dlg = null;
let resolveFn = null;

function build(app) {
  const body = document.createElement('div');
  body.className = 'cf-body';
  body.innerHTML = `
    <p class="cf-text"></p>
    <div class="cf-actions">
      <button type="button" class="pane-btn cf-cancel">Cancel</button>
      <button type="button" class="pane-btn cf-ok">OK</button>
    </div>`;
  const d = app.registerDialog(new Dialog({ id: 'confirm-dlg', title: 'Confirm', body }));
  const end = (ok) => {
    const r = resolveFn;
    resolveFn = null;
    if (d.isOpen) d.close();
    r?.(ok);
  };
  body.querySelector('.cf-cancel').addEventListener('click', () => end(false));
  body.querySelector('.cf-ok').addEventListener('click', () => end(true));
  d.onClose = () => end(false);
  d.q = (s) => body.querySelector(s);
  return d;
}

export function askConfirm(app, { title = 'Confirm', message = '', ok = 'OK', danger = true } = {}) {
  if (!app?.registerDialog) return Promise.resolve(window.confirm(message));
  dlg ||= build(app);
  if (resolveFn) { const r = resolveFn; resolveFn = null; r(false); }   // a question still open: that one is a no
  dlg._overlay.querySelector('.dlg-title').textContent = title;
  dlg.q('.cf-text').textContent = message;
  const okBtn = dlg.q('.cf-ok');
  okBtn.textContent = ok;
  okBtn.classList.toggle('cf-danger', danger);
  return new Promise((resolve) => {
    resolveFn = resolve;
    dlg.open();
    setTimeout(() => dlg.q('.cf-cancel').focus(), 0);
  });
}

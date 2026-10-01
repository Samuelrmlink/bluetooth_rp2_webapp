// Copying to the system clipboard, from a page served over plain http.
//
// navigator.clipboard exists only in a "secure context" (https, or
// localhost), and the gateway serves http on the LAN and over WireGuard, so
// it is usually absent. The fallback is the old one that still works there:
// select the text in a hidden textarea and execCommand('copy'), which the
// browser allows inside a user gesture (a key press, a click).
//
// The fallback has to borrow focus, so it gives it back -- with the
// selection it had -- before returning. Reading the clipboard has no such
// fallback over http; a paste arrives through the browser's own paste event.

export async function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch { /* fall through: permission refused, or not in a gesture */ }
  }
  const ae = document.activeElement;
  const sel = ae && typeof ae.selectionStart === 'number'
    ? [ae.selectionStart, ae.selectionEnd, ae.selectionDirection] : null;
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.cssText = 'position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;pointer-events:none';
  document.body.appendChild(ta);
  ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch { ok = false; }
  ta.remove();
  if (ae && ae !== document.body) {
    ae.focus({ preventScroll: true });
    if (sel) ae.setSelectionRange(sel[0], sel[1], sel[2]);
  }
  return ok;
}

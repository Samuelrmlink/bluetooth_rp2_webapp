// Transient confirmation for a write that reached the device.
//
// One helper so every panel says the same thing the same way. It is shown
// only after the request returned ok — the device has applied the change
// (config lands in flash on its next main-loop tick, a few ms later) — so
// "saved" here means saved, not "sent". Clears itself after a few seconds:
// a confirmation that never goes away stops being one, and would still be
// saying "saved" next to a form the user has since edited.

const HOLD_MS = 3000;
const timers = new WeakMap();

export function showSaved(el, text = '✓ Saved to device') {
  setState(el, text, 'saved-ok');
  const t = setTimeout(() => {
    // Only clear what we set — a later "unsaved changes" or "applying…"
    // written to the same element must not be wiped by an old timer.
    if (el.textContent === text) setState(el, '', '');
    timers.delete(el);
  }, HOLD_MS);
  timers.set(el, t);
}

// For the non-success states that share the element: cancels a pending
// clear so a stale success timer cannot erase a newer message.
export function setState(el, text, cls) {
  const t = timers.get(el);
  if (t) {
    clearTimeout(t);
    timers.delete(el);
  }
  el.textContent = text;
  el.className = cls || 'dim';
}

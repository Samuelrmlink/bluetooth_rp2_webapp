// Bottom-sheet behaviour for the Settings panel on a phone: drag it down by
// the grabber, let go, and it snaps to full height, half height, or away.
//
// The sibling of draggable.js (free-floating panels on a desktop) and
// splitter.js (layout handles), following the same conventions — pointer
// events, `touch-action: none` on the handle, pointer capture with
// window-level move/up as the guarantee, `pointercancel` handled.
//
// Two deliberate limits:
//
//   * The drag comes from the HANDLE only, never the sheet body. A sheet that
//     also drags from its content has to arbitrate between "scroll the Debug
//     log" and "move the sheet" on every gesture, usually by tracking whether
//     the body is scrolled to the top. Handle-only has no ambiguity, and this
//     sheet's content genuinely scrolls.
//
//   * Position, not physics. Release snaps to the nearest stop, with a flick
//     (fast flings past a velocity threshold) overriding it by one stop in the
//     direction of travel. No momentum, no rubber band — this is a settings
//     panel, and the extra state is not worth the fidelity.
//
// `enabled()` is what keeps this from fighting makeDraggable: both are bound
// to the same header for the life of the page, and each bails at pointerdown
// when the viewport isn't theirs. Binding on a media-query change instead
// would leave a half-finished drag owned by a listener that just went away.

const HALF = 0.5; // of sheet height, measured from fully open
const FLICK_PX_PER_MS = 0.5;
const SNAP_MS = 180;

// `handles` rather than one handle: the grabber is a 20px strip and the
// header above the content is the rest of the natural grab area. They must
// share ONE instance — two instances would each track their own offset and
// fight over the same transform.
export function makeSheet(panel, { handles, enabled, onDismiss } = {}) {
  let activeId = null;
  let startY = 0;
  let startOffset = 0;
  let offset = 0; // px the sheet is translated DOWN from fully open
  let height = 0;
  let lastY = 0;
  let lastT = 0;
  let velocity = 0; // px/ms, positive downward

  const setOffset = (v) => {
    offset = v;
    panel.style.transform = v ? `translateY(${v}px)` : '';
  };

  // Exposed so the owner can put the sheet back to full height when it opens,
  // and clear the transform when the viewport stops being a phone — otherwise
  // a sheet left at half height would reopen there, or worse, carry its
  // translate onto the desktop floating panel.
  const reset = () => {
    panel.classList.remove('snapping');
    setOffset(0);
  };

  const snapTo = (target) => {
    panel.classList.add('snapping');
    setOffset(target);
    setTimeout(() => panel.classList.remove('snapping'), SNAP_MS);
  };

  for (const handle of handles) {
    handle.addEventListener('pointerdown', (e) => {
      if (!e.isPrimary || e.button !== 0) return;
      if (enabled && !enabled()) return;
      if (e.target.closest('.modal-close')) return; // the close button's click must survive

      height = panel.getBoundingClientRect().height;
      if (height <= 0) return;
      activeId = e.pointerId;
      startY = e.clientY;
      startOffset = offset;
      lastY = e.clientY;
      lastT = e.timeStamp;
      velocity = 0;
      panel.classList.remove('snapping'); // a drag interrupting a snap takes over
      try {
        handle.setPointerCapture(e.pointerId);
      } catch { /* see splitter.js: capture is an optimization, not a requirement */ }
      e.preventDefault();
    });
    handle.style.touchAction = 'none';
  }

  window.addEventListener('pointermove', (e) => {
    if (e.pointerId !== activeId) return;
    const dt = e.timeStamp - lastT;
    if (dt > 0) velocity = (e.clientY - lastY) / dt;
    lastY = e.clientY;
    lastT = e.timeStamp;

    // Clamped at 0: dragging up past fully-open does nothing, rather than
    // lifting the sheet off the bottom of the screen and showing a gap.
    setOffset(Math.max(0, Math.min(startOffset + (e.clientY - startY), height)));
    e.preventDefault();
  });

  const release = (e) => {
    if (e.pointerId !== activeId) return;
    activeId = null;

    const stops = [0, height * HALF, height]; // full, half, gone
    let target;
    if (velocity > FLICK_PX_PER_MS) {
      target = stops.find((s) => s > offset + 1) ?? height;
    } else if (velocity < -FLICK_PX_PER_MS) {
      target = [...stops].reverse().find((s) => s < offset - 1) ?? 0;
    } else {
      target = stops.reduce((a, b) => (Math.abs(b - offset) < Math.abs(a - offset) ? b : a));
    }

    if (target >= height) {
      // Dismissed. Snap it out of view first so the sheet is seen to leave,
      // then hand over to the owner's close() and reset for next time.
      snapTo(height);
      setTimeout(() => {
        onDismiss?.();
        reset();
      }, SNAP_MS);
      return;
    }
    snapTo(target);
  };
  window.addEventListener('pointerup', release);
  window.addEventListener('pointercancel', release);

  return { reset };
}

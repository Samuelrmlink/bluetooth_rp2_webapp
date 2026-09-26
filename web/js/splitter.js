// Drag-to-resize splitters for the pane layout — the sibling of draggable.js
// (which moves floating panels), deliberately following the same conventions:
// a press on the handle starts the drag, a local clamp(), and the cursor set
// from JS.
//
// Pointer events, not mouse events. One code path covers mouse, touch and pen,
// which is the whole reason a finger can resize anything: the browser
// synthesizes `click` on tap but never a usable mousemove stream, so the old
// mousedown/mousemove/mouseup version was inert on a touchscreen.
//
// Two details are load-bearing on touch:
//   * `touch-action: none` on the handle. Without it the browser claims the
//     gesture for scrolling as soon as it moves, and pointermove simply stops
//     firing mid-drag. Set here rather than in CSS so the module stays
//     self-contained, matching how it already sets `cursor`.
//   * setPointerCapture, which routes every subsequent move to the handle even
//     when the finger wanders off a 22px strip, and brings `pointercancel`
//     with it — the event fired when the OS steals the gesture (a system
//     edge-swipe, an incoming call), which otherwise leaves the drag stuck on
//     forever.
//
// Move/up stay on `window` even so. Capture retargets those events to the
// handle but they still propagate up to window, so the window listeners see
// them either way — and they keep the drag alive in the one case capture
// can't cover, which is capture itself failing (it throws when the pointer is
// already gone, and for synthetic events, which is how the touch tests drive
// it). Handle-only listeners would strand the drag the moment the finger left
// the strip.
//
// The caller owns the layout maths; this just reports a delta in pixels from
// where the drag started — the primary axis as the first argument, both axes
// as the second for callers that need them. `onEnd` fires on release, which is
// where persistence belongs: a cookie write per move would be dozens per drag.
//
// Returns { destroy }. The window listeners are registered through one
// AbortController so a handle that is thrown away — the tiling layout
// rebuilds its splitters whenever the tree changes — takes its listeners
// with it instead of leaving a dead pointermove handler on window for
// every rebuild.
export function makeSplitter(handle, { axis = 'y', onStart, onDrag, onEnd } = {}) {
  let activeId = null;
  let startX = 0;
  let startY = 0;
  const ac = new AbortController();
  const opts = { signal: ac.signal };

  handle.addEventListener('pointerdown', (e) => {
    // isPrimary keeps a second finger landing on the handle mid-drag from
    // hijacking it. button 0 is the left mouse button, and also what touch
    // and pen report, so the guard costs nothing on those.
    if (!e.isPrimary || e.button !== 0) return;
    activeId = e.pointerId;
    startX = e.clientX;
    startY = e.clientY;
    // Throws NotFoundError if the pointer is no longer active by the time we
    // get here — a real possibility for a fast tap, and guaranteed for a
    // synthetic (untrusted) event. The drag still works without capture, just
    // without the stray-pointer robustness, so this must not abort the gesture.
    try {
      handle.setPointerCapture(e.pointerId);
    } catch { /* capture is an optimization, not a requirement */ }
    handle.classList.add('dragging');
    // Without this, dragging over the charts text-selects the page and the
    // browser's own drag-select fights the resize.
    document.body.style.userSelect = 'none';
    onStart?.();
    e.preventDefault();
  }, opts);

  window.addEventListener('pointermove', (e) => {
    if (e.pointerId !== activeId) return;
    const dx = e.clientX - startX;
    const dy = e.clientY - startY;
    onDrag?.(axis === 'x' ? dx : dy, { dx, dy });
    e.preventDefault();
  }, opts);

  const end = (e) => {
    if (e.pointerId !== activeId) return;
    activeId = null;
    handle.classList.remove('dragging');
    document.body.style.userSelect = '';
    onEnd?.();
  };
  window.addEventListener('pointerup', end, opts);
  window.addEventListener('pointercancel', end, opts);

  handle.style.cursor = axis === 'x' ? 'col-resize' : 'row-resize';
  handle.style.touchAction = 'none';

  return {
    destroy() {
      // A drag in flight when its handle is rebuilt must not leave the page
      // with text selection off.
      if (activeId !== null) {
        activeId = null;
        document.body.style.userSelect = '';
      }
      ac.abort();
    },
  };
}

export function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(v, hi));
}

// Drag-by-header for the floating Settings panel and the Live Values panel —
// mirrors ImGui, where every window/popup is draggable by its title bar. Pure
// position tracking, no dependency on the panel's starting layout (flex-
// centered modal or already-fixed floating panel): the first press pins it to
// `position: fixed` at its current on-screen spot, then every subsequent drag
// just moves left/top from there.
//
// Pointer events rather than mouse events, for the same reasons spelled out in
// splitter.js: one path for mouse/touch/pen, `touch-action: none` so the
// browser doesn't claim the gesture for scrolling, setPointerCapture so the
// drag survives the pointer leaving the header, and `pointercancel` so an
// interrupted gesture doesn't leave the panel stuck to the cursor.
// `enabled()` lets this share a handle with sheet.js: the Settings panel is a
// free-floating window on a desktop and a bottom sheet on a phone, and both
// behaviours stay bound for the life of the page, each bailing at pointerdown
// when the viewport isn't theirs. Rebinding on a media-query change instead
// would risk orphaning a drag mid-gesture.
export function makeDraggable(panel, handle, { enabled } = {}) {
  let activeId = null;
  let startX = 0;
  let startY = 0;
  let startLeft = 0;
  let startTop = 0;

  handle.addEventListener('pointerdown', (e) => {
    if (!e.isPrimary || e.button !== 0) return;
    if (enabled && !enabled()) return;
    // Same as sheet.js: cancelling pointerdown on a <select> keeps the
    // Android picker from opening. The device list sits in this header.
    const hit = e.target instanceof Element ? e.target : e.target.parentElement;
    if (hit?.closest('button, a, input, select, textarea, label')) return;
    const rect = panel.getBoundingClientRect();
    panel.style.position = 'fixed';
    panel.style.left = `${rect.left}px`;
    panel.style.top = `${rect.top}px`;
    panel.style.right = 'auto';
    panel.style.margin = '0';
    startLeft = rect.left;
    startTop = rect.top;
    startX = e.clientX;
    startY = e.clientY;
    activeId = e.pointerId;
    try {
      handle.setPointerCapture(e.pointerId);
    } catch { /* see splitter.js: capture is an optimization, not a requirement */ }
    // Matches splitter.js: without it, dragging selects the header text and
    // the browser's drag-select fights the move.
    document.body.style.userSelect = 'none';
    e.preventDefault();
  });

  // window, not the handle — same reasoning as splitter.js: capture retargets
  // here anyway, and this is what keeps the drag alive if capture failed.
  window.addEventListener('pointermove', (e) => {
    if (e.pointerId !== activeId) return;
    const rect = panel.getBoundingClientRect();
    // Keep at least a sliver of the header on-screen so a dragged panel
    // can never end up somewhere it can't be grabbed back from.
    const minVisible = 40;
    const left = clamp(startLeft + (e.clientX - startX), minVisible - rect.width, window.innerWidth - minVisible);
    const top = clamp(startTop + (e.clientY - startY), 0, window.innerHeight - minVisible);
    panel.style.left = `${left}px`;
    panel.style.top = `${top}px`;
    e.preventDefault();
  });

  const end = (e) => {
    if (e.pointerId !== activeId) return;
    activeId = null;
    document.body.style.userSelect = '';
  };
  window.addEventListener('pointerup', end);
  window.addEventListener('pointercancel', end);

  handle.style.cursor = 'move';
}

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(v, hi));
}

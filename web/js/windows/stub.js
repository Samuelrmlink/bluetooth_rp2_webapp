// A window that is registered but not built yet: holds its place in the
// layout, pickers and persistence while its real module is written.
export function stub(title) {
  return class {
    constructor(el) {
      el.innerHTML = '';
      const p = document.createElement('div');
      p.className = 'dim pane-empty';
      p.textContent = `${title} — coming in a later step.`;
      el.appendChild(p);
    }
  };
}

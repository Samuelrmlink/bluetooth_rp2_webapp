// A one-line hint that fades: "Copied", "Filter set". One element for the
// page, on the body, so it shows over whatever is on screen.
let el = null;
let timer = 0;

export function toast(text, ms = 1600) {
  if (!el) {
    el = document.createElement('div');
    el.className = 'toast';
    el.setAttribute('role', 'status');
    document.body.appendChild(el);
  }
  el.textContent = text;
  el.classList.add('show');
  clearTimeout(timer);
  timer = setTimeout(() => el.classList.remove('show'), ms);
}

/*
 * Does the page actually RENDER?
 *
 * Every existing webapp test checks a model in isolation, and all of them
 * passed while the app opened to an empty workspace in which nothing could be
 * selected. A model test cannot catch that: the bugs were a wrong argument
 * shape at the one call site that builds the layout, and a render method
 * nobody called. Both are only visible once a real browser has run the real
 * page.
 *
 * So this runs in chromium against the real index.html and asserts the things
 * a person would check in the first five seconds. It is deliberately about
 * STRUCTURE, not appearance -- it must not break every time a label changes.
 *
 * Loaded by tools/webtest.py, which writes the result into the DOM for the
 * headless dump to pick up.
 */
const checks = [];
const ok = (name, cond, detail = '') => checks.push({ name, pass: !!cond, detail: String(detail) });

export function run({ errors }) {
  /* --- nothing threw ---------------------------------------------------- */
  ok('no uncaught JS errors', errors.length === 0, errors.join(' | '));

  /* --- every id the code reaches for exists ----------------------------- */
  // A pane whose markup is missing makes its mount throw, which kills the
  // module and takes the whole app with it.
  const missing = [];
  for (const id of window.__wantedIds || []) {
    if (!document.getElementById(id)) missing.push(id);
  }
  ok('every element the JS looks up exists', missing.length === 0, missing.join(','));

  /* --- the workspace is not empty --------------------------------------- */
  const tiles = document.getElementById('tiles');
  ok('#tiles has content on first load', tiles && tiles.children.length > 0,
     `children=${tiles ? tiles.children.length : 'no #tiles'}`);

  /* --- panes actually moved out of the park ----------------------------- */
  const placed = [...document.querySelectorAll('#tiles section.pane')].map(s => s.id);
  ok('panes are placed into tiles, not left parked', placed.length >= 2, placed.join(','));

  /* --- no tile is stuck on the placeholder ------------------------------ */
  // "Choose a pane above" in every slot is exactly the symptom of a layout
  // built with no known panes.
  const holders = document.querySelectorAll('#tiles .tile-placeholder');
  ok('no tile is stuck showing the empty placeholder', holders.length === 0,
     `${holders.length} placeholder(s)`);

  /* --- every registered pane is reachable ------------------------------- */
  // Either it is placed, or its id appears in a picker so it CAN be placed.
  const pickers = [...document.querySelectorAll('#tiles select, #tabbar .tab')];
  const offered = new Set();
  for (const p of pickers) {
    if (p.tagName === 'SELECT') for (const o of p.options) offered.add(o.value);
    else offered.add(p.dataset.pane);
  }
  const parkIds = [...document.querySelectorAll('#pane-park section.pane')].map(s => s.id);
  const unreachable = parkIds
    .map(id => id.replace(/^pane-/, ''))
    .filter(id => !offered.has(id));
  ok('every parked pane can be selected', unreachable.length === 0, unreachable.join(','));

  /* --- selecting a pane puts it on screen ------------------------------- */
  const sel = document.querySelector('#tiles select');
  if (sel) {
    const target = [...sel.options].map(o => o.value).find(v => v && !placed.includes('pane-' + v));
    if (target) {
      sel.value = target;
      sel.dispatchEvent(new Event('change', { bubbles: true }));
      const nowPlaced = !!document.querySelector(`#tiles #pane-${CSS.escape(target)}`);
      ok(`selecting "${target}" places it`, nowPlaced);
    } else {
      ok('selecting a pane places it', true, 'nothing left to place');
    }
  } else {
    ok('a pane picker exists', false, 'no <select> inside #tiles');
  }

  return checks;
}

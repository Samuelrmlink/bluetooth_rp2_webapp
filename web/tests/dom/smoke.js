/*
 * Does the page actually RENDER?
 *
 * Every existing webapp test checked a model in isolation, and all of them
 * passed while the app opened to an empty workspace in which nothing could be
 * selected. A model test cannot catch that: the bugs were a wrong argument
 * shape at the one call site that builds the layout, and a render method
 * nobody called. Both are only visible once a real browser runs the real page.
 *
 * Two app shapes are checked, because there are two: the EXPLORER carries the
 * pane manager, and a SOLO app (/anker/, /script/) is a single pane on its own
 * page. The shared assertions apply to both; the layout ones only make sense
 * for the explorer.
 *
 * Deliberately about structure, not appearance -- it must not break every time
 * a label changes.
 */
const checks = [];
const ok = (name, cond, detail = '') => checks.push({ name, pass: !!cond, detail: String(detail) });

export function run({ errors, wantedIds = [] }) {
  /* --- shared ----------------------------------------------------------- */
  ok('no uncaught JS errors', errors.length === 0, errors.join(' | '));

  // A pane whose markup is missing makes its mount throw, which kills the
  // module and takes the whole page with it.
  const missing = wantedIds.filter((id) => !document.getElementById(id));
  ok('every element this app\'s JS looks up exists', missing.length === 0, missing.join(','));

  // The header is shared by every app and is how you get between them.
  const links = document.querySelectorAll('#appnav .applink');
  ok('the app nav rendered', links.length >= 2, `${links.length} link(s)`);
  ok('the nav marks the current app', !!document.querySelector('#appnav [aria-current="page"]'));

  const tiles = document.getElementById('tiles');
  const solo = document.getElementById('solo');

  if (solo && !tiles) {
    /* --- a solo app ----------------------------------------------------- */
    const pane = solo.querySelector('section.pane');
    ok('the app\'s pane is on the page', !!pane, pane ? pane.id : 'no section.pane in #solo');
    ok('the pane is not empty', !!pane && pane.children.length > 0,
       pane ? `${pane.children.length} child(ren)` : '');
    return checks;
  }

  /* --- the explorer ----------------------------------------------------- */
  ok('#tiles has content on first load', tiles && tiles.children.length > 0,
     `children=${tiles ? tiles.children.length : 'no #tiles'}`);

  const placed = [...document.querySelectorAll('#tiles section.pane')].map((s) => s.id);
  ok('panes are placed into tiles, not left parked', placed.length >= 2, placed.join(','));

  // "Choose a pane above" in every slot is exactly the symptom of a layout
  // built with no known panes.
  const holders = document.querySelectorAll('#tiles .tile-placeholder');
  ok('no tile is stuck showing the empty placeholder', holders.length === 0,
     `${holders.length} placeholder(s)`);

  // Every parked pane must be reachable: offered by a picker or a tab.
  const offered = new Set();
  for (const p of document.querySelectorAll('#tiles select')) {
    for (const o of p.options) offered.add(o.value);
  }
  for (const t of document.querySelectorAll('#tabbar .tab')) offered.add(t.dataset.pane);
  const unreachable = [...document.querySelectorAll('#pane-park section.pane')]
    .map((s) => s.id.replace(/^pane-/, ''))
    .filter((id) => !offered.has(id));
  ok('every parked pane can be selected', unreachable.length === 0, unreachable.join(','));

  // And selecting one must actually put it on screen.
  const sel = document.querySelector('#tiles select');
  if (sel) {
    const target = [...sel.options].map((o) => o.value)
      .find((v) => v && !placed.includes('pane-' + v));
    if (target) {
      sel.value = target;
      sel.dispatchEvent(new Event('change', { bubbles: true }));
      ok(`selecting "${target}" places it`,
         !!document.querySelector(`#tiles #pane-${CSS.escape(target)}`));
    } else {
      ok('selecting a pane places it', true, 'nothing left to place');
    }
  } else {
    ok('a pane picker exists', false, 'no <select> inside #tiles');
  }

  return checks;
}

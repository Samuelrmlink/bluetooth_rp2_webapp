// Keyboard control: one table of bindings that drives the key handler, the
// help overlay and the context banner alike, so the three can never
// disagree about what a key does.
//
// Mod is Alt. It is the one modifier a browser mostly leaves to the page:
// Ctrl and Meta combos are the browser's own (new tab, find, print...),
// and Alt+letter is free everywhere except Firefox on Linux/Windows, where
// Alt+H / Alt+S and friends open the menu bar. That is stated in the help
// overlay rather than fought; the arrows and Enter work there too.
//
// Ctrl is used by exactly one family of bindings, the special resize, and
// only ever together with Alt. Ctrl+- / Ctrl+= is the browser's zoom, so
// those are preventDefault'ed in the capture phase like everything else
// here; the numpad forms are bound as well, for a window manager that
// claims the main ones first.
//
// Letters match on e.code (KeyH...), not e.key: on macOS Alt+letter types a
// dead or special character (Alt+H = ˙) and e.key would never match. The
// punctuation keys match on e.code for the same reason, with the numpad
// equivalents accepted. `?` is the exception — it is Shift+Slash on a US
// layout and somewhere else on most others, so it matches on e.key.
//
// Contexts. Which bindings apply depends on where you are:
//   dropdown   our <select> popup is open (it takes every key)
//   help       the shortcut overlay is up (Esc / ? close it)
//   drag       a pane is being dragged (Esc cancels)
//   split-move Alt+Shift+S is HELD: the direction keys pick the window to
//              split with
//   split-side it has been released: one direction picks which half of that
//              window the pane takes
//   settings   the Settings panel is open and focus is in it or nowhere
//   popover    a pane's own settings panel is open (js/popover.js)
//   workspace  everything else
// `always` bindings apply in every context.
//
// One binding arms rather than acts, and it is the only two-phase gesture
// here: Alt+Shift+S held puts the keyboard in the 'split-move' context,
// where the direction keys walk a highlighted target; releasing Alt moves it
// to 'split-side', where one direction says which half of that window to
// take and commits. Both are contexts rather than special cases so the
// banner and the help overlay describe them the way they describe
// everything else.
//
// There is no timeout on either. Phase one ends when Alt comes up, which is
// a real event rather than a guess; phase two ends on the next keystroke,
// with the banner saying so meanwhile. Phase one swallows keys it does not
// know instead of cancelling, because the chord is still held down and a
// stray press (including the auto-repeat of the S that started it) should
// not throw the gesture away.
//
// Two layers of keys. Alt+key acts on windows: focus, move, split, close.
// Plain h/j/k/l and the arrows act INSIDE the focused window — the tile's
// header buttons and its pane's fields, the Settings panel's controls —
// through nav.js, and Space/Enter on a <select> open the dropdown popup
// (dropdown.js). Plain keys are never handled while a text field has
// focus — you are typing — except that Esc leaves the field first, so a
// second Esc reaches the app. Mod combos are handled everywhere; the one
// native thing lost is Alt+Down opening a <select>.

import { Nav, isTextField, stepSelect } from './nav.js';
import { Dropdown } from './dropdown.js';

const IS_MAC = /Mac|iPhone|iPad/.test(navigator.platform || '');

// Display strings use the key names people see. Combos are what the matcher
// reads: Mod / Shift plus an e.code, or the literal '?'.
export const BINDINGS = [
  // Workspace: focus
  { id: 'focus-left', group: 'Windows', keys: 'Alt+H / Alt+←', combos: ['Mod+KeyH', 'Mod+ArrowLeft'], when: 'workspace', label: 'Focus the window to the left (in tabbed view: the previous tab)' },
  { id: 'focus-down', group: 'Windows', keys: 'Alt+J / Alt+↓', combos: ['Mod+KeyJ', 'Mod+ArrowDown'], when: 'workspace', label: 'Focus the window below' },
  { id: 'focus-up', group: 'Windows', keys: 'Alt+K / Alt+↑', combos: ['Mod+KeyK', 'Mod+ArrowUp'], when: 'workspace', label: 'Focus the window above' },
  { id: 'focus-right', group: 'Windows', keys: 'Alt+L / Alt+→', combos: ['Mod+KeyL', 'Mod+ArrowRight'], when: 'workspace', label: 'Focus the window to the right (in tabbed view: the next tab)' },
  { id: 'cycle-next', group: 'Windows', keys: 'Alt+N', combos: ['Mod+KeyN'], when: ['workspace', 'settings', 'popover'], label: 'Next window (tiled) / next tab (tabbed), and Settings when open' },
  { id: 'cycle-prev', group: 'Windows', keys: 'Alt+Shift+N', combos: ['Mod+Shift+KeyN'], when: ['workspace', 'settings', 'popover'], label: 'Previous window (tiled) / previous tab (tabbed)' },
  // Alt+1..9. One row in the help overlay for all nine: the other eight are
  // real bindings the matcher needs, but a table listing them one per line
  // would say the same thing nine times, and a binding with no `group` is
  // not rendered (see modals/help.js).
  ...Array.from({ length: 9 }, (_, i) => ({
    id: `tab-${i + 1}`,
    ...(i === 0
      ? { group: 'Windows', keys: 'Alt+1 … Alt+9', label: 'Focus that tab (tiled: that tile, in tree order)' }
      : {}),
    combos: [`Mod+Digit${i + 1}`, `Mod+Numpad${i + 1}`],
    when: 'workspace',
  })),
  // Workspace: arrange
  { id: 'move-left', group: 'Arrange', keys: 'Alt+Shift+H / Alt+Shift+←', combos: ['Mod+Shift+KeyH', 'Mod+Shift+ArrowLeft'], when: ['workspace', 'settings', 'popover'], label: 'Swap with the pane to the left (nudge a floating window)' },
  { id: 'move-down', group: 'Arrange', keys: 'Alt+Shift+J / Alt+Shift+↓', combos: ['Mod+Shift+KeyJ', 'Mod+Shift+ArrowDown'], when: ['workspace', 'settings', 'popover'], label: 'Swap with the pane below (nudge a floating window)' },
  { id: 'move-up', group: 'Arrange', keys: 'Alt+Shift+K / Alt+Shift+↑', combos: ['Mod+Shift+KeyK', 'Mod+Shift+ArrowUp'], when: ['workspace', 'settings', 'popover'], label: 'Swap with the pane above (nudge a floating window)' },
  { id: 'move-right', group: 'Arrange', keys: 'Alt+Shift+L / Alt+Shift+→', combos: ['Mod+Shift+KeyL', 'Mod+Shift+ArrowRight'], when: ['workspace', 'settings', 'popover'], label: 'Swap with the pane to the right (nudge a floating window)' },
  { id: 'split-move-arm', group: 'Arrange', keys: 'Alt+Shift+S (hold)', combos: ['Mod+Shift+KeyS'], when: 'workspace', label: 'Move the pane INTO another window, sharing its room rather than swapping. Hold and pick the window with the direction keys; release, and the direction keys move it between that window\u2019s halves; Enter settles it' },
  // Phase one, while the chord is held. Alt+direction is accepted as well as
  // Alt+Shift+direction: Shift is easy to let go of a moment early, and
  // nothing else in this context wants those keys.
  { id: 'split-pick-left', combos: ['Mod+Shift+KeyH', 'Mod+Shift+ArrowLeft', 'Mod+KeyH', 'Mod+ArrowLeft'], when: 'split-move' },
  { id: 'split-pick-down', combos: ['Mod+Shift+KeyJ', 'Mod+Shift+ArrowDown', 'Mod+KeyJ', 'Mod+ArrowDown'], when: 'split-move' },
  { id: 'split-pick-up', combos: ['Mod+Shift+KeyK', 'Mod+Shift+ArrowUp', 'Mod+KeyK', 'Mod+ArrowUp'], when: 'split-move' },
  { id: 'split-pick-right', combos: ['Mod+Shift+KeyL', 'Mod+Shift+ArrowRight', 'Mod+KeyL', 'Mod+ArrowRight'], when: 'split-move' },
  // Phase two, after the release.
  { id: 'split-side-left', combos: ['KeyH', 'ArrowLeft'], when: 'split-side' },
  { id: 'split-side-down', combos: ['KeyJ', 'ArrowDown'], when: 'split-side' },
  { id: 'split-side-up', combos: ['KeyK', 'ArrowUp'], when: 'split-side' },
  { id: 'split-side-right', combos: ['KeyL', 'ArrowRight'], when: 'split-side' },
  { id: 'split-confirm', combos: ['Enter', 'NumpadEnter', 'Space'], when: 'split-side' },
  { id: 'shrink-x', group: 'Arrange', keys: 'Alt+-', combos: ['Mod+Minus', 'Mod+NumpadSubtract'], when: ['workspace', 'settings', 'popover'], label: 'Move the nearest vertical split left (a floating window: narrower)' },
  { id: 'grow-x', group: 'Arrange', keys: 'Alt+=', combos: ['Mod+Equal', 'Mod+NumpadAdd'], when: ['workspace', 'settings', 'popover'], label: 'Move the nearest vertical split right (a floating window: wider)' },
  { id: 'shrink-y', group: 'Arrange', keys: 'Alt+Shift+-', combos: ['Mod+Shift+Minus', 'Mod+Shift+NumpadSubtract'], when: ['workspace', 'settings', 'popover'], label: 'Move the nearest horizontal split up (a floating window: shorter)' },
  { id: 'grow-y', group: 'Arrange', keys: 'Alt+Shift+=', combos: ['Mod+Shift+Equal', 'Mod+Shift+NumpadAdd'], when: ['workspace', 'settings', 'popover'], label: 'Move the nearest horizontal split down (a floating window: taller)' },
  { id: 'edge-out-x', group: 'Arrange', keys: 'Alt+Ctrl+=', combos: ['Mod+Ctrl+Equal', 'Mod+Ctrl+NumpadAdd'], when: 'workspace', label: 'Push this pane\u2019s right edge out past the next border, taking the room from the window beyond it \u2014 this pane alone gets wider. Once per border; refused where the ordinary resize already does it' },
  { id: 'edge-in-x', group: 'Arrange', keys: 'Alt+Ctrl+-', combos: ['Mod+Ctrl+Minus', 'Mod+Ctrl+NumpadSubtract'], when: 'workspace', label: 'Pull that right edge back in, giving the room back' },
  { id: 'edge-out-y', group: 'Arrange', keys: 'Alt+Ctrl+Shift+=', combos: ['Mod+Ctrl+Shift+Equal', 'Mod+Ctrl+Shift+NumpadAdd'], when: 'workspace', label: 'The same downwards: this pane alone gets taller' },
  { id: 'edge-in-y', group: 'Arrange', keys: 'Alt+Ctrl+Shift+-', combos: ['Mod+Ctrl+Shift+Minus', 'Mod+Ctrl+Shift+NumpadSubtract'], when: 'workspace', label: 'Pull that bottom edge back in' },
  { id: 'split-h', group: 'Arrange', keys: 'Alt+Enter', combos: ['Mod+Enter', 'Mod+NumpadEnter'], when: 'workspace', label: 'Split horizontally (new tile to the right)' },
  { id: 'split-v', group: 'Arrange', keys: 'Alt+Shift+Enter', combos: ['Mod+Shift+Enter', 'Mod+Shift+NumpadEnter'], when: 'workspace', label: 'Split vertically (new tile below)' },
  { id: 'float', group: 'Arrange', keys: 'Alt+T', combos: ['Mod+KeyT'], when: 'workspace', label: 'Float the active tile / tile the active floating window' },
  { id: 'close', group: 'Arrange', keys: 'Alt+W', combos: ['Mod+KeyW'], when: ['workspace', 'settings', 'popover'], label: 'Close the active window' },
  { id: 'pick-pane', group: 'Arrange', keys: 'Alt+P', combos: ['Mod+KeyP'], when: 'workspace', label: 'Focus the pane picker of the active tile' },
  { id: 'swap-splits', group: 'Arrange', keys: 'Alt+R', combos: ['Mod+KeyR'], when: ['workspace', 'settings', 'popover'], label: 'Swap every split at once: side by side becomes top/bottom and back' },
  { id: 'toggle-tabs', group: 'Arrange', keys: 'Alt+Shift+T', combos: ['Mod+Shift+KeyT'], when: ['workspace', 'settings', 'popover'], label: 'Tabbed view: one pane at a time, with a tab bar' },
  { id: 'pane-settings', group: 'Arrange', keys: 'Alt+,', combos: ['Mod+Comma'], when: ['workspace', 'popover'], label: "Open the active pane's own settings (its gear)" },
  // Inside a window: plain keys, handled by nav.js / dropdown.js. Listed
  // here for the help overlay; `combos` is empty because the handler for
  // them is not a table lookup.
  { id: 'nav', group: 'Inside a window', keys: 'H / J / K / L / arrows', combos: [], when: 'workspace', label: 'Move between the controls of the focused window or panel (header buttons, fields, Settings)' },
  { id: 'nav-select', group: 'Inside a window', keys: 'Tab / Shift+Tab on a dropdown', combos: [], when: 'workspace', label: 'Next / previous value without opening it' },
  { id: 'nav-open', group: 'Inside a window', keys: 'Space / Enter', combos: [], when: 'workspace', label: 'Toggle a checkbox, press a button, open a dropdown list (J/K to move, Enter to pick, Esc to cancel)' },
  { id: 'nav-list', group: 'Inside a window', keys: 'Space on a list', combos: [], when: 'workspace', label: 'Enter the event list: J/K move the selection, G bottom, gg top, Esc leaves it' },
  { id: 'nav-section', group: 'Inside a window', keys: 'L / H on a heading', combos: [], when: 'workspace', label: 'Expand / collapse a Settings section' },
  { id: 'nav-esc', group: 'Inside a window', keys: 'Esc in a text field', combos: [], when: 'workspace', label: 'Leave the field' },
  // Everywhere
  { id: 'settings', group: 'Everywhere', keys: 'Alt+S', combos: ['Mod+KeyS'], when: 'always', label: 'Open / close Settings' },
  // The editor's own. Context 'editor' is the workspace with the Editor
  // window active (see WORKSPACE_LIKE below), so these act only there: F5
  // still reloads the page from any other window.
  { id: 'editor-save', group: 'Editor', keys: 'Ctrl+S', combos: ['Ctrl+KeyS'], when: 'editor', label: 'Save the file to the device' },
  { id: 'editor-run', group: 'Editor', keys: 'F5', combos: ['F5'], when: 'editor', label: 'Run the script from the buffer (a draft: nothing is written to flash)' },
  { id: 'editor-stop', group: 'Editor', keys: 'Shift+F5', combos: ['Shift+F5'], when: 'editor', label: 'Stop the script' },
  { id: 'editor-sidebar', group: 'Editor', keys: 'Alt+B', combos: ['Mod+KeyB'], when: 'editor', label: 'Show / hide the file sidebar' },
  { id: 'editor-files', group: 'Editor', keys: 'j / k, l, h', combos: [], when: 'editor', label: 'In the file list: move, open or expand, collapse' },
  { id: 'editor-to-files', group: 'Editor', keys: 'Ctrl+H', combos: ['Ctrl+KeyH'], when: 'editor', label: 'Move to the file list (opens the sidebar if it is shut)' },
  { id: 'editor-to-text', group: 'Editor', keys: 'Ctrl+L', combos: ['Ctrl+KeyL'], when: 'editor', label: 'Move back into the text' },
  { id: 'editor-vim-toggle', group: 'Editor', keys: 'VIM button', combos: [], when: 'editor', label: 'Vim keys on / off (off: the text behaves like any text box; also Settings → UI)' },
  // Vim, in normal mode (editor/vim.js). No combos: the editor's own key
  // handler takes these inside the text; listed so help can find them.
  { id: 'vim-search', group: 'Editor', keys: '/text  ?text', combos: [], when: 'editor', label: 'Vim: search forward / backward — every match highlighted' },
  { id: 'vim-search-next', group: 'Editor', keys: 'n  N', combos: [], when: 'editor', label: 'Vim: next / previous match (wraps)' },
  { id: 'vim-search-word', group: 'Editor', keys: '*  #', combos: [], when: 'editor', label: 'Vim: search for the word under the cursor, forward / backward' },
  { id: 'vim-noh', group: 'Editor', keys: ':noh', combos: [], when: 'editor', label: 'Vim: clear the search highlight' },
  { id: 'vim-write', group: 'Editor', keys: ':w  :w path', combos: [], when: 'editor', label: 'Vim: save / write a copy to another file (and stay on this one)' },
  { id: 'vim-quit', group: 'Editor', keys: ':q  :q!  ZQ', combos: [], when: 'editor', label: 'Vim: close the file (refused with unsaved changes) / close, discarding them' },
  { id: 'vim-wq', group: 'Editor', keys: ':wq  :x  ZZ', combos: [], when: 'editor', label: 'Vim: save and close (:x and ZZ only write if modified)' },
  { id: 'vim-edit', group: 'Editor', keys: ':e path  :e!', combos: [], when: 'editor', label: 'Vim: open a file (a new one if missing) / revert to what is on the device' },
  { id: 'vim-line', group: 'Editor', keys: ':12  :$', combos: [], when: 'editor', label: 'Vim: go to line 12 / the last line' },
  { id: 'vim-run', group: 'Editor', keys: ':run  :stop', combos: [], when: 'editor', label: 'Vim: run the buffer as a draft / stop the script (F5 / Shift+F5)' },
  { id: 'vim-visual', group: 'Editor', keys: 'v  V, then y d c p P', combos: [], when: 'editor', label: 'Vim: select characters / lines (or drag with the mouse or a finger), then yank, delete, change, or paste over' },
  { id: 'vim-clipboard', group: 'Editor', keys: 'y  /  Ctrl+V', combos: [], when: 'editor', label: 'Vim: a yank also copies to the system clipboard; paste (Ctrl+V or the phone\'s Paste) in normal mode inserts like p' },
  // The Scanner's: 'scanner' (the window), 'scanner-head' / 'scanner-table'
  // (the table's cursor on the header / a row), 'scanner-filter' (a text box
  // in the Filter dialog).
  { id: 'scanner-filter', group: 'Scanner', keys: '/  or  f', combos: ['Slash', 'KeyF'], when: ['scanner', 'scanner-table', 'scanner-head'], label: 'Open the Filter dialog (filters apply as you type, scanning or not)' },
  { id: 'scanner-columns', group: 'Scanner', keys: 'c', combos: ['KeyC'], when: ['scanner', 'scanner-table', 'scanner-head'], label: 'Open the Columns dialog: choose which advertisement fields are shown' },
  { id: 'scanner-clear', group: 'Scanner', keys: 'Shift+X', combos: ['Shift+KeyX'], when: ['scanner', 'scanner-table', 'scanner-head'], label: 'Clear the list (connected devices stay; the rest return as they advertise)' },
  { id: 'scanner-filter-next', group: 'Scanner', keys: 'Alt+N in a filter box', combos: ['Mod+KeyN'], when: 'scanner-filter', label: 'Next filter box' },
  { id: 'scanner-filter-prev', group: 'Scanner', keys: 'Alt+Shift+N in a filter box', combos: ['Mod+Shift+KeyN'], when: 'scanner-filter', label: 'Previous filter box' },
  { id: 'table-enter', group: 'Scanner', keys: 'Space / Enter on a table', combos: [], when: 'scanner', label: 'Into the table (any table): a cell cursor; Esc leaves' },
  { id: 'table-move', group: 'Scanner', keys: 'h j k l / arrows', combos: [], when: ['scanner-table', 'scanner-head'], label: 'Table: move between cells (the header is the top row); gg / G first / last row' },
  { id: 'table-sort', group: 'Scanner', keys: 'a  /  d', combos: [], when: ['scanner-table', 'scanner-head'], label: 'Table: sort the cursor\'s column ascending / descending (Enter on a heading cycles)' },
  { id: 'table-reorder', group: 'Scanner', keys: 'Shift+H / Shift+L on the header', combos: [], when: 'scanner-head', label: 'Table: move the column left / right (or drag its heading)' },
  { id: 'table-resize', group: 'Scanner', keys: 'Shift+H / Shift+L on a row', combos: [], when: 'scanner-table', label: 'Table: narrow / widen the column (or drag a heading\'s edge; double-click resets)' },
  { id: 'table-menu', group: 'Scanner', keys: 'Enter / Space on a cell', combos: [], when: 'scanner-table', label: 'Its menu: Connect, Show in GATT, Filter by, the whole value (tap to copy). On the Connect column: connect' },
  // The GATT window's tree (context 'gatt'; 'gatt-panel' inside an open panel).
  { id: 'gatt-move', group: 'GATT', keys: 'j / k, gg / G', combos: [], when: 'gatt', label: 'In the tree: move between services and characteristics' },
  { id: 'gatt-open', group: 'GATT', keys: 'Enter / Space', combos: [], when: 'gatt', label: 'Open or close a characteristic\'s panel (or a service)' },
  { id: 'gatt-into', group: 'GATT', keys: 'l / →', combos: [], when: 'gatt', label: 'Into an open panel\'s controls (opens it first if closed); h / ← closes, or goes to the service' },
  { id: 'gatt-back', group: 'GATT', keys: 'Esc in a panel', combos: [], when: 'gatt-panel', label: 'Back to the characteristic\'s row' },
  { id: 'gatt-uuid', group: 'GATT', keys: 'click a 128-bit UUID', combos: [], when: 'gatt', label: 'Show it whole, with Copy (or turn on UUID-128 in the bar)' },
  { id: 'gatt-events-split', group: 'GATT', keys: 'Events button', combos: [], when: 'gatt', label: 'Tabbed view: GATT Events beside or below the tree, with a resize handle' },
  { id: 'help', group: 'Everywhere', keys: '?', combos: ['?', 'Mod+Slash'], when: 'always', label: 'Show / hide this list' },
  // No combos: typing is not a binding, it goes to the filter box. Listed so
  // that the one place people look for what the keys do says so.
  { id: 'help-filter', group: 'Everywhere', keys: 'type', combos: [], when: 'help', label: 'While this list is open: filter it by any part of a key or of what it does' },
  { id: 'escape', group: 'Everywhere', keys: 'Esc', combos: ['Escape'], when: ['settings', 'help', 'drag', 'popover'], label: "Close Settings, a pane's settings or this list; cancel a drag" },
];

export const GROUPS = ['Windows', 'Arrange', 'Inside a window', 'Editor', 'Scanner', 'GATT', 'Everywhere'];

// The banner's contents per context: short, and only what applies now.
// `tiled` marks entries that make no sense in tabs mode.
export const HINTS = {
  editor: [
    { keys: 'Ctrl+S', label: 'save' },
    { keys: 'F5', label: 'run' },
    { keys: 'Ctrl+H/L', label: 'files / text' },
    { keys: '/', label: 'search' },
    { keys: ':', label: 'command' },
    { keys: 'Esc', label: 'leave the text' },
    { keys: 'Alt+HJKL', label: 'focus window' },
  ],
  workspace: [
    { keys: 'HJKL', label: 'controls' },
    { keys: 'Space', label: 'press / open' },
    { keys: 'Alt+HJKL', label: 'focus window' },
    { keys: 'Alt+N', label: 'next window' },
    { keys: 'Alt+1…9', label: 'tab' },
    { keys: 'Alt+Shift+HJKL', label: 'swap', tiled: true },
    { keys: 'Alt+Shift+S', label: 'split-move', tiled: true },
    { keys: 'Alt+-/=', label: 'move split', tiled: true },
    { keys: 'Alt+Ctrl+-/=', label: 'edge out / in', tiled: true },
    { keys: 'Alt+Enter', label: 'split', tiled: true },
    { keys: 'Alt+R', label: 'swap splits', tiled: true },
    { keys: 'Alt+Shift+T', label: 'tabs' },
    { keys: 'Alt+P', label: 'pick pane', tiled: true },
    { keys: 'Alt+T', label: 'float', tiled: true },
    { keys: 'Alt+W', label: 'close', tiled: true },
    { keys: 'Alt+S', label: 'settings' },
    { keys: '?', label: 'help' },
  ],
  'split-move': [
    { keys: 'Alt+Shift+HJKL', label: 'pick the window to split with' },
    { keys: 'release Alt', label: 'then choose which half' },
    { keys: 'Esc', label: 'cancel' },
  ],
  'split-side': [
    { keys: 'HJKL / arrows', label: 'which half of it the pane takes' },
    { keys: 'Enter', label: 'settle it there' },
    { keys: 'Esc', label: 'cancel' },
  ],
  settings: [
    { keys: 'HJKL', label: 'controls' },
    { keys: 'L / H', label: 'expand / collapse' },
    { keys: 'Space', label: 'press / open' },
    { keys: 'Alt+N', label: 'next window' },
    { keys: 'Alt+Shift+HJKL', label: 'move panel' },
    { keys: 'Esc', label: 'close' },
  ],
  popover: [
    { keys: 'HJKL', label: 'controls' },
    { keys: 'Space', label: 'press / open' },
    { keys: 'Alt+,', label: 'close' },
    { keys: 'Esc', label: 'close' },
  ],
  help: [
    { keys: 'type', label: 'filter' },
    { keys: '\u2191 / \u2193', label: 'scroll' },
    { keys: 'Esc', label: 'clear, then close' },
  ],
  drag: [{ keys: 'Esc', label: 'cancel' }],
  dropdown: [{ keys: 'J / K', label: 'move' }, { keys: 'Enter', label: 'pick' }, { keys: 'Esc', label: 'cancel' }],
};

// The Scanner is the workspace plus a way into its filters; inside one,
// Alt+N means the next box.
HINTS.gatt = [
  { keys: 'Space', label: 'into the tree' },
  { keys: 'J/K', label: 'move' },
  { keys: 'Enter', label: 'open / close' },
  { keys: 'L', label: 'into the panel' },
  { keys: 'Alt+HJKL', label: 'focus window' },
  { keys: 'Alt+N', label: 'next window' },
];
HINTS['gatt-panel'] = [
  { keys: 'HJKL', label: 'controls' },
  { keys: 'Space', label: 'press' },
  { keys: 'Esc', label: 'back to the row' },
  { keys: 'Alt+HJKL', label: 'focus window' },
];
HINTS.scanner = [{ keys: '/', label: 'filter' }, { keys: 'c', label: 'columns' }, { keys: 'Space', label: 'table' }, ...HINTS.workspace];
HINTS['scanner-table'] = [
  { keys: 'HJKL', label: 'cell' },
  { keys: 'Enter', label: 'menu' },
  { keys: 'a/d', label: 'sort' },
  { keys: 'Shift+H/L', label: 'width' },
  { keys: '/', label: 'filter' },
  { keys: 'Esc', label: 'leave' },
];
HINTS['scanner-head'] = [
  { keys: 'H/L', label: 'column' },
  { keys: 'Enter', label: 'sort' },
  { keys: 'a/d', label: 'asc / desc' },
  { keys: 'Shift+H/L', label: 'move column' },
  { keys: 'J', label: 'rows' },
  { keys: 'Esc', label: 'leave' },
];
HINTS['scanner-filter'] = [
  { keys: 'Alt+N', label: 'next filter' },
  { keys: 'Alt+Shift+N', label: 'previous filter' },
  { keys: 'Esc', label: 'leave the box (again: close)' },
];

// 'Mod+Shift+KeyH' -> { mod, shift, code }; '?' -> { key: '?' }; 'Escape' -> code.
export function parseCombo(s) {
  if (s === '?') return { key: '?' };
  const parts = s.split('+');
  const code = parts.pop();
  return {
    mod: parts.includes('Mod'),
    shift: parts.includes('Shift'),
    ctrl: parts.includes('Ctrl'),
    code,
  };
}

export function matches(e, combo) {
  const c = typeof combo === 'string' ? parseCombo(combo) : combo;
  if (e.metaKey) return false;
  if (c.key !== undefined) return e.key === c.key && !e.altKey && !e.ctrlKey;
  // Exact, both ways: a binding without Ctrl must not fire with Ctrl held,
  // or adding the first Ctrl binding would quietly double every other one.
  if (e.ctrlKey !== !!c.ctrl) return false;
  if (e.altKey !== !!c.mod) return false;
  if (e.shiftKey !== !!c.shift) return false;
  return e.code === c.code;
}

// Contexts that are the workspace with something extra: every workspace
// binding (Alt+hjkl and the rest) applies in them too, plus their own.
// ('scanner-filter' is a box in a dialog, not the workspace: only its own keys.)
const WORKSPACE_LIKE = new Set(['editor', 'scanner', 'scanner-table', 'scanner-head', 'gatt', 'gatt-panel']);

function applies(b, context) {
  const w = Array.isArray(b.when) ? b.when : [b.when];
  return w.includes('always') || w.includes(context) ||
    (WORKSPACE_LIKE.has(context) && w.includes('workspace'));
}

// A context's own bindings come before the workspace's it inherits, so one
// may take over a workspace key while it lasts (Alt+N in a Scanner filter
// box steps between the boxes, not the windows).
export function bindingsFor(context) {
  const own = (b) => (Array.isArray(b.when) ? b.when : [b.when]).includes(context);
  const all = BINDINGS.filter((b) => applies(b, context));
  return [...all.filter(own), ...all.filter((b) => !own(b))];
}

// Pressing a modifier is not "some other key" — it must not end an armed
// prefix on its way to the key it modifies.
const MODIFIER_KEYS = new Set(['Alt', 'Shift', 'Control', 'Meta', 'CapsLock']);

const KBD_KEY = 'btgw.kbd';

export class Keys {
  /*
    actions:     { [binding id]: (e) => void }
    getContext:  () => 'workspace' | 'settings' | 'help' | 'drag'
    getNavRoots: () => [element...] — what plain h/j/k/l navigate within
    isTabs:      () => boolean — hides the tiled-only hints
    hintEl:      the banner element
  */
  constructor({ actions, getContext, getNavRoots, isTabs, hintEl }) {
    this._actions = actions;
    this._escOwners = new WeakMap();    // text field -> (e) => took it?
    this._getContext = getContext;
    // An armed context that owns the keyboard ahead of everything but the
    // dropdown. `sticky` ones last until something ends them (a key released,
    // Esc); the rest own exactly the next keystroke.
    this._pending = null;
    this._pendingSticky = false;
    // Whether releasing Alt is what ends this one (a chord being held) or
    // whether it lasts until one of its own keys ends it.
    this._pendingAltUp = false;
    // Called when an armed context ends without having acted.
    this._onDisarm = null;
    this._isTabs = isTabs || (() => false);
    this._hint = hintEl;
    this._lastHint = null;
    this.dropdown = new Dropdown();
    this.nav = new Nav({ getRoots: getNavRoots || (() => []) });

    // A returning keyboard user gets the banner from the first paint; a
    // touch-only session never sets the flag and never sees it.
    let seen = false;
    try {
      seen = window.localStorage.getItem(KBD_KEY) === '1';
    } catch {
      seen = false;
    }
    if (seen) document.body.dataset.kbd = '1';

    // Capture phase, on the window: uPlot and the panels stop propagation of
    // some events on their own elements, and a shortcut must not depend on
    // which element happened to have focus.
    window.addEventListener('keydown', (e) => this._onKey(e), true);
    window.addEventListener('keyup', (e) => this._onKeyUp(e), true);
    // A gesture held with Alt can outlive the page's focus — Alt alone opens
    // the menu bar on some platforms — and a keyup that never arrives would
    // strand it armed.
    window.addEventListener('blur', () => {
      if (this._pendingAltUp) this.disarm();
    });
    document.addEventListener('focusin', () => this.refreshHint());
    document.addEventListener('focusout', () => queueMicrotask(() => this.refreshHint()));
    this.refreshHint();
  }

  get keyboardUser() {
    return document.body.dataset.kbd === '1';
  }

  _markKeyboardUser() {
    if (this.keyboardUser) return;
    document.body.dataset.kbd = '1';
    try {
      window.localStorage.setItem(KBD_KEY, '1');
    } catch {
      // Not persisting is fine; the banner still shows this session.
    }
    this.refreshHint();
  }

  // The context, with the dropdown popup on top of everything and an armed
  // prefix just under it.
  context() {
    if (this.dropdown.isOpen) return 'dropdown';
    return this._pending || this._getContext();
  }

  // Arm a context. The banner changes to say what the next key will do,
  // which is the whole reason this is a context rather than a flag.
  // `sticky` keeps it armed across keystrokes — for a gesture held down,
  // where the end is a key coming up rather than a key going down.
  arm(context, { sticky = false, altUp = false, onDisarm = null } = {}) {
    this._pending = context;
    this._pendingSticky = sticky;
    this._pendingAltUp = altUp;
    this._onDisarm = onDisarm;
    this.refreshHint();
  }

  // Ends an armed context. `acted` means a binding of its own ran, so the
  // caller has already done whatever the gesture was for and needs no
  // cleanup.
  disarm(acted = false) {
    if (!this._pending) return false;
    const cb = this._onDisarm;
    this._pending = null;
    this._pendingSticky = false;
    this._pendingAltUp = false;
    this._onDisarm = null;
    if (!acted) cb?.();
    this.refreshHint();
    return true;
  }

  // The end of a held gesture is a key coming up, not going down. Alt is the
  // one that matters: phase one's keys are Alt(+Shift)+direction, so the
  // chord is over when Alt is released, whatever happened to Shift or to the
  // letter that started it.
  _onKeyUp(e) {
    if (!this._pendingAltUp || e.key !== 'Alt') return;
    const cb = this._onDisarm;
    this._pending = null;
    this._pendingSticky = false;
    this._pendingAltUp = false;
    this._onDisarm = null;
    cb?.(true);
    this.refreshHint();
  }

  _onKey(e) {
    if (e.defaultPrevented || e.isComposing) return;
    const typing = isTextField(document.activeElement);
    // Ctrl+Alt counts too, now that the special resize uses it.
    const isAlt = e.altKey && !e.metaKey && e.key !== 'Alt';
    if (e.key === 'Tab' || e.key === '?' || isAlt) this._markKeyboardUser();

    // The dropdown popup owns the keyboard while it is up; an Alt combo
    // closes it and goes on to do its own thing.
    if (this.dropdown.isOpen) {
      if (isAlt) {
        this.dropdown.close(false);
      } else {
        if (this.dropdown.handleKey(e)) {
          e.preventDefault();
          e.stopPropagation();
        }
        this.refreshHint();
        return;
      }
    }

    // Esc in a text field leaves the field; the next Esc reaches the app.
    if (typing) {
      // A text field may own Esc for as long as it means something inside
      // it: the editor's vim layer drops from insert to normal mode on Esc,
      // and only a SECOND Esc (in normal mode) should leave the field. The
      // owner says whether it took the key; if not, the rules below apply.
      const escOwner = e.key === 'Escape' && this._escOwners.get(document.activeElement);
      if (escOwner && escOwner(e)) {
        e.preventDefault();
        e.stopPropagation();
        this.refreshHint();
        return;
      }
      // Esc in a text field leaves the field; the next Esc reaches the app.
      // Not in the help overlay: there the field is a filter over the list
      // it sits above, nothing else is focusable, and Esc means "clear it,
      // then close" — which is the app's business, so the key goes through.
      if (e.key === 'Escape' && this.context() !== 'help') {
        document.activeElement.blur();
        e.preventDefault();
        this.refreshHint();
        return;
      }
      // Ctrl-combos and function keys reach the bindings from inside a text
      // field ONLY when one is bound in this context -- Ctrl+S saves and F5
      // runs in the editor, while Ctrl+C/V/Z and every unbound key stay the
      // browser's, untouched.
      const shortcut = (e.ctrlKey || /^F\d{1,2}$/.test(e.key)) &&
        bindingsFor(this.context()).some((b) => b.combos.some((c) => matches(e, c)));
      if (e.key !== 'Escape' && !isAlt && !shortcut) return;
    }

    // Tab / Shift+Tab on a <select> step its value in place: the direction
    // keys are for moving on, and the native dropdown is a window we cannot
    // see into. Leaving a <select> is h/j/k/l like everything else.
    if (e.key === 'Tab' && !e.altKey && !e.ctrlKey && !e.metaKey && document.activeElement?.tagName === 'SELECT') {
      stepSelect(document.activeElement, e.shiftKey ? -1 : 1);
      e.preventDefault();
      e.stopPropagation();
      this.refreshHint();
      return;
    }

    // An armed context owns the keyboard — except a bare modifier, which is
    // just a finger still on its way to the real key.
    if (this._pending && !MODIFIER_KEYS.has(e.key)) {
      const armed = this._pending;
      const sticky = this._pendingSticky;
      let ran = false;
      for (const b of bindingsFor(armed)) {
        if (!b.combos.some((c) => matches(e, c))) continue;
        const fn = this._actions[b.id];
        if (!fn) continue;
        if (!sticky) this.disarm(true);
        fn(e, b);
        ran = true;
        break;
      }
      if (ran || (sticky && e.key !== 'Escape')) {
        // A sticky gesture swallows what it does not know rather than
        // cancelling on it: the chord is still held, and the auto-repeat of
        // the key that started it arrives here every few tens of ms.
        e.preventDefault();
        e.stopPropagation();
        this.refreshHint();
        return;
      }
      // Not one of its keys: the mode is off and the key goes on to mean
      // whatever it normally means (so Esc simply cancels).
      this.disarm();
    }

    const context = this.context();
    let handled = false;
    for (const b of bindingsFor(context)) {
      if (!b.combos.some((c) => matches(e, c))) continue;
      const fn = this._actions[b.id];
      if (!fn) continue;
      fn(e, b);
      handled = true;
      break;
    }
    // Plain keys inside the focused window: h/j/k/l and arrows move between
    // controls, Space/Enter on a <select> open the popup, lists take their
    // own keys (Shift is allowed through for G). Outside text fields only
    // (already returned above).
    const plain = !e.altKey && !e.ctrlKey && !e.metaKey;
    // Never the in-window navigation from inside a text field: a key that
    // got this far there was a bound shortcut, not a direction.
    if (!handled && plain && context !== 'drag' && !typing) {
      const ae = document.activeElement;
      if ((e.code === 'Space' || e.code === 'Enter') && !e.shiftKey && ae?.tagName === 'SELECT') {
        handled = this.dropdown.open(ae);
      } else if (this.nav.handle(e)) {
        handled = true;
        this._markKeyboardUser();
      }
    }

    // Alt+arrows navigate history / scroll in some browsers; a miss must not
    // fling the page back. Other unmatched Alt combos are left alone (the
    // Firefox menu bar, accesskeys).
    if (handled || (isAlt && e.code.startsWith('Arrow'))) e.preventDefault();
    if (handled) {
      e.stopPropagation();
      this.refreshHint();
    }
  }

  // Lets a text field take Esc before the "Esc leaves the field" rule.
  // `fn(e)` returns true when it used the key.
  ownEscape(el, fn) {
    this._escOwners.set(el, fn);
  }

  // Rebuilds the banner for the current context. Cheap enough to call from
  // every focus change and layout pass: it compares the text it would
  // render and touches the DOM only when that changed.
  refreshHint() {
    if (!this._hint) return;
    const context = this.context();
    const tabs = this._isTabs();
    const items = (HINTS[context] || []).filter((h) => !(h.tiled && tabs));
    const show = this.keyboardUser && items.length > 0;
    const sig = show ? `${context}|${tabs}` : '';
    if (sig === this._lastHint) return;
    this._lastHint = sig;
    this._hint.hidden = !show;
    if (!show) return;
    const frag = document.createDocumentFragment();
    for (const h of items) {
      const span = document.createElement('span');
      span.className = 'hint';
      const kbd = document.createElement('kbd');
      kbd.textContent = h.keys;
      span.append(kbd, ` ${h.label}`);
      frag.appendChild(span);
    }
    this._hint.replaceChildren(frag);
  }
}

export { IS_MAC };

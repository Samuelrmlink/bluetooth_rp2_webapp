# The gateway's web app

One page, served from the gateway's LittleFS, in which every UI is a window:
Editor, Scanner, GATT, Console, Hardware, and a window or more per device a
running Lua script reports on. Plain ES modules, no bundler, no framework, no
runtime library; gzip-only shipping (`tools/make_release.py`), about 136 KB for
a first load.

The window manager (tiles, splits, floating windows, tabbed view, the key
layer, Settings and Help) is the km003c web app's, copied in and cut at its
seams — see the header of `js/app.js`. Fixes to it may need making in both
projects.

## Layout

```
js/app.js                 glue: windows, layout, keys, connection, frame loop
js/{tiles,layout,keys,nav,dropdown,popover,sheet,splitter,draggable}.js
                          the window manager (from km003c)
js/wsapi.js  js/bus.js    the WebSocket API; per-owner event subscriptions
js/fs.js                  the /api/fs file client
js/launchers.js           toolbar buttons for another web app
js/windows/*.js           the built-in windows
js/settings/*.js          Settings sections (UI, WiFi, WireGuard, Bluetooth, Firmware)
js/editor/                the editor: core (textarea over highlight), highlight,
                          filetree, status markers, vim (a pure reducer), history
js/controls/              the control kit and pending/confirm state
js/devices/               device plugins: registry, host (lifecycle), windows,
                          and one directory per plugin
```

## Another web app

A separate page registers a button in the top bar without adding a file to
this app. Put `app.json` in its own folder:

```json
{ "label": "Printer" }
```

`apps/<id>/app.json` on LittleFS opens `/apps/<id>/`. The same file under
`apps/<id>/` on the card opens `/sd/apps/<id>/`. `app.json.gz` is used when
the plain file is absent. `label` is the button text; `title` means the same
thing. The link is the folder, not a field in the file. A folder with no
manifest is left off the bar.

Code that already runs in this page, such as `js/ext/<name>/index.js`, can
call `app.addLauncher({id, label, href})` or `{id, label, onClick}` instead
of `addWindow`. `href` is a path on this device. `onClick` is used only when
there is no `href`. `app.removeLauncher(id)` takes the button away.

## A window

A class, `constructor(el, ctx)` — `ctx` has `app`, `ws`, and `bus` (a scope:
subscribe through it and everything is dropped with it). Optional:
`render(t)` (called every frame while on screen), `onShow()` / `onHide()`,
`onConnect()`, `attachKeys(keys)`, and `context()` to claim a key context
while active (the editor claims `editor`). Register built-ins in
`js/windows/index.js`.

## GATT

**GATT** lists a connected device's services and characteristics. Each characteristic opens (click,
or Enter) into a panel with only what it permits — Read, Notify / Indicate toggles, a write box (with a
"with response" choice only when both kinds of write are allowed) — plus its permissions, its last
value, and a format: Hex, Decimal (one number per byte, `100 0 255`) or String (`\xNN` for other
bytes). The format is used both to show values and to type them, and is remembered per
service/characteristic UUID in the browser. 16-bit UUIDs always show, with names when known;
128-bit ones show as a name or abbreviated — hover, or click for the whole UUID and Copy — or in full
with **UUID-128**. The bar shows the connection's negotiated MTU. The **preferred MTU** — the most a
connection asks for; each device's answer decides what its link gets — is in Settings → Bluetooth
(`bt.mtu`, firmware 1.84+, 23–255 on this build, kept in `cfg/bt.json`; raising it back to the maximum
takes a reboot, a BTstack limit).

**GATT Events** is every value, write and failure from the shared log (`js/gattlog.js`). Its table
behaves exactly like the Scanner's (same grid, Columns dialog and cell menu, from `js/controls/`), with
optional columns for the address, service, full UUID, raw hex and notes. **Search** (`/`) marks
matches in every shown column; `n` / Shift+N (or the dialog's ◀ Prev / Next ▶, or Enter / Shift+Enter
in the box) step through them, moving the cell cursor; "Show only matching rows" turns it into a
filter. A cell's menu copies its value or the row, or searches for it. In tabbed
view, where it cannot sit beside GATT, GATT's **Events** button splits it into the GATT window
instead, side by side or stacked, with a resize handle.

## Tables

Every table (Scanner, GATT Events, Unclassified) is built with `js/controls/grid.js`
(rules in `gridmodel.js`): click a heading to sort ascending, descending, then back to the
default; drag a heading sideways to move its column; drag its right edge to resize (double-click
resets; down to 24 px). A trailing spacer takes the leftover width, so columns keep exactly the
width they were given, left-aligned. Order, widths, sort and hidden columns are remembered per
table in `btgw.prefs.tables`. Keyboard: Space/Enter on a table enters it; h/j/k/l move a cell
cursor (the header is the top row); `a` / `d` sort the cursor's column; Shift+H/L moves the column
(on the header) or narrows/widens it (on a row).

## Scanner

**Filter** (`/` or `f`): name or address, manufacturer-data prefix, service UUID, a minimum-RSSI
slider, and **Expire** — while scanning, devices not heard for that long are removed (connected
ones stay). Filters apply in the browser as you type, scanning or not, and are remembered;
**Clear filters** resets them. Alt+N / Alt+Shift+N step between the text boxes.
**Columns** (`c`): any advertisement field — names (complete / shortened), flags, appearance,
manufacturer code and data, 16/32/128-bit service lists (complete / incomplete), service data,
TX power, and the raw advertisement and scan response — parsed in the browser (`js/adparse.js`)
from the raw payloads the firmware sends (1.85+). **Clear list** (Shift+X) empties the table; devices
return as they advertise. Clicking a cell (or Enter on it) opens its menu: Connect / Disconnect,
Show in GATT, Filter by (a service, a name, a company), and the whole value — tap to copy.
`scan.list` is paged (`offset`, `limit` → `total`, `next`), so a busy room is listed in full.

## Adding a device plugin

A device appears when a running script publishes `profile.<name>.telemetry`
with an `addr` (and, for families, a `product_code`). To give it controls:

1. Add a registry entry in `js/devices/registry.js`: `id`, `match(topic, data)`,
   `name(data)`, `module: './<id>/plugin.js'`.
2. Create `js/devices/<id>/plugin.js` exporting:

   ```js
   export const controls = [
     { kind: 'readout', field: 'battery_pct', label: 'Battery', unit: '%' },
     { kind: 'switch',  field: 'car_output_enabled', label: 'Car output', verified: true },
     { kind: 'select',  field: 'mode', label: 'Mode', options: [[0, 'Off'], [1, 'On']],
       blocked: 'setter payload not yet captured' },
   ];
   export async function write(ctx, control, value) {       // optional
     const r = await ctx.ws.call('<driver op>', { device: ctx.device.addr, field: control.field, value });
     if (r && r.error) throw new Error(r.message || r.error);  // script ops report refusal as a result
   }
   export const css = 'plugin.css';                          // optional, linked while mounted
   export const windows = { /* kind: WindowClass */ };      // optional extra windows
   ```

   Kinds: `switch`, `select`, `number`, `slider`, `readout`, `button`. A control
   with `verified: false` or `blocked: '<why>'` is shown disabled with that
   reason; say which, honestly, until a change has been seen on hardware.

Every device gets **Controls** (the list above) and **Unclassified** (every
telemetry field no control claims). Nothing is fetched until a window opens;
closing the last one removes its DOM, listeners and stylesheet. A device with
no plugin still gets a telemetry window.

The Anker driver's writes go through `anker.set`, which accepts only the
setters in the PEX module (`pex/modules/drivers/anker_models.c`) — the ones
whose payloads were captured from the official app.
The A2687 charger's setters (ports, timers, charging modes, protocols, screen)
and the `anker.setters` op that lists them are in `docs/a2687-protocol.md`.

## The editor

Ctrl+S saves the file to LittleFS; F5 runs the buffer as a **draft** (RAM only,
flash untouched); Shift+F5 stops it; Stage / Commit / Revert are under `⋯`.
Markers: grey (on the device, unmodified), bright with `●` (unsaved), green `L`
(the Lua arena is running this file and it matches flash), amber `L` (running,
but the file has changed since). The arena state comes from the firmware's
`script.loaded` (1.80+).

Vim keys — toggle with the **VIM** button in the title bar or in Settings → UI
(off by default on a touch-first device; off, the text is an ordinary text box):
normal, insert, visual, visual line; `hjkl w b e 0 ^ $ gg G` with counts;
`d c y` + motion, `dd cc yy D C Y x X p P J`; `i a I A o O`; `u` / Ctrl+R.
Visual (`v`, `V`, or drag with the mouse or a finger): `y d x c`, and `p`/`P` paste over the selection.
Search: `/text` `?text` (every match highlighted as you type), `n N`, `*` `#` (word under the cursor), `:noh`.
Commands: `:w`, `:w path` (a copy; stays on this file), `:q` (refused with unsaved changes), `:q!`,
`:wq`, `:x`/`ZZ`, `ZQ` (these close the tab), `:e path` (a new tab; a new file if it does not exist), `:e!` (revert), `:12`, `:$`,
`:run`, `:stop`. Ctrl+H moves to the file list, Ctrl+L back to the text. Esc: insert → normal →
leave the text → Alt+hjkl between windows.

### Editor keys

The editor has its **own key list** — the `?` button in its title bar, or Alt+/
(and `?` outside the text) while the editor is the active window; from any
other window those keys open the main list, which has one row pointing here.
Both are generated from `js/keys.js` (`group: 'Editor'`, by `section`), so they
cannot drift from what the keys do. In short:

| Keys | What |
|---|---|
| Ctrl+S · F5 · Shift+F5 | save · run the buffer as a draft · stop |
| Alt+] · Alt+[ | next · previous tab (in the focused half) |
| Alt+Shift+] · Alt+Shift+[ | move the tab right · left |
| Alt+Q | close the tab (asks if unsaved; also its ×, or middle-click) |
| Alt+\ · Alt+Shift+\ | split / unsplit · side by side ↔ stacked |
| Alt+O · Alt+Shift+O | the other half · move the tab there |
| Alt+B · Ctrl+H · Ctrl+L | sidebar · into the file list · back to the text |
| m / Shift+F10 / Menu (file list) | the row's menu (also right-click, long press, ⋯) |
| vim `gt` `gT` `3gt` | next · previous tab · tab 3 |
| vim `:tabnew p` `:tabc` `:sp p` `:vs p` `:only` | new tab · close it · split stacked · side by side · one half |

All Alt combinations nothing else binds — Ctrl+Tab, Ctrl+W and Ctrl+PageUp/Down
are the browser's and a page cannot have them. Tabs keep their own cursor,
scroll and undo; tabs and the split survive a reload.

Drag and drop: files or folders from the computer onto a folder row upload
into it (onto a file row: beside it; asks before replacing anything); a `.tar`
dropped by itself is extracted there after a preview; a row dropped on a folder
row moves there; a row dragged out of the browser saves it (a folder as
`.tar`; Chrome / Edge). Mouse only — on a touch screen a long press is the menu.

Clipboard: a yank also copies to the system clipboard; text copied elsewhere comes in with a normal
paste (Ctrl+V, the phone's Paste), which in normal mode inserts like `p`. The page cannot read the
clipboard on demand: the gateway serves plain http, which is not a secure context (`js/clipboard.js`).

The editor keeps its own undo history, not the textarea's: Chrome ends a typing
undo group whenever any text on the page changes between keystrokes, which a
live highlight layer does by design (`js/editor/history.js` has the measurements).

## Developing

```sh
python3 web/dev/devserver.py --port 8093           # mock device on /ws and /api/fs
#   open http://127.0.0.1:8093/
#   or drive the real gateway from here: http://127.0.0.1:8093/?host=10.255.255.23
cd web && npm run test:unit                          # node:test, DOM-free modules
cd web && npm install && npx playwright test         # end to end against the mock
python3 tools/run_tests.py --tier 2                  # everything, including the above
python3 tools/make_release.py && \
  python3 tools/upload_fw_assets.py --base http://10.255.255.23 --sync   # ship
```

The mock (`dev/devserver.py`) is one device shared by every tab, as a gateway
is: an in-memory LittleFS seeded from `lua/`, a Lua runtime that reports
`script.loaded`, and a C1000 streaming telemetry whose `anker.set` accepts the
same setters as the driver. `test.reset` (mock only) restores it.

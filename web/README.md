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
js/windows/*.js           the built-in windows
js/settings/*.js          Settings sections (UI, WiFi, WireGuard, Bluetooth, Firmware)
js/editor/                the editor: core (textarea over highlight), highlight,
                          filetree, status markers, vim (a pure reducer), history
js/controls/              the control kit and pending/confirm state
js/devices/               device plugins: registry, host (lifecycle), windows,
                          and one directory per plugin
```

## A window

A class, `constructor(el, ctx)` — `ctx` has `app`, `ws`, and `bus` (a scope:
subscribe through it and everything is dropped with it). Optional:
`render(t)` (called every frame while on screen), `onShow()` / `onHide()`,
`onConnect()`, `attachKeys(keys)`, and `context()` to claim a key context
while active (the editor claims `editor`). Register built-ins in
`js/windows/index.js`.

## Tables

Every table (Scanner, GATT value log, Unclassified) is built with
`js/controls/grid.js`: click a heading to sort ascending, descending, then back
to the table's default; drag a heading's right edge to resize (double-click it
to reset). Widths and sort are remembered per table in `btgw.prefs.tables`.
A window sorts its rows with `grid.sort(rows)` before drawing them.

The Scanner's filter boxes filter what has been heard, in the browser, as you
type -- scanning or not. `/` jumps to them; inside one, Alt+N / Alt+Shift+N
step between them (elsewhere those keys still cycle windows).

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
setters listed in the model file (`lua/profiles/anker/models/A1761.lua`,
`setters`) — the ones whose payloads were captured from the official app.

## The editor

Ctrl+S saves the file to LittleFS; F5 runs the buffer as a **draft** (RAM only,
flash untouched); Shift+F5 stops it; Stage / Commit / Revert are under `⋯`.
Markers: grey (on the device, unmodified), bright with `●` (unsaved), green `L`
(the Lua arena is running this file and it matches flash), amber `L` (running,
but the file has changed since). The arena state comes from the firmware's
`script.loaded` (1.80+).

Vim keys (toggle in Settings → UI): normal, insert, visual, visual line;
`hjkl w b e 0 ^ $ gg G` with counts; `d c y` + motion, `dd cc yy D C Y x X p P J`;
`i a I A o O`; `u` / Ctrl+R; `:w :q :wq :<line>`; `/pat n N`. Esc: insert →
normal → leave the text → Alt+hjkl between windows.

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

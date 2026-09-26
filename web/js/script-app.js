/*
 * The Script app: Lua edited on the device, on its own page.
 *
 * Its own page on purpose. Editing is the one thing here you do for minutes
 * at a time, and it wants the whole window -- and a reload while debugging a
 * script should not disturb a scan or drop a charger session running in
 * another tab.
 */
import { WsApi } from './wsapi.js';
import { mountHeader } from './appnav.js';
import { mountScriptPane } from './panes/script.js';

const api = new WsApi();
const log = (kind, text) => console.log(kind, text);

mountHeader(api, 'script');
const pane = mountScriptPane({ api, log });
api.on('_open', () => pane.refresh());
api.connect();

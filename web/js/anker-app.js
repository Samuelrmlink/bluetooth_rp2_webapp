/*
 * The Anker app. One page, one job.
 *
 * It carries no pane manager and no GATT browser: everything it draws comes
 * from profile.anker.telemetry, an event a Lua script on the device emits.
 * That is the claim the whole architecture rests on, and it is easier to see
 * here than buried in a tab of a general-purpose tool.
 */
import { WsApi } from './wsapi.js';
import { mountHeader } from './appnav.js';
import { mountAnkerPane } from './panes/anker.js';

const api = new WsApi();
const log = (kind, text) => console.log(kind, text);

mountHeader(api, 'anker');
mountAnkerPane({ api, log });
api.connect();

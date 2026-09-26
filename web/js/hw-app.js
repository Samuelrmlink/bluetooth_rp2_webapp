/*
 * The Hardware app: the board itself, rather than something attached over
 * the radio.
 */
import { WsApi } from './wsapi.js';
import { mountHeader } from './appnav.js';
import { mountHwPane } from './panes/hw.js';

const api = new WsApi();
const log = (kind, text) => console.log(kind, text);

mountHeader(api, 'hw');
mountHwPane({ api, log });
api.connect();

/*
 * The Pinecil app. Telemetry and setpoint control for a soldering iron the
 * firmware has never heard of.
 */
import { WsApi } from './wsapi.js';
import { mountHeader } from './appnav.js';
import { mountPinecilPane } from './panes/pinecil.js';

const api = new WsApi();
const log = (kind, text) => console.log(kind, text);

mountHeader(api, 'pinecil');
mountPinecilPane({ api, log });
api.connect();

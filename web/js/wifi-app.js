/*
 * The WiFi app: scan, join, forget, and choose what the radio does.
 */
import { WsApi } from './wsapi.js';
import { mountHeader } from './appnav.js';
import { mountWifiPane } from './panes/wifi.js';

const api = new WsApi();
const log = (kind, text) => console.log(kind, text);

mountHeader(api, 'wifi');
mountWifiPane({ api, log });
api.connect();

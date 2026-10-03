// Which plugin handles which device, decided from the telemetry a running
// script publishes. Small and always loaded; the plugins themselves are
// fetched only when one of their windows is opened.
//
// A device with no plugin still gets an Unclassified window ("generic"), so
// the telemetry of a brand-new Lua profile is visible the moment it runs.

const PPS = new Set(['A1761', 'A1753', 'A1770', 'A1771']);   // C1000 family, one base layout
const SCP = new Set(['A2687']);

export const PLUGINS = [
  {
    id: 'anker-pps',
    match: (topic, d) => topic === 'profile.anker.telemetry' && PPS.has(d.product_code),
    // "Anker SOLIX C1000(X) Portable Power Station" or "SOLIX C1000(X) ..." -> "C1000"
    name: (d) => (d.model_name || 'Anker PPS').replace(/^(Anker )?SOLIX /, '').replace(/\(X\).*| Portable.*/, '').trim(),
    module: './anker-pps/plugin.js',
  },
  {
    id: 'anker-scp',
    match: (topic, d) => topic === 'profile.anker.telemetry' && SCP.has(d.product_code),
    // "Anker Prime 160W 3-Port GaN Charger (smart display)" is too long for a
    // tab; the model code is what it is called on the bench.
    name: (d) => d.product_code || 'Anker charger',
    module: './anker-scp/plugin.js',
    windows: [
      { kind: 'controls', title: 'Controls' },
      { kind: 'unclassified', title: 'All Data' },
      { kind: 'screensaver', title: 'Screensaver' },
    ],
  },
  {
    id: 'pinecil',
    match: (topic) => topic === 'profile.pinecil.telemetry',
    name: () => 'Pinecil',
    module: './pinecil/plugin.js',
  },
];

export const GENERIC = {
  id: 'generic',
  match: () => true,
  name: (d, topic) => d.model_name || topic.replace(/^profile\.|\.telemetry$/g, ''),
  module: null,
  windows: [{ kind: 'unclassified', title: 'All Data' }],
};

export function pluginFor(topic, d) {
  return PLUGINS.find((p) => p.match(topic, d)) || GENERIC;
}

// A stable key for a device: its address without separators when it has
// one, else the telemetry topic (a script that publishes no address).
export function deviceKey(topic, d) {
  if (typeof d.addr === 'string' && d.addr) return d.addr.replace(/[^A-Za-z0-9]/g, '').toUpperCase();
  return topic.replace(/^profile\./, '').replace(/[^A-Za-z0-9]/g, '-');
}

// 'unclassified' is the All Data window's kind (and so part of its id in a
// saved layout): it began as the fields no control claimed, and still
// offers that view ("Unclassified only").
export const DEFAULT_WINDOWS = [
  { kind: 'controls', title: 'Controls' },
  { kind: 'unclassified', title: 'All Data' },
];

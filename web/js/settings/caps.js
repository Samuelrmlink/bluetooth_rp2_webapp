// Which Settings sections a board can use. The source is that board's
// sys.info: `modules` is the op registry (ops_modules_json on the Pico, the
// same names on the C5) and `transports` is the radios. A section with no
// rule is the page itself, or something every image has. Until a sys.info
// has arrived, a ruled section stays hidden.

const ALWAYS = new Set(['sect-ui', 'sect-system']);

// A section is shown when any listed module or transport is present.
const RULES = {
  'sect-wifi': { modules: ['wifi'] },
  'sect-nap': { transports: ['bnep'] },
  'sect-ethernet': { modules: ['eth'] },
  'sect-uplinks': { modules: ['uplink'] },
  'sect-wireguard': { modules: ['wg'] },
  'sect-bluetooth': { modules: ['bt'], transports: ['le', 'classic', 'bnep'] },
  'sect-sd': { modules: ['sd'] },
  'sect-firmware': { modules: ['ota'] },
  'sect-startup': { modules: ['launch'] },
};

function has(list, want) {
  if (!Array.isArray(list) || !want) return false;
  return want.some((name) => list.includes(name));
}

export function sectionShown(id, info) {
  if (ALWAYS.has(id)) return true;
  const rule = RULES[id];
  if (!rule) return true;
  if (!info) return false;
  return has(info.modules, rule.modules) || has(info.transports, rule.transports);
}

// The firmware op namespace is "script". The page's dev server still reports
// "lua". Either one means the Lua controls in System apply to that board.
export function luaShown(info) {
  return has(info?.modules, ['script', 'lua']);
}

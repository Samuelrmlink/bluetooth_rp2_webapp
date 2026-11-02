// Which tool windows a discovered board gets, from the sys.info it already
// sends. modules is the op registry. transports is the radios. There is no
// separate capability list.

export const LOCAL_TOOLS = [
  ['editor', 'Editor'],
  ['scanner', 'Scanner'],
  ['gatt', 'GATT'],
  ['gatt-events', 'GATT Events'],
  ['console', 'Console'],
  ['hardware', 'Hardware'],
];

export function toolGroup(name) {
  return `Tools [${name}]`;
}

export function roleTitle(role, name) {
  return `${role} [${name}]`;
}

// The local Tools heading does not name a board, so the board goes on the
// row. It is left off while no remote window is registered.
export function localToolTitle(role, hasRemote) {
  return hasRemote ? `${role} [Local]` : role;
}

// DOM ids (dialogs, remembered table layout). An mDNS name is a hostname;
// anything else is flattened so it cannot break an id.
export function scopeOf(name) {
  return `@${String(name).replace(/[^A-Za-z0-9-]/g, '_')}`;
}

function has(list, name) {
  return Array.isArray(list) && list.includes(name);
}

// Scanner, GATT, and GATT Events are the LE radio. Classic alone does not
// get them. A bt module that names no transports is an older image, which
// did not split the radios, so the LE windows stay.
export function bleSupported(info) {
  if (!info) return false;
  if (has(info.transports, 'le')) return true;
  return has(info.modules, 'bt') && !Array.isArray(info.transports);
}

export function hardwareSupported(info) {
  return has(info?.modules, 'io');
}

// Editor and Console belong to every board. The rest wait until sys.info
// says the board can run them. A missing info therefore adds only those two.
export function peerRoles(info) {
  const roles = [
    { id: 'editor', title: 'Editor' },
    { id: 'console', title: 'Console' },
  ];
  if (bleSupported(info)) {
    roles.push(
      { id: 'scanner', title: 'Scanner' },
      { id: 'gatt', title: 'GATT' },
      { id: 'gatt-events', title: 'GATT Events' },
    );
  }
  if (hardwareSupported(info)) roles.push({ id: 'hardware', title: 'Hardware' });
  return roles;
}

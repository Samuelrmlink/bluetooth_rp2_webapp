// The built-in windows. Device windows are not listed here: they come and go
// with devices (devices/host.js).
import { ConsoleWindow } from './console.js';
import { stub } from './stub.js';

export const BUILTIN_WINDOWS = [
  { id: 'editor', title: 'Editor', group: 'Tools', create: stub('Editor') },
  { id: 'scanner', title: 'Scanner', group: 'Tools', create: stub('Scanner') },
  { id: 'gatt', title: 'GATT', group: 'Tools', create: stub('GATT explorer') },
  { id: 'console', title: 'Console', group: 'Tools', create: ConsoleWindow },
  { id: 'hardware', title: 'Hardware', group: 'Tools', create: stub('Hardware') },
];

// First visit: the editor beside the scanner over the console.
export const DEFAULT_TREE = ['row', 0.6, 'editor', ['col', 0.55, 'scanner', 'console']];

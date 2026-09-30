// The built-in windows. Device windows are not listed here: they come and go
// with devices (devices/host.js).
import { ConsoleWindow } from './console.js';
import { ScannerWindow } from './scanner.js';
import { GattWindow } from './gatt.js';
import { HardwareWindow } from './hardware.js';
import { stub } from './stub.js';

export const BUILTIN_WINDOWS = [
  { id: 'editor', title: 'Editor', group: 'Tools', create: stub('Editor') },
  { id: 'scanner', title: 'Scanner', group: 'Tools', create: ScannerWindow },
  { id: 'gatt', title: 'GATT', group: 'Tools', create: GattWindow },
  { id: 'console', title: 'Console', group: 'Tools', create: ConsoleWindow },
  { id: 'hardware', title: 'Hardware', group: 'Tools', create: HardwareWindow },
];

// First visit: the editor beside the scanner over the console.
export const DEFAULT_TREE = ['row', 0.6, 'editor', ['col', 0.55, 'scanner', 'console']];

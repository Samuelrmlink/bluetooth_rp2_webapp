// Settings sections, in the order they appear.
import { UiSection } from './ui.js';
import { WifiSection } from './wifi.js';
import { WireguardSection } from './wireguard.js';
import { BluetoothSection } from './bluetooth.js';
import { FirmwareSection } from './firmware.js';
import { SystemSection } from './system.js';

export const SETTINGS_SECTIONS = [UiSection, WifiSection, WireguardSection, BluetoothSection, FirmwareSection, SystemSection];

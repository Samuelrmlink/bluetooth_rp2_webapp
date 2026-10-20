// Settings sections, in the order they appear.
import { UiSection } from './ui.js';
import { WifiSection } from './wifi.js';
import { EthernetSection } from './ethernet.js';
import { UplinksSection } from './uplinks.js';
import { WireguardSection } from './wireguard.js';
import { BluetoothSection } from './bluetooth.js';
import { FirmwareSection } from './firmware.js';
import { SdCardSection } from './sdcard.js';
import { SystemSection } from './system.js';

export const SETTINGS_SECTIONS = [UiSection, WifiSection, EthernetSection, UplinksSection, WireguardSection, BluetoothSection, SdCardSection, FirmwareSection, SystemSection];

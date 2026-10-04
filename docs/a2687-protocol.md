# Anker A2687 (Prime 160 W, 3× USB-C, smart display) — protocol and API

Firmware **1.6.0.6** (the app offers 1.6.1.0 — not installed; it may change any of this).
Evidence: captures from the Anker app (`com.anker.charging` 3.23.0) driven over adb on 2026-10-03,
`anker_c1000_protocol/a2687-re/captures/20261003-181234/` (app log with plaintext both ways, snoop
log, screenshots of every screen in `ui/`). Earlier notes: `a2687-re/A2687-CRACKED.md`.
Every setter below was sent from the gateway and confirmed by the charger's telemetry.

## Framing (unchanged from A2687-CRACKED.md)
Session: AES-128-GCM after the encrypted negotiation. Setters go out on **FT 0x0F**; the reply is
**opcode | 0x0800** with a status byte (0 = ok). Body: `A1 01 21 · <items> · <stamp>`.
Items are `<tag> <len> <type> <value>` (len counts the type byte; types 01 u8, 02 u16, 03 u32,
04 bytes, 00 text). Stamp: `FE 04 <epoch u32 LE>` (most), `FE 05 03 <epoch>` (0x021F), or none
(0x021D).

**Is there JSON on the wire?** No. Every port tag is a type-04 byte block. The JSON seen in the app's
log is the app's own decode (`handleFetchDeviceInfoEvent {c1PowerData: {...}}`); its field names are
listed below next to ours.

## Telemetry — 0x0300 pushed ~1 Hz (0x0A00 = the same tags, polled)
| tag | our field | bytes | app's names |
|---|---|---|---|
| A5/A6/A7 | `cN_power` | 7: status u8, mV u16, mA u16, cW u16 | cPowerStatus, cVoltageData, cElectricData, cPowerData |
| AC/AD/AE | `cN_control` | 12: switch u8, timer active u8, timer s u32, remaining s u32, cable u8, vid u8 | cSwitchStatus, countdownTaskStatus, countdownTime, countdownRemainTime, cableInfo, deviceVID |
| A8 | `screen_off` | u8 0..4 = 30s / 1min / 5min / 30min / 12h | screenOffTime |
| A9 | `backlight_pct` | u8 (app: 25..100) | lcdBacklightBrightness |
| AA | `charging_protocol` | u8 0 AI / 1 Standard / 2 Custom | chargingProtocol |
| AB | `custom_power` | 8: mode no., auto-exit, c1 W, c2 W, c3 W, c1/c2/c3 protocol | customChargingProtocolPower |
| AF | `direction` | u8 0 up / 1 left / 2 down / 3 right | pinDirection |
| B0 | `language` | u8 0 en / 1 zh / 2 ja / 3 de | languageType |
| B1 | `fixed_allocation` | u8 0 C1 priority / 1 dual-laptop | fixedAllocation |
| B2 | `gyroscope` | u8 auto-rotate | gyroscopeSwitch |
| B3 | `custom_mode_number` | u8 last custom profile used (255 none) | savedCustomModeNumber |
| B4 | `identify_device` | 6× u16 VID/PID per port (65530/65531 unknown) | identifierDeviceModel |
| B5 | `connected_device` | 6× u16 brand/model per port (65535 none) | cNPortConnectedDeviceBrand/Model |
| E0 | `ability_info` | bit flags | abilityInfo (supportCustomScreenSaver, supportCustomProtocol, …) |
| E1 | `screensaver` | 10: flags (bit 7 = Screensaver Display), type, id u32, holiday id u32 | screensaverInfo |

Notes: cable 0 = 60 W ("3A-60W Max"), 1 = 100 W, 2 = 240 W, 3 = none. After a timer is cancelled the
control struct keeps the old timer/remaining values with "timer active" = 0. The gateway adds flat
fields: `cN_on`, `cN_timer_s` (0 unless a timer runs), `cN_timer_remain_s`, `cN_w`,
`screensaver_enabled`, and `protocols` (what the gateway last set — the charger does not report it).

## Setters (all verified on the bench)
| field (`anker.set`) | opcode | items | value |
|---|---|---|---|
| `c1_on` `c2_on` `c3_on` | 0x0207 | A2 u8 port 0..2 · A3 u8 | 0 / 1 (the app asks before turning a port off) |
| `c1_timer_s` … `c3_timer_s` | 0x0209 | A2 u8 port · A3 type-04 u32 LE | seconds, 0 cancels (app: 1/2/3 h or a custom h + 5-min wheel) |
| `charging_protocol` | 0x0206 | A2 u8 | 0 / `ai`, 1 / `standard` |
| `custom_mode` | 0x0206 | A2 u8 = 2 · A3 type-04 [mode][auto_exit][c1 W][c2 W][c3 W] | `{mode, auto_exit, c1_w, c2_w, c3_w}`: each 0 or 15..140 W, ≤ 160 W total. Replies status 4 then 0 (applied). |
| `fixed_allocation` | 0x0205 | A2 u8 | 0 / `c1_priority`, 1 / `laptop_mode` |
| `backlight_pct` | 0x0204 | A2 u8 | 25..100 |
| `screen_off` | 0x0203 | A2 u8 | 0..4 or `30s` `1min` `5min` `30min` `12h` |
| `direction` | 0x020B | A2 u8 | 0..3 or `up` `left` `down` `right` (fw 1.6.0.6 replies 0x0A0B; the app expects an echo and logs a false error) |
| `gyroscope` | 0x020D | A2 u8 | 0 / 1 (auto-rotate) |
| `language` | 0x0202 | A2 u8 | 0..3 or `en` `zh` `ja` `de` (reads back in ~4 s) |
| `screensaver_enabled` | 0x021F | A2 u8, stamp `FE 05 03` | 0 / 1 |
| `protocols` | 0x021D | A2 type-04 3× u16 LE, no stamp | `{c1, c2, c3}` masks: bit0 UFCS, 1 SCP, 2 PD 12V, 3 PPS 5-11V, 4 PPS 5-16V, 5 PPS 4.5-21V; at most 3 of bits 2..5. Default 59 (0x3B). |

The app re-sends its stored protocol masks every time it connects (`0x021D [0,59,59]` on
2026-10-03), so a mask set from the app sticks until changed there.

## Other commands seen
- **0x020C history** (Detailed Data screen): request `A2 u8` = 0 start / 1 more (the app sends
  "more" when the stream pauses). Replies 0x0A0C, pushed: `A2 u8` last-packet flag · `A3 u8`
  records in this packet (10, the last fewer) · `A4 u16` span in minutes (360) · `A5` 120 bytes of
  record slots. A record is 12 bytes = 3 ports × (mV u16, mA u16), `ffff ffff` = no reading;
  records are **5 minutes apart, oldest first, the last one now** — 73 for 6 h (checked against
  the app's chart: its 8 W peak at 17:00 is record 55). A full read takes ~3 s.
- **0x0200 with `F0 05 03 01000000`**: a full-report request the app sends when opening a page.
- **Cloud-only (no BLE):** "Charging Device Identification" (Test Features) =
  `POST mini_power/v1/app/setting/set_charging_device_identity_status`; custom profile names.
- **Not captured (deliberately untouched):** Restore Default Settings, Firmware Update, Delete Device;
  screensaver styles other than custom images (this unit's ability flags say it has none).

## WebSocket API (the gateway's Anker driver, `lua/profiles/anker/run.lua`)
- `anker.set {device, field, value}` → `{queued, field, value}` or `{error, message}`
  (`unknown_field`, `bad_value`, `not_this_device`). Objects/arrays (`custom_mode`, `protocols`)
  are read from the request's raw JSON. The change shows in `profile.anker.telemetry` ~1 s later.
- `anker.history {device}` → `{queued}`; then `profile.anker.history` events, one per packet:
  `{addr, id, seq, n, last, span_min, interval_s, hex}` (the packet's n×12 record bytes), and a
  final `{addr, id, done, packets, records}` or `{done, error}`. The web's History window draws it.
- `anker.setters {device}` → `{device, setters: {field: {opcode, values, min, max, names, kind,
  verified, readback}}}` — what a UI can offer.

## Lua API (`require 'profiles.anker.control'`)
```lua
local anker = require 'profiles.anker.control'
anker.set('A2687', 'c3_on', 0)                                -- queued, returns at once
local ok, why = anker.set_wait('A2687', 'backlight_pct', 60)  -- waits for the read-back
anker.set_wait('A2687', 'custom_mode', { mode = 1, auto_exit = 1, c1_w = 100, c2_w = 45, c3_w = 15 })
print(anker.get('A2687', 'c2_w'), anker.find('A2687'))
for field, rule in pairs(anker.setters('A2687')) do print(field, rule.opcode) end
```
`which` is an address, a product code, or nil for the only device. `set_wait` blocks the calling
script (never use it in an op handler).

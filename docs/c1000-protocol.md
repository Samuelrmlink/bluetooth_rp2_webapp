# Anker SOLIX C1000(X) (A1761, productType B105) — what the app sends, and what moves

App firmware shown: **v1.7.1** (OTA query 0x0830: `v0.2.9.0`, main `v1.6.6`, `A1761_low`, `A1761_mcu_low`).
Evidence: the Anker app (`com.anker.charging`) driven over adb on 2026-10-03 with nothing plugged into
the unit, every frame decrypted from the phone's HCI snoop —
`anker_c1000_protocol/c1000-re/captures/20261003-220927/` (`decoded.txt` every frame, `actions.jsonl`
each tap with the phone's clock, `ui/` a screenshot per screen with `ui/INDEX.md`).
Every setting changed was put back; the unit's full report afterwards matched the one before except
`d9` (see Screen Brightness). Older notes: `anker_c1000_protocol/anker-protocol.md` §6–§11.

## Session
The app's session with this unit is **AES-128-GCM** (`encryptType=2`), as with the A2687 — not the
CBC our earlier notes assumed — and the gateway negotiates GCM with it too. The app logs the session
key and nonce (`协商后的加密用的密钥-----<key>-----<nonce>`), so `a2687-re/snoop_decrypt.py
--mac E8:EE:CC:B6:89:7E` decrypts a whole capture. The app does not log every payload in the clear
(car output OFF was logged without one) — the snoop is the record.

On connect: 0x0001, 0x0003, 0x0029, 0x0005, 0x0021, 0x0022, 0x0027, then **0x0040 twice** (the full
report, 0x0840). After that the app never polls 0x0040 again: it lives on **0x0402**, pushed every
1–3 s, which carries a2…cf only (no d1…f9 — the settings). Opening the device page again sends
0x0041 (`getAcCarCountDownTime`); opening Settings sends 0x0030 (OTA state).

## Setters (all from the app, FT 0x0F, reply opcode | 0x0800 status 0)
Body `a1 01 21 · a2 <len> <type> <value> · fe 05 03 <epoch u32 LE>`: types 01 u8, 02 u16, 03 u32.

| app control | opcode | a2 | values seen | read back |
|---|---|---|---|---|
| AC Output button | 0x004A | u8 | 1 on, 0 off (no question asked either way) | 0x0402 `bb`; 0x0840 `d7` |
| Car Charger Output button | 0x004B | u8 | 1, 0 | 0x0402 `cc`; 0x0840 `d8` |
| AC timer (Off / 1–3Hr / Customize h + 5 min) | 0x0042 | u32 s | 3600, 600, 0 = off | `a2`, counts down |
| Car timer | 0x0043 | u32 s | 7200, 0 | `a3`, counts down |
| Brightness (light bar) Off/Low/Mid/High | 0x004F | u8 | 0, 1, 2, 3 | 0x0840 `dc`; 0x0402 `b0` and `bf` move (below) |
| Light SOS Mode switch | 0x004F | u8 | **4** on, 0 off | as above |
| Screen button | 0x0052 | u8 | 0, 1 | 0x0840 `de` |
| AC Recharging Power wheel | 0x0044 | u16 W | 500, 400 — wheel **200–1000** in 100s | 0x0840 `d1` |
| Device Timeout wheel | 0x0045 | u16 **minutes** | 30, 0 = Never, 720 — wheel 30m, 1, 2, 4, 6, 12, 24 h, Never | 0x0840 `d2` |
| Screen Timeout wheel | 0x0046 | u16 **seconds** | 20, 1800 — wheel 20s, 30s, 1, 5, 30 min | 0x0840 `d3` |
| Screen Brightness Low/Mid/High | 0x004C | u8 | 1, 2, 3 | 0x0840 `d9` |
| Temperature ℃/℉ | 0x0050 | u8 | 0 ℃, 1 ℉ | 0x0840 `dd` |
| Car Charger Output Power Saving Mode | 0x0076 | u8 | 0, 1 | not seen yet (no 0x0840 between) |
| Smart AC Output Mode | 0x0077 | u8 | 1, 0 | not seen yet |
| Output Port Memory Switch | 0x0079 | u8 | 0, 1 | not seen yet |
| Wi‑Fi page open / closed | 0x0059 | u8 | 1 on entering, 0 on leaving | — |

Corrections to older notes: 0x004C takes **1/2/3** from the app (AnkerControls.txt had 0x80 = dim,
0x01 = high — the unit held **0x80** in `d9` at the start of this session and the app showed it as
"Mid" while its picker highlighted Low; 0x80 is likely what the unit's own button sets). Light SOS is
0x004F value 4, not a separate opcode. The app's AC charge range is 200–1000 W, not 100–1300.

Help texts (the app's own): Device Timeout — "If the device is not being charged or discharged, it
will enter sleep mode and shut down after a set time". Car power saving — "below 3W for 5 consecutive
hours, car charging automatically stops". Smart AC — "if no electrical appliances are plugged into the
AC output port, the AC output will automatically turn off after 15 minutes". Output Port Memory — an
output closed by the discharge limit "will automatically reopen when the battery level recovers to
discharge limit + 10%". UltraFast Charging — "can only be turned on when charging" (greyed out here:
no AC input, so its opcode was not captured; `e5` reads it).

## Telemetry tags this capture pinned down (0x0402 unless noted)
| tag | was in A1761.lua | what it is |
|---|---|---|
| `a4` | `remaining_time_h` ×0.1 | **confirmed**: 2319 = 231.9 h = the app's "9Days 15Hr 54mins"; 0 with every output off |
| `bb` | `unknown_bb` | **AC output on** (u16 0/1), the live mirror of `d7` |
| `cc` | `car1_work_state` | **car output on** (0/1), the live mirror of `d8` |
| `b5` | `unknown_b5` | the inverter's firmware version (115), reported only while AC is on |
| `b0` | `total_output_power_w` | 1 / 2 / 3 with the light bar at Low / Mid / High, 2 in SOS, 0 off — fits a total-output W (nothing else was drawing) |
| `bf` | `bms_state_major` | 1 while the light bar was on, 0 off (discharging vs idle) |
| `bd` | `bms_temp_major_c` | °C: 22 = the app's 71 ℉ |

Not offered by the app for this unit (so not captured): AC frequency (`da` reads 60), energy-saving
mode, discharge limit, the inverter wait time (`d4` reads 60).

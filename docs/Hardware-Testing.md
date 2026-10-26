# Hardware testing

Checks that need a real device. The feature list is [Feature-Roadmap.md](Feature-Roadmap.md). A box here is ticked after a session on the hardware, with the date and what was seen. A host test, the mock page, or a reading of the source does not tick it.

Do not start the Lua Anker stack beside the PEX Anker app, and do not replace the signed Anker module, as part of one of these checks.

## Devices

| Unit | What it is | Address |
|---|---|---|
| C1000 | Anker product A1761 | `E8:EE:CC:B6:89:7E` |
| Charger | Anker product A2687 | `7C:E9:13:60:1F:D8` |
| Pinecil | Pinecil V2, IronOS | the iron on the bench |

Add a row when a session uses another unit.

## Checks

- [ ] **H1 — Anker PEX on the A2687**
  - Gates: [R14](Feature-Roadmap.md) in the feature roadmap.
  - The running driver is the Anker PEX module. The spec is `pex/modules/drivers/anker_app.c` and `anker_models.c`.
  - See: the link comes up, telemetry arrives, `anker.set` applies a value the setters list allows, `anker.history` returns records, and `anker.screensaver` runs on this model.
  - Seen:

- [ ] **H2 — Anker PEX on the C1000 (A1761)**
  - Gates: R14.
  - Same module, the other product. Screensaver is an A2687 behavior. This check is the C1000 session: link, telemetry, and a setter the A1761 table allows.
  - Seen:

- [ ] **H3 — Pinecil PEX on a Pinecil V2**
  - Gates: [R13](Feature-Roadmap.md) and R14.
  - The module polls the 56-byte live characteristic and registers `pinecil.set_temp`. The profile is `lua/profiles/pinecil.lua`.
  - See: identity (build string), live telemetry, and a setpoint write the iron accepts. A setting the profile marks unconfirmed is not a failure of this check.
  - Seen:

- [ ] **H4 — Wi-Fi recovery against hostapd**
  - Gates: [R19](Feature-Roadmap.md).
  - An access point on a Linux PC or a Raspberry Pi, run with hostapd. Drop the AP and bring it back while a BLE link is up, and again while Bluetooth is idle.
  - See: the station gets an address again without a power cycle, and the shared radio still serves the open BLE link.
  - Seen:

## New check

```
- [ ] **H# — Title**
  - Gates:
  - See:
  - Seen:
```

# Platform capabilities

What this tree runs on each chip. The C5 port plan, including the features still missing from that image, is in [Feature-Roadmap.md](Feature-Roadmap.md). The PEX target ids are in [pex-format.md](pex-format.md) section 5.

A cell is the state of this tree.

- **yes** — the images this tree builds for that chip include it.
- **some images** — a defconfig turns it on. The product-shaped Pico images leave it off.
- **option** — the code is behind a Kconfig option that defaults off.
- **specified** — the symbol exists and turning it on fails configure, because the behavior is not built.
- **absent** — the ESP-IDF project in `esp32c5/` does not include it.
- **no chip** — this silicon cannot do it.
- **no image** — this tree has no firmware defconfig for that chip. The RP2040 column is this case except where a PEX target already exists.

The Pico 2 W column is the RP2350 images under `configs/pico2w_*_defconfig`. The ESP32-C5 column is `esp32c5/`. That project is a station and an MCUmgr endpoint. It has been linked and it has not been flashed.

| Feature | Pico 2 W (RP2350) | RP2040 | ESP32-C5 |
|---|---|---|---|
| Station Wi-Fi, 2.4 GHz | yes (CYW43439) | no image | yes, one SSID compiled in |
| Station Wi-Fi, 5 GHz | no chip | no chip | radio yes; the image does not choose a band |
| SoftAP | yes, with `NET_WIFI` | no image | absent |
| USB CDC-NCM | yes, device `192.168.8.1`, host `.16` | no image | no chip |
| USB RNDIS | absent | absent | no chip |
| Console | SWD RTT; CDC-ACM off | no image | USB Serial/JTAG |
| W5500 Ethernet | some images | no image | Wi-Fi image; SPI2 GPIO8/23/24, CS 10, INT 4, RST 5 |
| WireGuard | yes | no image | absent |
| Uplink selection | yes | no image | absent |
| mDNS responder | yes | no image | absent |
| HTTP server and websocket | yes | no image | absent |
| Lua | yes, except the minimal image | no image | default off; the port still includes it |
| PEX modules | yes, target `m33` (`0x0002`) | target `m0p` (`0x0001`) only; no firmware image | required on C5 builds; target `c5` (`0x0103`) reserved, loader not built |
| SD card | some images | no image | absent |
| Config store | LittleFS | no image | NVS, OTA slot hashes only |
| BLE central | yes (BTstack on the CYW43439) | no image | absent; the chip has its own controller |
| BLE peripheral | option (`BT_PERIPHERAL`) | no image | absent |
| Classic RFCOMM | some images | no image | no chip |
| Classic NAP | some images | no image | no chip |
| MCUmgr over UDP 1337 | yes | no image | yes |
| Partition-swap OTA | yes, boot ROM, try-before-you-buy | a separate link per slot; no image | yes, on every C5 build; `ota_0` / `ota_1`, rollback; confirm does not reboot |
| Gateway LAN, NAT, peer mDNS | option | no image | specified, and absent from `esp32c5/` |
| 802.15.4 and Thread | no chip; the symbols fail configure | no chip | Kconfig choice; the Thread arm fails configure |
| CAN, TWAI | no chip | no chip | specified, and absent from `esp32c5/` |
| CAN, MCP2515 | specified | specified; no image | the RP2350 and RP2040 part |
| J1939 | specified | specified; no image | specified, and absent from `esp32c5/` |

An `m33` module loads on the RP2350 firmware. An `m0p` module loads on an RP2040 firmware. A C5 firmware, once the loader exists, accepts `c5` only. The Hazard3 ids `0x0101` and `0x0102` stay reserved for the RP2350 RISC-V mode and are not a C5 module.

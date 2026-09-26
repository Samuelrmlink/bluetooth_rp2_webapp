# Third-party code

**Every licence text referenced here is reproduced in full under `licenses/`.**
That is not decoration: BTstack, littlefs, lwIP, the pico-sdk and the cyw43
driver are all BSD-3-Clause or similar, whose clause 2 requires that binary
redistribution reproduce their copyright notice and disclaimer "in the
documentation and/or other materials provided with the distribution". Shipping
a .uf2 of this firmware is such a distribution, so those texts travel with the
repository rather than being cited from a path on someone's disk.

Everything vendored here is under a permissive licence, and each keeps its own
licence text in-tree. Nothing is copyleft, and nothing obliges a downstream user
of this project to publish their own changes.

| Component | Where | Licence | Notes |
|---|---|---|---|
| **Lua 5.4.8** (`licenses/Lua.LICENSE`) | `third_party/lua/` | MIT | Pristine upstream from lua.org. `lua.c`, `luac.c`, `liolib.c`, `loslib.c`, `loadlib.c` and `ldblib.c` are not built — see that directory's CMakeLists for why. |
| **kconfiglib** (`licenses/kconfiglib.LICENSE`) | `third_party/kconfiglib.py` | ISC | Ulf Magnusson. Used at configure time only; nothing from it ships in the firmware. |
| **littlefs v2.10** | `src/fs/littlefs/` | BSD-3-Clause | The littlefs authors. `licenses/littlefs.LICENSE` |
| **tiny-AES-c** (`licenses/tiny-AES-c.LICENSE`) | `third_party/tiny-AES-c/` | Unlicense (public domain) | kokke. **Locally patched** to make the key length a run-time argument instead of a compile-time macro, so one build serves AES-128 and AES-256; the patch is marked `LOCAL PATCH` in both files and is covered by the FIPS-197 vectors in the tier-0 suite. The Unlicense places no condition on modification or redistribution. |

Linked but not vendored, and supplied by the Raspberry Pi Pico SDK:

| Component | Licence | Text |
|---|---|---|
| pico-sdk | BSD-3-Clause | `licenses/pico-sdk.LICENSE` |
| BTstack | BlueKitchen + Raspberry Pi grant — see below | `licenses/BTstack.LICENSE`, `licenses/BTstack.LICENSE.RaspberryPi` |
| lwIP | BSD-3-Clause | `licenses/lwIP.LICENSE` |
| cyw43-driver (+ radio firmware blobs) | George Robotics + Raspberry Pi grant — see below | `licenses/cyw43-driver.LICENSE`, `licenses/cyw43-driver.LICENSE.RaspberryPi` |
| TinyUSB | MIT | `licenses/TinyUSB.LICENSE` |
| mbedTLS | Apache-2.0 | `licenses/mbedTLS.LICENSE` | 
| micro-ecc (uECC) | BSD-2-Clause | `licenses/micro-ecc.LICENSE` |

**micro-ecc is worth a line of its own.** It arrives inside BTstack, which links
it for LE Secure Connections, but `src/crypto/anker_crypto.c` now calls
`uECC_shared_secret()` and `uECC_compute_public_key()` **directly**. That makes
Kenneth MacKay's BSD-2-Clause our obligation and not merely BTstack's, so its
text is reproduced here like the rest. Clause 2 is the usual "reproduce the
notice in materials provided with the distribution", which `tools/collect_notices.sh`
satisfies.

**mbedTLS is listed but is NOT in the firmware.** The SDK builds it for
picotool only. This project deliberately does not link it: uECC was already in
the image and tiny-AES-c is about 3 KB, where mbedTLS would have been roughly
80 KB for the same four operations.

## BTstack and the cyw43 driver: the two that need reading

Both ship with a restrictive base licence AND a Raspberry Pi grant that
supersedes it for our use. Both base licences carry a clause forbidding
commercial use; **neither of those clauses applies here**, because we are using
Raspberry Pi silicon and Raspberry Pi radio hardware, which is exactly what the
grants cover.

The two grants are not scoped identically, and the difference only matters if
the hardware ever changes:

| | Base licence | Raspberry Pi grant covers |
|---|---|---|
| **cyw43-driver** | George Robotics, clause 3 non-commercial | RP2040 "or any other semiconductor device produced by the Licensor" — i.e. any Raspberry Pi **silicon**, so RP2350 is covered |
| **BTstack** | BlueKitchen, clause 4 non-commercial | Pico W, Pico WH, Pico 2 W, Pico 2 WH and RM2 — i.e. named Raspberry Pi **radio products**, plus products derived from them |

So BTstack's grant is the narrower of the two: it attaches to Raspberry Pi's
wireless boards and the RM2 module rather than to the MCU. A Pico 2 W, which is
what this firmware targets, is squarely inside both. A custom board carrying an
RM2 is also inside both. A custom board with an RP2350 and a bare CYW43439
sourced elsewhere would satisfy the cyw43 grant but would fall outside the
BTstack one, and would need a licence from BlueKitchen.

Neither grant imposes a non-commercial limit, so a commercial product built on
Raspberry Pi wireless hardware is fine.

The radio firmware blobs under `lib/cyw43-driver/firmware/` are redistributed by
Raspberry Pi under the same arrangement and are linked into the image.

*This is a reading of the licence texts, which are reproduced in full under
`licenses/`, not legal advice.*

This repository's own code is MIT (see LICENSE) and imposes nothing further.

**If you distribute a firmware binary built from this tree**, reproduce the
contents of `licenses/` alongside it. That satisfies BTstack clause 2, the same
clause in littlefs / lwIP / pico-sdk / cyw43-driver, MIT's notice requirement
for Lua and TinyUSB, and Apache-2.0's for mbedTLS.

## Not used

`~/git-clone/picocalc_lua` (BSD-3-Clause) was briefly consulted for its Pico
build of Lua. Its sources are **not** used: it patches `lauxlib.c` to read files
through FatFs, and this project vendors pristine upstream Lua instead, so
nothing here derives from it.

# tools/meshtastic — PC-side Meshtastic client (hardware-in-the-loop)

The "person on the mesh" end of the gateway's Meshtastic remote control: the PC
talks over Bluetooth LE to a **second** Meshtastic node, which sends Direct
Messages over LoRa to the gateway's node. Used for regression and HIL tests.

```
PC (this tool) --BLE--> test node ~~LoRa~~> gateway's node --BLE--> gateway (Lua) --> Anker C1000
```

## Setup (once)
```sh
python3 -m venv tools/meshtastic/.venv
tools/meshtastic/.venv/bin/pip install -r tools/meshtastic/requirements.txt
```

## Use
```sh
M=tools/meshtastic/.venv/bin/python
$M tools/meshtastic/mesh_client.py scan                    # Meshtastic nodes in range
$M tools/meshtastic/mesh_client.py pair <addr> --pin 123456
$M tools/meshtastic/mesh_client.py run <addr|name> --fifo /tmp/mesh.cmd --hops 1
echo 'dm !xxxx9d88 status' > /tmp/mesh.cmd                  # drive a running session
```
`run` logs every received text (sender, PKI-encrypted or not, hops, SNR/RSSI) and
classifies every ACK for our DMs: **delivered** (from the recipient), **implicit**
(our own node overheard a relay — not proof of delivery), **nak**, **other**.
Everything also goes to `mesh_client.log.jsonl` (JSON lines) for test scripts.

## Rules
- Never connect this tool to the **gateway's** node (`Meshtastic_9d88`,
  DC:54:75:E1:9D:89): a node takes one BLE client and the gateway must hold it.
  The tool refuses it unless `--force`.
- Keep test traffic local: hop limit 1 (the default here) on the test nodes, or
  a private channel, so tests don't spend the public mesh's airtime.
- Both nodes need each other's NodeInfo (public keys) before PKI DMs work.

## Known issue worked around
meshtastic 2.7's `BLEInterface.scan()` uses a BlueZ service-UUID discovery
filter that bleak 3 + this BlueZ reject ("No discovery started"); `connect()`
relies on it. `patch_ble_scan()` scans unfiltered and filters in Python.

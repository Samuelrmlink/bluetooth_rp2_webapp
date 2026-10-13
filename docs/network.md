# Networking: interfaces, uplink selection, mDNS groups, forwarding

What the gateway's network side is made of, how a second uplink is chosen
and switched, and the two script-facing features built on top of it.

## Interfaces

| netif | what | built when | addressing |
|---|---|---|---|
| `us` | USB-NCM (Ethernet over the USB cable) | `CONFIG_NET_USB_NCM` | 192.168.8.1/24, DHCP server for the host |
| `w0` | WiFi station | `CONFIG_NET_WIFI` | DHCP |
| `w1` | WiFi SoftAP | `CONFIG_NET_WIFI` | 192.168.4.1/24, DHCP server |
| `e0` | W5500 Ethernet on SPI | `CONFIG_NET_ETH_W5500` (default **n**) | DHCP, or manual from `cfg/eth.json` |
| `wg` | WireGuard tunnel | `CONFIG_NET_WIREGUARD` | `cfg/wireguard.json`; never the default route |
| `lo` | loopback | `CONFIG_NET_FORWARD` | 127.0.0.1 |

### The W5500 (`e0`)

Optional the way WiFi is. `configs/pico2w_eth_defconfig` is `pico2w` plus the
module on SPI1:

| W5500 | Pico 2 W |
|---|---|
| SCK | GP14 |
| MOSI | GP15 |
| MISO | GP8 |
| CS (plain GPIO) | GP13 |
| INT | GP10 (`CONFIG_ETH_W5500_INT`, -1 = poll only) |
| RST | GP11 (`CONFIG_ETH_W5500_RST`, -1 = soft reset through MR) |
| 3V3, GND | 3V3(OUT), GND |

All six pins are SYSTEM-reserved in the pin broker (`pins.map` shows them as
`reserved` / `w5500`); `gpio.*`, `spi.xfer` and scripts cannot touch them.

The chip runs in MACRAW mode: it moves frames, lwIP does ARP, IP and the
DHCP client. `src/net/w5500.c` is a non-blocking state machine, one SPI
transaction per step; `src/net/eth_w5500.c` is the lwIP glue. Received
frames are DMA'd straight into a pbuf; transmitted ones are cloned and
queued (at most 6). At boot the driver reads VERSIONR: no chip, and the
interface reports `"present": false` and nothing else is touched.

Settings -> Ethernet (shown only when the build has it): status, DHCP or
Manual (IP, mask, optional gateway and DNS -- DNS is stored for later; there
is no resolver yet). Ops: `eth.status`, `eth.config {mode, ip, mask, gw,
dns}` (validated, then applied and saved from the main loop); topic
`eth.state`.

### The SPI bus layer

`src/io/spibus.c` is the only code that touches an SPI block (a tier-0 guard
fails the build otherwise). A device claims its bus through a `semaphore_t`
(safe across cores and from an IRQ); a claim reprograms clock and mode for
that device. Transfers are chains of segments with CS held low across all
of them, moved by two DMA channels in the background. The DMA IRQ only
starts the next segment; the completion callback runs from `spibus_poll()`
in the main loop -- never in the IRQ, never inside `cyw43_wait.c` -- so lwIP
stays single-threaded. `spi.xfer` goes through it too and is refused on a
bus a driver owns (`spi.buses` lists them).

## Uplink selection

`src/net/uplink.c` decides which interface carries the WireGuard tunnel and
new outbound connections. It is the **only** caller of `netif_set_default()`.

* **Candidates**: `w0` and `e0`, whichever have a gateway. An interface
  without one is shown but neither checked nor chosen.
* **Measuring**: an ICMP echo every 2 s per candidate to the WireGuard
  endpoint (or `probe_ip` in `cfg/uplink.json`, for a server that drops
  ICMP), sent outside the tunnel from that interface's own address. RTT is
  an EWMA, jitter the EWMA of |delta RTT|, loss counted over the last 20.
  `metric = rtt + 2 * jitter + 50 ms per lost probe in the window`; three in
  a row lost = failed.
* **Fast suspicion**: if the tunnel has sent data and received nothing for
  1.5 s, the active uplink is probed at once (3 pings, 300 ms apart).
* **Policy** (Settings -> Uplinks, `uplink.policy`):
  * `best` (default): switch when another uplink's metric is 30 % lower for
    20 s, or at once when the active one fails;
  * `failover`: stay until the active one fails;
  * `fixed:<iface>`: that interface whenever it has a gateway (if it loses
    it, behave as failover rather than strand the device).
  If every probe fails everywhere the active uplink is kept: the target is
  more likely down than every link at once.
* **WiFi alone** behaves exactly as before: the station is the default
  route whenever it is enabled, never the AP, and WireGuard waits for it to
  be associated and addressed.

### Switching without loss

Make-before-break:

1. the default route moves to the new uplink (new connections);
2. the tunnel's outer UDP is pointed at the new interface (a LOCAL CHANGE in
   `third_party/wireguard-lwip`, `wireguardif_set_tx_netif`) and a keepalive
   goes out on it at once -- the server roams to the new source address on
   that first authenticated packet;
3. the old interface is left up: the tunnel's pcb is bound to ANY, so
   whatever the server already sent to the old address still arrives.

Existing non-tunnel connections stay on the interface that owns their
source address (`LWIP_HOOK_IP4_ROUTE_SRC`, `uplink_route_src`) for as long
as it is up, so a switch only steers new ones.

Ops: `uplink.status`, `uplink.policy {policy, probe_ip}`, `uplink.switch
{iface}` (switch now; the policy resumes from there); topic `uplink.state`.
Lua: `net.uplinks()`.

### The switching experiment

Procedure: a PC sends a continuous 20 Hz stream through the tunnel (ICMP to
the gateway's tunnel address, `ping -i 0.05 10.255.255.23`, counting replies
by sequence number) while (a) `uplink.switch` is forced back and forth,
(b) `best` switches on a metric change (`tc netem` on one WAN path), and
(c) one uplink is pulled (Ethernet cable out / WiFi dropped). Lost packets
and the switch time are recorded per case.

Results: see the section at the end of this file.

## mDNS record groups

The responder (lwIP's, vendored in `third_party/lwip-mdns` with one hook)
answers for the device's hostname and its services. Scripts can also
declare records for **other** names:

```lua
local id = mdns.group{ records = {
  { type='A',   name='printer.local', addr='192.168.8.50' },
  { type='PTR', name='_ipp._tcp.local', target='Printer._ipp._tcp.local' },
  { type='SRV', name='Printer._ipp._tcp.local', target='printer.local', port=631 },
  { type='TXT', name='Printer._ipp._tcp.local', txt={ 'rp=ipp/print' } },
} }
mdns.ungroup(id)
print(mdns.groups())        -- JSON, like mdns.list()
```

Announced twice on add, withdrawn with a TTL-0 goodbye. Answers include the
usual additionals (PTR -> SRV + TXT, SRV -> A), honour the QU bit, and
answer legacy queriers (`dig @224.0.0.251 -p 5353 printer.local`). Limits:
8 groups, 16 records in all, 8 per group, names up to 63 characters ending
in `.local`, TXT items up to 63 bytes together. No probing: these are names
the script asserts.

Groups **and** `mdns.service` entries belong to the script that declared
them and go when it stops (services used to stay advertised for the life of
the boot). A replaced default `_http` service comes back. Op: `mdns.groups`.

## Forwarding rules

```lua
local id = net.forward{ proto='tcp', listen=6767, to='127.0.0.1', port=80 }
net.forward{ proto='udp', listen=7007, iface='e0', to='192.168.50.9', port=7 }
net.unforward(id)
for _, r in ipairs(net.forwards()) do print(r.id, r.proto, r.listen, r.target) end
```

Not IP forwarding and no NAT: the gateway terminates the client's connection
or datagram and re-originates it, so replies come back through the same
pair whatever the interfaces. `iface` binds the listener to one interface
(`e0`, `w0`, `w1`, `us`, `wg`); omitted, it listens on all.

* **TCP**: spliced with backpressure -- at most one received chain per
  direction is held; until it has been written on, lwIP keeps the rest and
  the sender's window stays closed. FIN is passed on as a half-close; a
  reset on one side aborts the other.
* **UDP**: one session per client (rule, ip, port) with its own outbound
  socket; replies are relayed back from the listening port; 60 s idle
  expiry.
* **Fail fast** (`fail_fast=true`, the default): a target is marked down on
  real signals only -- TCP connect refused or timed out (3 s), an ICMP
  unreachable for a UDP session, no ARP reply for a target on a local
  subnet. While it is down, new TCP clients get an immediate RST and UDP
  senders an ICMP port unreachable. Every 5 s the next attempt passes
  through as a probe, and TCP targets are also re-tested actively. Silence
  is never a failure: one-way UDP is legitimate.
* Limits: 8 rules, 8 spliced TCP connections, 8 UDP sessions.

Rules belong to the script that made them. Op: `net.forwards` (read-only,
with counters and target state).

## Memory and flash

See the commit messages for per-feature numbers; the summary at the time of
writing is in the results section below.

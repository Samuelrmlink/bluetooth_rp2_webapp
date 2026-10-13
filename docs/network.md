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
is no resolver yet) or DHCP server. Ops: `eth.status`, `eth.config {mode, ip,
mask, gw, dns, dhcp_router}` (validated, then applied and saved from the main
loop); topic `eth.state`.

### Addressing modes (`src/net/eth_cfg.c`)

* **dhcp** (default): DHCP client. With no lease after 8 s, AutoIP link-local
  (169.254/16, with ACD conflict detection) is added beside it, and DHCP
  keeps asking -- restarted every 30 s while on link-local, since lwIP's own
  discover backoff grows to one attempt a minute. A lease replaces the
  link-local address; losing it, or a link flap, starts the sequence again.
  This is per-netif, not `LWIP_DHCP_AUTOIP_COOP`: that option is global and
  would put the WiFi station on 169.254 whenever its DHCP was slow. A
  link-local-only e0 has no gateway, so it is never an uplink.
* **manual**: ip/mask, optional gateway (which makes e0 an uplink candidate)
  and DNS.
* **server**: e0 takes a static address (default 192.168.9.1/24) and serves
  DHCP on it (leases .16-.23). Offers carry NO router/DNS option unless
  `dhcp_router` is set, so a computer on the cable keeps its own internet.
  Never automatic: only an explicit `{"mode":"server"}` sets it, and a
  missing, corrupt or unknown `cfg/eth.json` is the DHCP client (host test +
  tier-0 guard). e0 is never an uplink in this mode.

mDNS answers on whatever address e0 has, and lwIP re-probes/announces on
every address and link change (netif ext-status callback).

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

## Hardware results (2026-10-13, fw 1.139/1.140 trial, PC eth0 cabled to the W5500)

The PC side was switched with nmcli on its own profile only, kept
`never-default` / `ignore-auto-dns`, and checked after every change that
its default route and DNS were untouched.

| case | result |
|---|---|
| chip probe | VERSIONR ok, MACRAW open, link 100/full |
| no chip fitted (earlier, 1.136) | `present:false`, SPI1 + DMA + all six pins given back |
| PC link-local, e0 DHCP | e0 169.254.212.146 (same address every boot, MAC-seeded); ping 1.2-2.7 ms both PC->e0; web UI over e0 200 in 7 ms; avahi-browse `_http._tcp` on eth0 shows it at the link-local address; unicast mDNS query answered with it (A, PTR+SRV+TXT+A) |
| link-local timing | 15.6 s from a DHCP restart to the link-local address (8 s DHCP-alone + ~7.5 s ACD); mDNS answering 4.6 s later (lwIP probe) |
| PC manual 192.168.50.1, e0 manual .2 | ping, web UI, avahi on eth0 + unicast query all move to the new address |
| e0 manual with a gateway that does not route | e0 becomes a candidate, its probes fail, and it is never chosen (after the fix below) |
| e0 DHCP server, PC manual 192.168.9.50 | e0 192.168.9.1, server bound on e0, ping/web/mDNS on .1, e0 not an uplink, cfg persisted with dhcp_router false. **Not done:** the PC as a DHCP client of it (owner's rule: the PC is never put in DHCP-client mode on this rig) |
| net.forward TCP in over e0 -> 127.0.0.1:80 | 200 in 10 ms |
| `iface='e0'` binding | answers on e0, not on USB-NCM |
| TCP, client on e0 -> router on WiFi (192.168.1.254:80) | 302 in 155 ms |
| UDP relay, client on e0 -> router DNS over WiFi | answer relayed (the first query of a new session timed out once) |
| fail fast, TCP to a closed port | first attempt 3 s (connect timeout), then RST in ~4 ms |
| fail fast, UDP to a closed port | router's ICMP unreachable marks it down; then ICMP port unreachable back in 1-3 ms, one probe let through every 5 s |
| mDNS group over e0 | printer-test.local answered on e0 |
| mDNS service churn (the 1.97 HardFault) | 20 script stop/start cycles + 30 back-to-back replacements, no fault (was: crash within 1-5 cycles) |
| C1000 (BLE) | connect, GCM negotiation, A1761 telemetry; setters light bar on/off and screen off/on read back through telemetry, all restored |

Not possible on this rig, owner-gated:
* **PC shared mode (e0 DHCP lease, e0 as a real uplink, WireGuard over
  Ethernet, the switch experiment)**: the PC's UFW drops inbound DHCP
  requests on eth0 (and every unsolicited inbound packet -- seen as
  `[UFW BLOCK] IN=eth0` for forwarded TCP/UDP), so dnsmasq never sees a
  DISCOVER. Needs e.g. `ufw allow in on eth0` (plus forwarding for NAT), which
  I may not change.
* Cable unplug/replug (physical).
* A PC as a DHCP client of the e0 server mode (owner, tomorrow).

Found and fixed on the rig:
* The mDNS HardFault (also on 1.97): the lwIP timeout pool was too small
  for the responder's per-netif timers; see `include/lwipopts.h`.
* Uplink failover to an unmeasured link: when w0's probes failed, the policy
  moved to e0, which had just been given a gateway and had never answered a
  probe; it failed too and the tunnel stayed on it. A failover target must
  now have answered probes.

Observed, not caused by this work as far as I can tell: once, the WiFi AP
silently dropped the station while the cyw43 driver still reported a link
(no ARP replies on WiFi, BT unaffected); a `wifi.connect` to the same SSID
brought it back. WireGuard had started flapping before any e0 change. Uplink
selection now detects that state (w0 "failed") but, by design, does not touch
WiFi itself.

## Memory and flash

See the commit messages for per-feature numbers; the summary at the time of
writing is in the results section below.

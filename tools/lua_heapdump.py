#!/usr/bin/env python3
"""
Dump and analyse the gateway's Lua arena (fw 1.90+).

  python3 tools/lua_heapdump.py --host 10.255.255.23 -o dump.json   # fetch over the API
  python3 tools/lua_heapdump.py dump.json                          # analyse one dump
  python3 tools/lua_heapdump.py --diff before.json after.json      # what grew

A dump is what Settings -> System -> "Download heap dump" saves, or what
--host fetches: the device's own census (script.heap, taken after a full
collection) plus the raw arena (script.arena, 1 KB at a time).

The offline half walks the allocator's block chain (src/lua/lheap.c: a
gapless, address-ordered list; each block's header is {size, next, free} in
the first `header` bytes, 32-bit little-endian) and labels every live block
by the Lua GC header at the start of its payload ({next, tt, marked}): a
string, a table, a closure ... or plain data (arrays, hash parts, stacks,
buffers), which have no such header. --diff compares two dumps by label and
by size, which is where a leak shows: something that is there after and was
not before, every cycle.
"""
import argparse, base64, collections, json, struct, sys, time

# Lua 5.4 type tags (lobject.h: makevariant(type, variant)).
TT = {0x04: 'short_string', 0x14: 'long_string', 0x05: 'table', 0x06: 'lua_closure',
      0x16: 'light_c_function', 0x26: 'c_closure', 0x07: 'userdata', 0x08: 'thread',
      0x09: 'upvalue', 0x0A: 'proto'}


def fetch(host):
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    from wsapi import WS

    def call(op, params=None, tries=4):
        """One op on its own connection; a timeout or a dropped frame over a
        slow link is retried rather than losing a 160-chunk dump."""
        for attempt in range(tries):
            try:
                return call_once(op, params)
            except (OSError, RuntimeError, ValueError) as e:
                if attempt == tries - 1:
                    raise
                print(f'\n  {op}: {e}; retrying', file=sys.stderr)
                time.sleep(1)

    def call_once(op, params=None):
        ws = WS(host)
        try:
            ws.send({"id": 1, "op": op, "params": params or {}})
            end = time.time() + 20
            while time.time() < end:
                m = ws.recv(timeout=5)
                if not m:
                    continue
                try:
                    m = json.loads(m)
                except ValueError:
                    continue                # an event cut short; not our reply
                if m.get('id') == 1:
                    if not m.get('ok', True):
                        raise RuntimeError(f"{op}: {m.get('error')}")
                    return m['result']
            raise RuntimeError(f'{op}: no reply')
        finally:
            ws.close()

    heap = call('script.heap')
    parts, off, size, base, header = [], 0, None, 0, 0
    while size is None or off < size:
        r = call('script.arena', {'offset': off, 'len': 1024})
        size, base, header = r['size'], r['base'], r['header']
        parts.append(r['hex'])
        off += 1024
        if off % 16384 == 0 or off >= size:
            print(f'  arena {min(off, size)}/{size} B', file=sys.stderr)
    print(file=sys.stderr)
    return {'taken': time.strftime('%Y-%m-%dT%H:%M:%S'), 'heap': heap,
            'arena': {'base': base, 'header': header, 'size': size, 'hex': ''.join(parts)}}


def blocks(dump):
    """[(offset, size, free, label)] from the raw arena."""
    a = dump['arena']
    raw = bytes.fromhex(a['hex'])
    base, hdr = a['base'], a['header']
    out, off = [], 0
    while off + hdr <= len(raw):
        size, nxt, free = struct.unpack_from('<IIB', raw, off)
        payload = raw[off + hdr: off + hdr + size]
        label = 'free' if free else label_of(payload, base, len(raw))
        out.append((off, size, bool(free), label))
        if nxt == 0:
            break
        noff = nxt - base
        if noff <= off or noff >= len(raw):
            out.append((off, 0, False, 'CHAIN BROKEN'))
            break
        off = noff
    return out


def label_of(p, base, size):
    """A live block's Lua type from its GC header, or 'data'."""
    if len(p) >= 6:
        nxt = struct.unpack_from('<I', p, 0)[0]
        tt = p[4]
        # `next` is NULL or points into the arena; tt is a known variant
        if tt in TT and (nxt == 0 or base <= nxt < base + size):
            return TT[tt]
    return 'data'


def summary(dump):
    bl = blocks(dump)
    by = collections.defaultdict(lambda: [0, 0])
    for _, size, free, label in bl:
        by[label][0] += 1
        by[label][1] += size
    return bl, by


def show(dump):
    h = dump.get('heap', {})
    a = h.get('arena', {})
    print(f"taken {dump.get('taken')}: used {a.get('used')} B of {a.get('bytes')}, peak {a.get('peak')}, "
          f"largest free {a.get('largest_free')} in {a.get('free_blocks')} free blocks, "
          f"{a.get('alloc_fails')} failed allocations")
    print('free block sizes:', ', '.join(f"{x['size']}: {x['n']} ({x['bytes']} B)" for x in a.get('free_hist', [])))
    print('\nLua objects (device census):')
    for k, v in sorted((h.get('objects') or {}).items(), key=lambda kv: -(kv[1]['bytes'] if isinstance(kv[1], dict) else 0)):
        if isinstance(v, dict) and v['n']:
            print(f'  {k:18s} {v["n"]:6d} objects  {v["bytes"]:7d} B')
    print(f"  unaccounted (non-object blocks: arrays, hash parts, stacks, buffers): {h.get('unaccounted')} B")
    print('largest tables:', ', '.join(f"{t['name'] or '?'} {t['bytes']} B" for t in h.get('tables', [])))
    bl, by = summary(dump)
    print(f'\narena blocks (offline walk, {len(bl)} blocks):')
    for label, (n, b) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print(f'  {label:18s} {n:6d} blocks  {b:7d} B')
    if any(l == 'CHAIN BROKEN' for *_, l in bl):
        print('  !! the block chain is broken -- arena corruption, or a dump taken mid-allocation')


def diff(a, b):
    _, ba = summary(a)
    _, bb = summary(b)
    print(f"{a.get('taken')} -> {b.get('taken')}")
    ua, ub = a['heap']['arena']['used'], b['heap']['arena']['used']
    print(f'used {ua} -> {ub} B ({ub - ua:+d})\n')
    print(f"{'block label':18s} {'count':>14s} {'bytes':>18s}")
    for label in sorted(set(ba) | set(bb)):
        (na, sa), (nb, sb) = ba.get(label, (0, 0)), bb.get(label, (0, 0))
        if (na, sa) != (nb, sb):
            print(f'{label:18s} {na:6d} -> {nb:6d} {sa:8d} -> {sb:8d} ({sb - sa:+d})')
    oa, ob = a['heap'].get('objects', {}), b['heap'].get('objects', {})
    print('\ndevice census by type:')
    for k in sorted(set(oa) | set(ob)):
        va, vb = oa.get(k, {}), ob.get(k, {})
        if isinstance(va, dict) and isinstance(vb, dict) and va != vb:
            print(f"  {k:18s} {va.get('n', 0):6d} -> {vb.get('n', 0):6d}  {va.get('bytes', 0):7d} -> {vb.get('bytes', 0):7d} B")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumps', nargs='*')
    ap.add_argument('--host', help='fetch a dump from this gateway')
    ap.add_argument('-o', '--out', help='with --host: save the dump here')
    ap.add_argument('--diff', action='store_true', help='compare two dumps')
    a = ap.parse_args()
    if a.host:
        d = fetch(a.host)
        if a.out:
            json.dump(d, open(a.out, 'w'))
            print(f'saved {a.out}', file=sys.stderr)
        show(d)
        return
    dumps = [json.load(open(p)) for p in a.dumps]
    if a.diff:
        if len(dumps) != 2:
            ap.error('--diff takes two dumps')
        diff(*dumps)
    else:
        for d in dumps:
            show(d)


if __name__ == '__main__':
    main()

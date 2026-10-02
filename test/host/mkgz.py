"""Writes gzip test streams for test/host/inflate_test.c (run by
tools/run_tests.py): Python zlib output at several levels, with every
optional header field, over random and real repo files -- plus corrupt
variants that must be rejected."""
import gzip, zlib, os, random, sys, glob, struct
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
for f in glob.glob(out + '/*'): os.remove(f)
rnd = random.Random(1)
samples = {
  'empty': b'', 'one': b'x', 'zeros': bytes(30000), 'abc': b'abc' * 9000,
  'random_small': bytes(rnd.getrandbits(8) for _ in range(300)),
  'random_16k': bytes(rnd.getrandbits(8) for _ in range(16384)),
  'exact_32k': bytes(rnd.getrandbits(8) for _ in range(32768)),
  'too_big': b'y' * 40000,
}
for p in glob.glob('lua/**/*.lua', recursive=True) + glob.glob('web/js/*.js')[:8]:
    d = open(p, 'rb').read()
    if len(d) <= 32768: samples[p.replace('/', '_')] = d
i = 0
for name, data in samples.items():
    for level in (0, 1, 6, 9):
        z = zlib.compressobj(level, zlib.DEFLATED, -15)
        body = z.compress(data) + z.flush()
        variants = {
          'plain': gzip.compress(data, compresslevel=level, mtime=0),
          'hdrs': b'\x1f\x8b\x08' + bytes([4 | 8 | 16 | 2]) + bytes(6) + struct.pack('<H', 3) + b'abc' + b'name\0' + b'comment\0' + b'\0\0'
                  + body + struct.pack('<II', zlib.crc32(data), len(data) & 0xffffffff),
        }
        for vn, gz in variants.items():
            base = f'{out}/{i:03d}_{name}_{level}_{vn}'
            open(base + '.gz', 'wb').write(gz); open(base + '.raw', 'wb').write(data)
            if len(gz) > 24 and len(data) <= 32768:
                # A flipped bit must be rejected only when it really corrupts
                # the stream: one in the header's mtime / OS / extra field is
                # (correctly) ignored -- by zlib too.
                g = bytes(gz[:len(gz) // 2]) + bytes([gz[len(gz) // 2] ^ 0x40]) + bytes(gz[len(gz) // 2 + 1:])
                try:
                    harmless = gzip.decompress(g) == data
                except Exception:
                    harmless = False
                if not harmless:
                    open(base + '.flip.bad', 'wb').write(g)
                open(base + '.trunc.bad', 'wb').write(gz[: len(gz) - 5])
            i += 1
open(out + '/notgz.bad', 'wb').write(b'-- plain lua\n')
print(i, 'streams')

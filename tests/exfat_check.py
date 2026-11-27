#!/usr/bin/env python3
"""After exfat_test: checks the volume with an independent checker and checks
every map the image mounter built against the checker's view of the file."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import struct
mode, img, rep = sys.argv[1], sys.argv[2], sys.argv[3]
fails = passes = 0
def check(c, msg):
    global fails, passes
    if c: passes += 1
    else: fails += 1; print('  FAIL:', msg)

if mode == 'exfat':
    import exfatcheck
    probs, files = exfatcheck.check(img)
    f = open(img, 'rb'); s0 = f.read(512)
    start = struct.unpack_from('<I', s0, 0x1BE + 8)[0]
    f.seek(start * 512); vbr = f.read(512)
    heap = start + struct.unpack_from('<I', vbr, 0x58)[0]; spc = 1 << vbr[0x6D]
    key = lambda n: '/images/' + n
else:
    import fatcheck
    probs, files, info = fatcheck.check(img)
    heap, spc = info['data'], info['spc']
    key = lambda n: '/IMAGES/' + n
    for k, v in list(files.items()):
        v['clusters'] = v['chain']
for p in probs:
    print('  checker:', p)
check(not probs, '%s volume consistent (%d problems)' % (mode, len(probs)))

for line in open(rep):
    parts = line.split()
    name, size, n = parts[0], int(parts[1]), int(parts[2])
    info = next((v for k, v in files.items() if k.lower() == key(name).lower()), None)
    check(info is not None, '%s present' % name)
    if not info: continue
    check(info['size'] == size, '%s size %d vs %d' % (name, info['size'], size))
    # expand the checker's clusters to SD sectors and compare with the map
    want = []
    for c in info['clusters']:
        want.extend(range(heap + (c - 2) * spc, heap + (c - 1) * spc))
    want = want[:size // 512]
    got = []
    for e in parts[3:3 + n]:
        fs, sd, cnt = map(int, e.split(':'))
        check(fs == len(got), '%s extent order' % name)
        got.extend(range(sd, sd + cnt))
    check(got == want, '%s map matches the on-disk allocation (%d vs %d sectors)' % (name, len(got), len(want)))
    if mode == 'exfat' and name in ('BIG.IMG', 'A.IMG'):
        check(info['nofat'], '%s stored contiguous (NoFatChain)' % name)
print('exfat_check %s: %d checks passed, %d failed' % (mode, passes, fails))
sys.exit(1 if fails else 0)

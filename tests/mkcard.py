#!/usr/bin/env python3
"""Builds SD card images for the quick-create tests: a FAT16/FAT32 volume
with a /IMAGES directory containing filler entries (with long file names),
other files laid out to chop free space into holes, and a 1-byte placeholder
file, exactly as libfat leaves it before quickAlloc_allocateRaw runs.

Writes <out>.img and <out>.json: {dir_cluster, placeholder, files:{path: sha}}.
"""
import hashlib, json, random, struct, sys


def lfn_entries(long_name, short83):
    chk = 0
    for ch in short83:
        chk = (((chk & 1) << 7) + (chk >> 1) + ch) & 0xFF
    name = long_name.encode('utf-16-le') + b'\x00\x00'
    while len(name) % 26: name += b'\xff\xff'
    parts = [name[i:i + 26] for i in range(0, len(name), 26)]
    out = []
    for i, p in enumerate(parts):
        e = bytearray(32)
        seq = i + 1
        if i == len(parts) - 1: seq |= 0x40
        e[0] = seq; e[1:11] = p[0:10]; e[11] = 0x0F; e[13] = chk
        e[14:26] = p[10:22]; e[28:32] = p[22:26]
        out.append(bytes(e))
    return list(reversed(out))


def build(out, fat32=True, spc=8, clusters=None, hole=0, gap=1, fill_frac=0.0,
          filler=40, mirror_off=False, seed=7, free_tail=True):
    rnd = random.Random(seed)
    part_start = 2048
    if clusters is None:
        clusters = 70000 if fat32 else 20000
    rsvd = 32 if fat32 else 4
    nfats = 2
    rootents = 0 if fat32 else 512
    ent = 4 if fat32 else 2
    spf = ((clusters + 2) * ent + 511) // 512
    data_rel = rsvd + nfats * spf + (rootents * 32) // 512
    psect = data_rel + clusters * spc
    total = part_start + psect
    disk = bytearray(total * 512)
    last = clusters + 1

    mbr = bytearray(512); mbr[0x1BE + 4] = 0x0C if fat32 else 0x06
    struct.pack_into('<II', mbr, 0x1BE + 8, part_start, psect); mbr[510:512] = b'\x55\xaa'
    disk[0:512] = mbr
    v = bytearray(512); v[0:3] = b'\xEB\x58\x90'; v[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', v, 11, 512, spc, rsvd, nfats, rootents, 0, 0xF8,
                     0 if fat32 else spf, 63, 255, part_start, psect)
    if fat32:
        struct.pack_into('<IHHI', v, 36, spf, 0x81 if mirror_off else 0, 0, 2)
        struct.pack_into('<HH', v, 48, 1, 6); v[66] = 0x29; v[82:90] = b'FAT32   '
    else:
        v[38] = 0x29; v[54:62] = b'FAT16   '
    v[510:512] = b'\x55\xaa'
    disk[part_start * 512:part_start * 512 + 512] = v

    fat_start = part_start + rsvd
    data = part_start + data_rel
    rootdir = part_start + rsvd + nfats * spf
    EOC = 0x0FFFFFFF if fat32 else 0xFFFF
    fat = {0: 0x0FFFFFF8 if fat32 else 0xFFF8, 1: EOC}
    csec = lambda c: data + (c - 2) * spc
    used = set()

    def alloc_chain(chain):
        for c in chain: assert c not in used and 2 <= c <= last; used.add(c)
        for a, b in zip(chain, chain[1:]): fat[a] = b
        fat[chain[-1]] = EOC

    nxt = 2
    if fat32:
        root = [nxt]; nxt += 1; alloc_chain(root)
    dirc = [nxt, nxt + 1]; nxt += 2; alloc_chain(dirc)   # /IMAGES spans 2 clusters

    # other files: chop free space into holes of `hole` clusters
    files = {}
    filedata = {}
    content = []
    if hole:
        blocks = []
        c = nxt
        limit = int(last * (1 - (0.02 if free_tail else 0)))
        while c + hole + gap <= limit:
            blocks.append(list(range(c + hole, c + hole + gap)))
            c += hole + gap
        max_files = min(100, spc * 16 - 3)            # keep the root directory within one cluster
        per = max(64, -(-len(blocks) // max_files))
        for i in range(0, len(blocks), per):          # group gap clusters into files
            ch = [x for blk in blocks[i:i + per] for x in blk]
            name = f'F{i // per:05d}.BIN'
            content.append((name, ch))
        if not free_tail:
            # fill whatever is after the last hole too
            tail = list(range(c + hole, last + 1))
            if tail: content.append(('TAIL.BIN', tail))
    elif fill_frac:
        n = int(clusters * fill_frac)
        content.append(('BIG.BIN', list(range(nxt, nxt + n))))
    for name, ch in content:
        alloc_chain(ch)
        size = len(ch) * spc * 512 - rnd.randint(0, 511)
        blob = bytes(rnd.getrandbits(8) for _ in range(min(size, 4096))) * (size // 4096 + 1)
        blob = blob[:size]
        for i, c in enumerate(ch):
            o = csec(c) * 512
            disk[o:o + spc * 512] = blob[i * spc * 512:(i + 1) * spc * 512].ljust(spc * 512, b'\0')
        files['/' + name] = (ch[0], size)
        filedata['/' + name] = hashlib.sha1(blob).hexdigest()

    # placeholder: one cluster near the start
    free_now = [c for c in range(2, last + 1) if c not in used]
    ph = free_now[0]
    alloc_chain([ph])
    disk[csec(ph) * 512] = 0

    # directory contents
    def entry(n83, attr, start, size):
        e = bytearray(32); e[0:11] = n83; e[11] = attr
        struct.pack_into('<H', e, 20, (start >> 16) if fat32 else 0)
        struct.pack_into('<H', e, 26, start & 0xFFFF); struct.pack_into('<I', e, 28, size)
        return bytes(e)

    rootents_l = [entry(b'IMAGES     ', 0x10, dirc[0], 0)]
    for name, (st, sz) in files.items():
        b_, _, e_ = name[1:].partition('.')
        rootents_l.append(entry((b_.ljust(8) + e_.ljust(3)).encode(), 0x20, st, sz))
    ro = (csec(root[0]) if fat32 else rootdir) * 512
    for i, e in enumerate(rootents_l): disk[ro + i * 32:ro + i * 32 + 32] = e

    dents = [entry(b'.          ', 0x10, dirc[0], 0), entry(b'..         ', 0x10, 0, 0)]
    for i in range(filler):                    # empty filler files, with LFNs
        s83 = f'FILL{i:04d}IMG'.encode()
        dents += lfn_entries(f'filler image number {i}.img', s83)
        dents.append(entry(s83, 0x20, 0, 0))
    s83 = b'NEWIMAGEIMG'
    dents += lfn_entries('My New Xbox Memory Unit.img', s83)
    dents.append(entry(s83, 0x20, ph, 1))
    cap = 2 * spc * 16
    assert len(dents) < cap, (len(dents), cap)
    dsecs = [csec(c) + i for c in dirc for i in range(spc)]
    for i, e in enumerate(dents):
        o = dsecs[i // 16] * 512 + (i % 16) * 32
        disk[o:o + 32] = e
    ph_index = len(dents) - 1
    assert ph_index >= spc * 16, 'placeholder should be in the 2nd dir cluster'

    # FATs
    copies = [1] if mirror_off else range(nfats)
    for k in copies:
        bo = (fat_start + k * spf) * 512
        for c, val in fat.items():
            if fat32: struct.pack_into('<I', disk, bo + c * 4, val)
            else: struct.pack_into('<H', disk, bo + c * 2, val)
    if mirror_off:
        # FAT 0 holds stale junk that must be ignored and left alone
        bo = fat_start * 512
        for c in range(2, min(last, 5000)):
            struct.pack_into('<I', disk, bo + c * 4, 0x0BADBAD)

    free = clusters - len(used)
    if fat32:
        fi = bytearray(512)
        struct.pack_into('<I', fi, 0, 0x41615252); struct.pack_into('<I', fi, 484, 0x61417272)
        struct.pack_into('<II', fi, 488, free, 0xFFFFFFFF); struct.pack_into('<I', fi, 508, 0xAA550000)
        disk[(part_start + 1) * 512:(part_start + 2) * 512] = fi

    open(out + '.img', 'wb').write(disk)
    json.dump({'dir_cluster': dirc[0], 'placeholder': ph, 'free': free, 'spc': spc,
               'files': filedata, 'mirror_off': mirror_off}, open(out + '.json', 'w'), indent=1)


if __name__ == '__main__':
    out, kind = sys.argv[1], sys.argv[2]
    if kind == 'roomy32':   build(out, True, spc=8)
    elif kind == 'holes32': build(out, True, spc=8, hole=37, gap=3)
    elif kind == 'tiny32':  build(out, True, spc=8, hole=1, gap=1)
    elif kind == 'full32':  build(out, True, spc=8, hole=5, gap=60, free_tail=False)
    elif kind == 'mirror32': build(out, True, spc=8, hole=50, gap=2, mirror_off=True)
    elif kind == 'crash32': build(out, True, spc=1, clusters=66000, hole=37, gap=3, filler=8)
    elif kind == 'roomy16': build(out, False, spc=4, filler=20)
    elif kind == 'holes16': build(out, False, spc=4, hole=20, gap=5, filler=20)
    else: raise SystemExit('unknown kind')

#!/usr/bin/env python3
"""Independent FAT16/FAT32 consistency checker (a small fsck.fat -n).

Usage: fatcheck.py disk.img            -> prints a report, exit 1 on problems
As a module: check(path) -> (problems, files) where files maps
'/DIR/NAME' -> {'size', 'start', 'chain', 'attr', 'lfn'}
"""
import struct, sys

EOC32, EOC16 = 0x0FFFFFF8, 0xFFF8


def check(path):
    d = open(path, 'rb').read()
    problems = []
    rd16 = lambda o: struct.unpack_from('<H', d, o)[0]
    rd32 = lambda o: struct.unpack_from('<I', d, o)[0]

    # partition: sector 0 is a VBR, or the first MBR entry
    ps = 0
    if d[0x36:0x39] != b'FAT' and d[0x52:0x55] != b'FAT':
        ps = rd32(0x1BE + 8)
    b = ps * 512
    bps, spc = rd16(b + 11), d[b + 13]
    rsvd, nfats, rootents = rd16(b + 14), d[b + 16], rd16(b + 17)
    tot = rd16(b + 19) or rd32(b + 32)
    spf = rd16(b + 22) or rd32(b + 36)
    rootdir = ps + rsvd + nfats * spf
    data = rootdir + (rootents * 32) // 512
    clusters = (tot - (data - ps)) // spc
    last = clusters + 1
    fat32 = clusters >= 65525
    extflags = rd16(b + 40) if fat32 else 0
    mirrored = not (fat32 and (extflags & 0x80))
    active = (extflags & 0x0F) if not mirrored else 0
    fat_off = (ps + rsvd + active * spf) * 512
    ent = 4 if fat32 else 2
    eoc = EOC32 if fat32 else EOC16

    def fat(c):
        v = rd32(fat_off + c * 4) & 0x0FFFFFFF if fat32 else rd16(fat_off + c * 2)
        return v

    if mirrored:
        size = (last + 1) * ent
        f0 = d[(ps + rsvd) * 512:(ps + rsvd) * 512 + size]
        for k in range(1, nfats):
            o = (ps + rsvd + k * spf) * 512
            if d[o:o + size] != f0:
                problems.append(f'FAT copy {k} differs from FAT 0')

    owner = {}
    files = {}

    def chain_of(start, what):
        ch = []
        c = start
        seen = set()
        while True:
            if c < 2 or c > last:
                problems.append(f'{what}: cluster {c} out of range'); break
            if c in seen:
                problems.append(f'{what}: loop at {c}'); break
            seen.add(c); ch.append(c)
            if c in owner:
                problems.append(f'{what}: cluster {c} cross-linked with {owner[c]}')
            owner[c] = what
            n = fat(c)
            if n == 0:
                problems.append(f'{what}: free cluster {c} inside chain'); break
            if n >= eoc:
                break
            c = n
        return ch

    def csec(c):
        return data + (c - 2) * spc

    def walk_dir(sectors, prefix):
        lfn = []
        for s in sectors:
            for e in range(16):
                o = s * 512 + e * 32
                ent_ = d[o:o + 32]
                if ent_[0] == 0:
                    return
                if ent_[0] == 0xE5:
                    lfn = []; continue
                attr = ent_[11]
                if attr == 0x0F:
                    lfn.append(ent_); continue
                if attr & 0x08:
                    lfn = []; continue
                name = ent_[0:8].decode('latin1').rstrip() + ('.' + ent_[8:11].decode('latin1').rstrip() if ent_[8:11].strip() else '')
                if name in ('.', '..'):
                    lfn = []; continue
                start = struct.unpack_from('<H', ent_, 26)[0] | ((struct.unpack_from('<H', ent_, 20)[0] << 16) if fat32 else 0)
                fsize = struct.unpack_from('<I', ent_, 28)[0]
                full = prefix + '/' + name
                info = {'size': fsize, 'start': start, 'attr': attr, 'lfn': len(lfn), 'chain': []}
                if start:
                    info['chain'] = chain_of(start, full)
                if attr & 0x10:
                    walk_dir([csec(c) + i for c in info['chain'] for i in range(spc)], full)
                else:
                    need = (fsize + spc * 512 - 1) // (spc * 512)
                    if len(info['chain']) != need:
                        problems.append(f'{full}: chain has {len(info["chain"])} clusters, size needs {need}')
                files[full] = info
                lfn = []

    if fat32:
        rc = rd32(b + 44)
        rch = chain_of(rc, '/')
        walk_dir([csec(c) + i for c in rch for i in range(spc)], '')
    else:
        walk_dir(list(range(rootdir, data)), '')

    used = 0
    for c in range(2, last + 1):
        v = fat(c)
        if v != 0:
            used += 1
            if c not in owner:
                problems.append(f'lost cluster {c} (FAT={v:#x})')
    free = clusters - used

    if fat32:
        fsi = ps + rd16(b + 48)
        o = fsi * 512
        if rd32(o) == 0x41615252 and rd32(o + 484) == 0x61417272:
            fc = rd32(o + 488)
            if fc != 0xFFFFFFFF and fc != free:
                problems.append(f'FSInfo free count {fc}, actual {free}')
    return problems, files, {'free': free, 'clusters': clusters, 'spc': spc, 'fat32': fat32, 'data': data}


if __name__ == '__main__':
    probs, files, info = check(sys.argv[1])
    for p in probs[:50]:
        print('PROBLEM:', p)
    print(f"{len(files)} entries, {info['free']} free clusters, {len(probs)} problems")
    sys.exit(1 if probs else 0)

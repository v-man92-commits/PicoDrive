#!/usr/bin/env python3
"""Build a FAT16/FAT32 SD-card style disk image (MBR + one partition) with
files whose cluster chains are laid out exactly as requested, so the
DSpico image mounter's FAT chain mapper can be tested against known truth.

Writes <out>.img (the disk) and <out>.json describing every file:
  name, size, start cluster, and the true list of SD sectors per 512-byte
  file sector (as run-length ranges).
"""
import json, struct, sys, random

def build(out, fat32=True, part_start=2048, total_sectors=None, spc=8,
          files=(), seed=1):
    rnd = random.Random(seed)
    if total_sectors is None:
        total_sectors = 70000 * spc + 4096 if fat32 else 40000 * spc + 1024
    rsvd = 32 if fat32 else 4
    nfats = 2
    root_entries = 0 if fat32 else 512
    root_dir_sectors = (root_entries * 32) // 512
    # cluster count estimate -> fat size
    psect = total_sectors - part_start
    clusters = psect // spc
    ent = 4 if fat32 else 2
    fat_sz = (clusters * ent + 2 * ent + 511) // 512
    data_start_rel = rsvd + nfats * fat_sz + root_dir_sectors
    clusters = (psect - data_start_rel) // spc
    if fat32: assert clusters >= 65525, clusters
    else: assert 4085 <= clusters < 65525, clusters
    last_cluster = clusters + 1

    disk = bytearray(total_sectors * 512)
    # MBR
    mbr = bytearray(512)
    mbr[0x1BE + 4] = 0x0C if fat32 else 0x06
    struct.pack_into('<II', mbr, 0x1BE + 8, part_start, psect)
    mbr[510:512] = b'\x55\xaa'
    disk[0:512] = mbr

    # VBR
    v = bytearray(512)
    v[0:3] = b'\xEB\x58\x90'
    v[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', v, 11, 512, spc, rsvd, nfats,
                     root_entries, 0, 0xF8, 0 if fat32 else fat_sz, 63, 255, part_start, psect)
    if fat32:
        struct.pack_into('<IHHI', v, 36, fat_sz, 0, 0, 2)   # extflags 0 = mirrored
        struct.pack_into('<HH', v, 48, 1, 6)
        v[66] = 0x29
        v[71:82] = b'TESTCARD   '
        v[82:90] = b'FAT32   '
    else:
        v[38] = 0x29
        v[43:54] = b'TESTCARD   '
        v[54:62] = b'FAT16   '
    v[510:512] = b'\x55\xaa'
    p0 = part_start * 512
    disk[p0:p0 + 512] = v

    fat_start = part_start + rsvd
    data_start = part_start + data_start_rel
    root_dir_start = part_start + rsvd + nfats * fat_sz

    fat = {}
    used = set()
    EOC = 0x0FFFFFFF if fat32 else 0xFFFF
    if fat32:
        fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF; fat[2] = EOC; used.add(2)  # root dir
    else:
        fat[0] = 0xFFF8; fat[1] = 0xFFFF

    def csec(c): return data_start + (c - 2) * spc

    dirents = []
    meta = []
    next_free = 3 if fat32 else 2
    for f in files:
        name, size, layout = f['name'], f['size'], f.get('layout', 'contig')
        fill = f.get('fill', 'random')
        nclus = max(1, (size + spc * 512 - 1) // (spc * 512)) if size else 0
        chain = []
        if layout == 'contig':
            chain = list(range(next_free, next_free + nclus))
            next_free += nclus
        elif layout == 'pieces':
            # split into N pieces placed out of order with gaps
            n = f['pieces']
            sizes = [nclus // n] * n
            for i in range(nclus % n): sizes[i] += 1
            starts = []
            cursor = next_free
            for s in sizes:
                starts.append(cursor)
                cursor += s + rnd.randint(1, 50)
            order = list(range(n)); rnd.shuffle(order)
            pieces = [list(range(starts[i], starts[i] + sizes[i])) for i in range(n)]
            for i in order: chain += pieces[i]
            next_free = cursor
        elif layout == 'scatter':
            base = next_free
            pool = list(range(base, base + nclus * 3)); rnd.shuffle(pool)
            chain = pool[:nclus]
            next_free = base + nclus * 3
        for c in chain:
            assert c not in used and 2 <= c <= last_cluster, c
            used.add(c)
        for a, b in zip(chain, chain[1:]): fat[a] = b
        if chain: fat[chain[-1]] = EOC
        if f.get('break_chain') and len(chain) > 2:
            fat[chain[len(chain) // 2]] = EOC   # chain ends early

        # content
        data = bytearray(size)
        if fill == 'random':
            for i in range(0, size, 4):
                data[i:i + 4] = struct.pack('<I', rnd.getrandbits(32))[:min(4, size - i)]
        elif fill == 'zero':
            pass
        elif fill == 'isolike':
            # ISO-shaped: 32 KiB zero system area, data, zero padding at the end
            for i in range(32768, size - 65536, 4):
                data[i:i + 4] = struct.pack('<I', rnd.getrandbits(32))
        for i, c in enumerate(chain):
            chunk = data[i * spc * 512:(i + 1) * spc * 512]
            o = csec(c) * 512
            disk[o:o + len(chunk)] = chunk

        # true sector map per 512-byte file sector
        truth = []
        for s in range(size // 512):
            c = chain[s // spc]
            truth.append(csec(c) + s % spc)
        # run-length encode
        runs = []
        for i, sd in enumerate(truth):
            if runs and runs[-1][1] + runs[-1][2] == sd:
                runs[-1][2] += 1
            else:
                runs.append([i, sd, 1])
        meta.append({'name': name, 'size': size, 'start': chain[0] if chain else 0,
                     'runs': runs, 'clusters': len(chain)})
        dirents.append((name, size, chain[0] if chain else 0))

    # write FATs
    for k in range(nfats):
        base = (fat_start + k * fat_sz) * 512
        for c, val in fat.items():
            if fat32: struct.pack_into('<I', disk, base + c * 4, val)
            else: struct.pack_into('<H', disk, base + c * 2, val)

    # root dir entries (8.3 names only, good enough for the test)
    rd = csec(2) if fat32 else root_dir_start
    o = rd * 512
    for i, (name, size, start) in enumerate(dirents):
        b, _, e = name.upper().partition('.')
        ent83 = (b[:8].ljust(8) + e[:3].ljust(3)).encode()
        d = bytearray(32)
        d[0:11] = ent83; d[11] = 0x20
        struct.pack_into('<H', d, 20, start >> 16)
        struct.pack_into('<H', d, 26, start & 0xFFFF)
        struct.pack_into('<I', d, 28, size)
        disk[o + i * 32:o + i * 32 + 32] = d

    open(out + '.img', 'wb').write(disk)
    json.dump({'fat32': fat32, 'spc': spc, 'part_start': part_start,
               'data_start': data_start, 'files': meta}, open(out + '.json', 'w'), indent=1)

if __name__ == '__main__':
    out = sys.argv[1]
    kind = sys.argv[2]
    MiB = 1024 * 1024
    if kind == 'fat32':
        build(out, True, spc=8, files=[
            {'name': 'CONTIG.IMG', 'size': 3 * MiB},
            {'name': 'PIECES.IMG', 'size': 2 * MiB + 1536, 'layout': 'pieces', 'pieces': 7},
            {'name': 'SCATTER.IMG', 'size': 1 * MiB, 'layout': 'scatter'},
            {'name': 'BLANK.IMG', 'size': 2 * MiB, 'layout': 'pieces', 'pieces': 4, 'fill': 'zero'},
            {'name': 'ODD.IMG', 'size': 100 * 512 + 77, 'layout': 'pieces', 'pieces': 3},
            {'name': 'BROKEN.IMG', 'size': 1 * MiB, 'layout': 'pieces', 'pieces': 3, 'break_chain': True},
            {'name': 'ZEROLEN.IMG', 'size': 0},
            {'name': 'ISO1.ISO', 'size': 2 * MiB, 'fill': 'isolike'},
            {'name': 'ISO2.ISO', 'size': 3 * MiB, 'layout': 'pieces', 'pieces': 5, 'fill': 'isolike'},
        ])
    else:
        build(out, False, spc=4, files=[
            {'name': 'F16A.IMG', 'size': 3 * MiB, 'layout': 'pieces', 'pieces': 5},
            {'name': 'F16B.IMG', 'size': 512 * 1024, 'layout': 'scatter', 'fill': 'zero'},
        ])

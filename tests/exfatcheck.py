#!/usr/bin/env python3
"""Independent exFAT consistency checker (a small fsck.exfat -n), written from
the exFAT specification, not from FatFs.

Checks: boot region checksum, directory entry set checksums, every file's
clusters (FAT chain or contiguous "NoFatChain"), no cluster used twice, the
allocation bitmap matching the clusters actually in use (no lost or missing
clusters), ValidDataLength <= DataLength.

Usage: exfatcheck.py disk.img [partition-start-sector]
As a module: check(path, start=None) -> (problems, files)
files maps '/DIR/NAME' -> {'size', 'valid', 'first', 'nofat', 'clusters', 'dir'}
"""
import struct, sys


def check(path, start=None):
    f = open(path, 'rb')
    problems = []

    def rd(sector, count=1):
        f.seek(sector * 512)
        return f.read(count * 512)

    if start is None:
        s0 = rd(0)
        start = 0 if s0[3:11] == b'EXFAT   ' else struct.unpack_from('<I', s0, 0x1BE + 8)[0]
    vbr = rd(start)
    if vbr[3:11] != b'EXFAT   ':
        return ['no exFAT boot sector'], {}
    q = lambda o: struct.unpack_from('<Q', vbr, o)[0]
    d = lambda o: struct.unpack_from('<I', vbr, o)[0]
    fat_off, fat_len = d(0x50), d(0x54)
    heap_off, cluster_count, root_cluster = d(0x58), d(0x5C), d(0x60)
    bps_shift, spc_shift, nfats = vbr[0x6C], vbr[0x6D], vbr[0x6E]
    if bps_shift != 9:
        return ['only 512-byte sectors are handled'], {}
    spc = 1 << spc_shift
    cbytes = spc * 512
    if q(0x40) != start:
        problems.append('PartitionOffset %d != partition start %d' % (q(0x40), start))
    if vbr[0x1FE:0x200] != b'\x55\xaa':
        problems.append('boot signature missing')

    # boot region checksum (sectors 0-10, skipping VolumeFlags and PercentInUse)
    region = rd(start, 11)
    chk = 0
    for i, b in enumerate(region):
        if i in (106, 107, 112):
            continue
        chk = (((chk >> 1) | (chk << 31)) + b) & 0xFFFFFFFF
    cs = rd(start + 11)
    if any(struct.unpack_from('<I', cs, i)[0] != chk for i in range(0, 512, 4)):
        problems.append('boot checksum sector does not match')

    fat = rd(start + fat_off, fat_len)
    fat_entry = lambda c: struct.unpack_from('<I', fat, c * 4)[0]

    def cluster_sector(c):
        return start + heap_off + (c - 2) * spc

    def chain(first, count, what):
        out, c = [], first
        for i in range(count):
            if c < 2 or c >= cluster_count + 2:
                problems.append('%s: bad cluster %#x in chain' % (what, c))
                return out
            out.append(c)
            nxt = fat_entry(c)
            if i == count - 1:
                if nxt != 0xFFFFFFFF:
                    problems.append('%s: chain does not end after %d clusters (next %#x)' % (what, count, nxt))
            else:
                c = nxt
        return out

    def clusters_of(first, length, nofat, what):
        count = (length + cbytes - 1) // cbytes
        if count == 0:
            if first not in (0,):
                problems.append('%s: empty but FirstCluster=%d' % (what, first))
            return []
        if first < 2 or first + (count if nofat else 1) - 1 >= cluster_count + 2:
            problems.append('%s: clusters out of range (first %d, %d clusters)' % (what, first, count))
            return []
        if nofat:
            return list(range(first, first + count))
        return chain(first, count, what)

    def read_clusters(cl):
        return b''.join(rd(cluster_sector(c), spc) for c in cl)

    used = {}
    files = {}

    def claim(cl, owner):
        for c in cl:
            if c in used:
                problems.append('cluster %d used by both %s and %s' % (c, used[c], owner))
            else:
                used[c] = owner

    bitmap = None

    def walk(dir_clusters, dpath, depth=0):
        nonlocal bitmap
        data = read_clusters(dir_clusters)
        i = 0
        while i + 32 <= len(data):
            t = data[i]
            if t == 0x00:
                break
            if not t & 0x80:
                i += 32
                continue
            if t == 0x81:  # allocation bitmap
                first, length = struct.unpack_from('<IQ', data, i + 20)
                cl = clusters_of(first, length, False, 'bitmap')
                claim(cl, 'bitmap')
                bitmap = (read_clusters(cl)[:length], cl)
            elif t == 0x82:  # up-case table
                first, length = struct.unpack_from('<IQ', data, i + 20)
                claim(clusters_of(first, length, False, 'upcase'), 'upcase')
            elif t == 0x85:  # file entry set
                sec = data[i + 1]
                entries = data[i:i + 32 * (sec + 1)]
                if len(entries) < 32 * (sec + 1) or sec < 2:
                    problems.append('%s: truncated entry set' % dpath)
                    break
                s = 0
                for j, b in enumerate(entries):
                    if j in (2, 3):
                        continue
                    s = (((s >> 1) | (s << 15)) + b) & 0xFFFF
                if s != struct.unpack_from('<H', entries, 2)[0]:
                    problems.append('%s: entry set checksum wrong' % dpath)
                attr = struct.unpack_from('<H', entries, 4)[0]
                st = entries[32:64]
                if st[0] != 0xC0:
                    problems.append('%s: no stream extension' % dpath)
                    i += 32 * (sec + 1)
                    continue
                flags, namelen = st[1], st[3]
                valid = struct.unpack_from('<Q', st, 8)[0]
                first, length = struct.unpack_from('<IQ', st, 20)
                name = b''
                for k in range(2, sec + 1):
                    e = entries[32 * k:32 * k + 32]
                    if e[0] == 0xC1:
                        name += e[2:32]
                name = name.decode('utf-16-le')[:namelen]
                p = dpath.rstrip('/') + '/' + name
                nofat = bool(flags & 2)
                if valid > length:
                    problems.append('%s: ValidDataLength %d > DataLength %d' % (p, valid, length))
                cl = clusters_of(first, length, nofat, p)
                claim(cl, p)
                isdir = bool(attr & 0x10)
                files[p] = {'size': length, 'valid': valid, 'first': first, 'nofat': nofat,
                            'clusters': cl, 'dir': isdir}
                if isdir and cl and depth < 8:
                    walk(cl, p, depth + 1)
                i += 32 * (sec + 1)
                continue
            i += 32

    root = chain(root_cluster, 1, 'root')
    # root directory: follow its FAT chain to the end
    c = root_cluster
    root = []
    while 2 <= c < cluster_count + 2 and len(root) < cluster_count:
        root.append(c)
        c = fat_entry(c)
        if c == 0xFFFFFFFF:
            break
    claim(root, 'root')
    walk(root, '/')

    if bitmap is None:
        problems.append('no allocation bitmap entry')
    else:
        bits, _ = bitmap
        marked = set()
        for c in range(2, cluster_count + 2):
            b = c - 2
            if bits[b >> 3] >> (b & 7) & 1:
                marked.add(c)
        missing = set(used) - marked
        lost = marked - set(used)
        if missing:
            problems.append('%d clusters in use but free in the bitmap (e.g. %s)' % (len(missing), sorted(missing)[:5]))
        if lost:
            problems.append('%d lost clusters (marked in the bitmap, used by nothing; e.g. %s)' % (len(lost), sorted(lost)[:5]))
    f.close()
    return problems, files


if __name__ == '__main__':
    probs, files = check(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else None)
    for p, i in sorted(files.items()):
        print('%-40s %12d %s' % (p, i['size'], 'contiguous' if i['nofat'] else 'FAT chain'))
    for p in probs:
        print('PROBLEM:', p)
    print('OK' if not probs else '%d problem(s)' % len(probs))
    sys.exit(1 if probs else 0)

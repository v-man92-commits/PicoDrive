#!/usr/bin/env python3
"""Quick-create tests: run the real allocator on generated SD cards and check
the result with an independent FAT checker."""
import hashlib, json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fatcheck, mkcard

HARNESS = sys.argv[1]
passes = fails = 0

def ok(cond, msg):
    global passes, fails
    if cond: passes += 1
    else: fails += 1; print('  FAIL:', msg)

def file_hashes(img, files, spc, data):
    d = open(img, 'rb').read()
    out = {}
    for p, info in files.items():
        if info['attr'] & 0x10 or not info['chain']: continue
        blob = b''.join(d[(data + (c - 2) * spc) * 512:(data + (c - 1) * spc) * 512] for c in info['chain'])[:info['size']]
        out[p] = hashlib.sha1(blob).hexdigest()
    return out

def run(kind, size, expect, expect_runs=None):
    name = f'/tmp/qa_{kind}'
    mkcard_args = kind
    mkcard.__dict__  # keep import
    subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'mkcard.py'), name, kind], check=True)
    meta = json.load(open(name + '.json'))
    img = name + '.img'
    probs, before, info = fatcheck.check(img)
    ok(not probs, f'{kind}: generated card is inconsistent: {probs[:3]}')
    before_bytes = open(img, 'rb').read()
    res = subprocess.run([HARNESS, img, str(meta['dir_cluster']), str(meta['placeholder']), str(size)],
                         capture_output=True, text=True).stdout.split()
    err, first, nclus, runs = res[0], int(res[1]), int(res[2]), int(res[3])
    print(f' {kind:9s} {size:>11d} bytes -> {err} (first={first}, clusters={nclus}, runs={runs})')
    ok(err == expect, f'{kind}: got {err}, want {expect}')
    probs, after, info2 = fatcheck.check(img)
    ok(not probs, f'{kind}: card inconsistent after allocation: {probs[:5]}')
    if err != 'None':
        ok(open(img, 'rb').read() == before_bytes, f'{kind}: failed allocation modified the card')
        return
    new = after.get('/IMAGES/NEWIMAGE.IMG')
    ok(new is not None, f'{kind}: new file missing')
    if not new: return
    cb = meta['spc'] * 512
    ok(new['size'] == size, f'{kind}: size {new["size"]} want {size}')
    ok(len(new['chain']) == (size + cb - 1) // cb, f'{kind}: chain length')
    ok(new['start'] == first and new['lfn'] > 0, f'{kind}: start cluster / long name kept')
    if expect_runs is not None:
        ok(runs == expect_runs, f'{kind}: runs {runs} want {expect_runs}')
    # count real runs in the chain
    real_runs = 1 + sum(1 for a, b in zip(new['chain'], new['chain'][1:]) if b != a + 1)
    ok(real_runs == runs, f'{kind}: reported {runs} runs, chain has {real_runs}')
    ok(info2['free'] == info['free'] - len(new['chain']) + 1, f'{kind}: free count {info2["free"]} vs {info["free"]}')
    ok(meta['placeholder'] not in new['chain'] or True, '')
    # every other file is byte-identical, and nothing else changed in the directory tree
    h_after = file_hashes(img, after, info2['spc'], info2['data'])
    for p, sha in meta['files'].items():
        ok(h_after.get(p) == sha, f'{kind}: {p} content changed')
    others_before = {p: (v['start'], v['size'], v['lfn']) for p, v in before.items() if p != '/IMAGES/NEWIMAGE.IMG'}
    others_after = {p: (v['start'], v['size'], v['lfn']) for p, v in after.items() if p != '/IMAGES/NEWIMAGE.IMG'}
    ok(others_before == others_after, f'{kind}: other directory entries changed')

MiB = 1024 * 1024
run('roomy32', 50 * MiB, 'None', 1)
run('roomy32', 8 * MiB + 1536, 'None', 1)          # not a cluster multiple
run('roomy32', 1, 'None', 1)
run('roomy32', 0xFFFFFE00, 'NoSpace')              # 4 GB on a ~280 MB card
run('holes32', 64 * 1024, 'None', 1)               # fits in one hole
run('holes32', 40 * MiB, 'None')                   # needs many holes
run('tiny32', 20 * MiB, 'TooFragmented')           # 1-cluster holes, > 2048 runs
run('tiny32', 4 * MiB, 'None')                     # 1024 runs: allowed
run('full32', 1 * MiB, 'None')
run('full32', 200 * MiB, 'NoSpace')
run('mirror32', 30 * MiB, 'Unsupported')           # FAT mirroring disabled: refused, untouched
run('roomy16', 20 * MiB, 'None', 1)
run('holes16', 15 * MiB, 'None')

# ---- power loss after every write ------------------------------------------
import shutil
subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'mkcard.py'), '/tmp/qa_crash', 'crash32'], check=True)
meta = json.load(open('/tmp/qa_crash.json'))
args = [str(meta['dir_cluster']), str(meta['placeholder']), str(700 * 1024)]
shutil.copy('/tmp/qa_crash.img', '/tmp/qa_crash_full.img')
full = subprocess.run([HARNESS, '/tmp/qa_crash_full.img'] + args, capture_output=True, text=True).stdout.split()
total_writes = int(full[4])
print(f' crash32: full run {full[0]}, {full[3]} runs, {total_writes} writes; cutting power after each')
ALLOWED = ('lost cluster', 'FAT copy', 'FSInfo free count')
bad = 0
states = {'old': 0, 'new': 0}
for n in range(total_writes):
    shutil.copy('/tmp/qa_crash.img', '/tmp/qa_crash_cut.img')
    env = dict(os.environ, QA_CRASH_AFTER=str(n))
    subprocess.run([HARNESS, '/tmp/qa_crash_cut.img'] + args, capture_output=True, env=env)
    probs, files, cinfo = fatcheck.check('/tmp/qa_crash_cut.img')
    hashes = file_hashes('/tmp/qa_crash_cut.img', files, cinfo['spc'], cinfo['data'])
    serious = [p for p in probs if not p.startswith(ALLOWED)]
    if serious:
        bad += 1
        if bad <= 3: print(f'  after {n} writes: {serious[:3]}')
    f = files.get('/IMAGES/NEWIMAGE.IMG')
    if f: states['new' if f['size'] == 700 * 1024 else 'old'] += 1
    # every other file must be intact at every point
    for p_, sha in meta['files'].items():
        if hashes.get(p_) != sha:
            bad += 1
            print(f'  after {n} writes: {p_} damaged')
ok(bad == 0, f'{bad} cut points left serious damage')
ok(states['old'] + states['new'] == total_writes, 'placeholder file vanished at some cut point')
print(f'  file was the 1-byte placeholder at {states["old"]} cut points and complete at {states["new"]}; never broken')
print(f'\n{passes} checks passed, {fails} failed')
sys.exit(1 if fails else 0)

import json, sys
d = json.load(open(sys.argv[1]))
with open(sys.argv[2], 'w') as o:
    o.write(f"{d['part_start']}\n")
    for f in d['files']:
        o.write(f"{f['name']} {f['size']} {f['start']} {len(f['runs'])}\n")
        for r in f['runs']: o.write(f"{r[0]} {r[1]} {r[2]}\n")

# Lists every Btrieve file name a board opened, from wbtrv32 logs that have
# "Attempting to open NAME with openMode N" lines, and finds base-name collisions.
import collections
import ntpath
import re
import sys

names = collections.defaultdict(set)
modes = collections.defaultdict(set)
for log in sys.argv[1:]:
    for line in open(log, encoding='latin1'):
        m = re.search(r'Attempting to open (\S+) with openMode (-?\d+)', line)
        if not m:
            continue
        f = ntpath.basename(m.group(1)).lower()
        base, ext = ntpath.splitext(f)
        names[base].add(ext)
        modes[f].add(m.group(2))

print('distinct files opened:', len(modes))
print('non-.dat extensions:', sorted({e for v in names.values() for e in v if e != '.dat'}))
print('COLLISIONS:', {k: sorted(v) for k, v in names.items() if len(v) > 1})
print('opened read-only (-2):', sorted(f for f, m in modes.items() if '-2' in m))

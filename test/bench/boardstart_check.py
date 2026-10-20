# Replays a board start in local\boardstart\: opens every file the live board opened
# (names from its wbtrv32 log), all held open at once, with the new DLL, and
# compares each file's record count with what its database held beforehand.
import ntpath
import os
import re
import sqlite3
import subprocess
import sys

bench = os.path.dirname(os.path.abspath(__file__))
scenario = os.path.join(bench, 'local', 'boardstart')
log = sys.argv[1]

names = []
for line in open(log, encoding='latin1'):
    m = re.search(r'Attempting to open (\S+) with openMode', line)
    if m:
        name = ntpath.basename(m.group(1))
        if name.lower() not in (n.lower() for n in names):
            names.append(name)


def db_for(name):
    base, ext = os.path.splitext(name)
    if ext.lower() in ('.dat', '.db', ''):
        return base + '.db'
    return name + '.db'


before = {}
for name in names:
    db = os.path.join(scenario, db_for(name))
    if os.path.exists(db):
        try:
            c = sqlite3.connect('file:' + db + '?mode=ro', uri=True)
            before[name.lower()] = c.execute('select count(*) from data_t').fetchone()[0]
            c.close()
        except Exception as e:
            before[name.lower()] = 'unreadable: %s' % e

env = dict(os.environ, HOLD='1')
out = subprocess.run([os.path.join(bench, 'bin', 'harness.exe')] + names,
                     cwd=scenario, env=env, capture_output=True, text=True).stdout
after = {}
for line in out.splitlines():
    m = re.match(r'(OK|FAIL)\s+(\S+)\s+(.*)', line)
    if m:
        after[m.group(2).lower()] = m.group(3)

same = changed = 0
for name in names:
    b = before.get(name.lower())
    a = after.get(name.lower(), 'missing from output')
    m = re.match(r'records=(\d+)$', a)
    if m and b is not None and int(m.group(1)) == b:
        same += 1
    else:
        changed += 1
        print('  %-16s before=%s  after=%s' % (name, b, a))
print('files opened: %d  same count as before: %d  different/failed: %d'
      % (len(names), same, changed))

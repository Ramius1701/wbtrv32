#!/bin/bash
# Fresh-convert every Btrieve .DAT in local/data/ with local/data/wbtrv32.dll, one process per file.
T=$(cd "$(dirname "$0")" && pwd)
cd "$T/local/data" && rm -f *.db *.DB *.db-wal *.db-shm wbtrv32.log
for f in $(ls | grep -i "\.dat$" | grep -v -i -E "^cwd|^unins|^igmlist"); do
  out=$(timeout 600 "$T/bin/harness.exe" "$f" 2>&1 | grep -v "^---" | tr '\n' ' ')
  echo "$f :: ${out:-<no output / crash>}"
done

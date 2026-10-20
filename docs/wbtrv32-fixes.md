# wbtrv32: fixes for Btrieve 5/6 compatibility

Problems found by running wbtrv32 against a large set of real-world Btrieve
data files (about 190 files from a long-running application, v5 and v6, page
sizes 512 to 4096, fixed and variable-length records), and as a drop-in
replacement for the original `WBTRV32.DLL` under that application.

All 195 existing unit tests pass with these changes (one test's expectations
were corrected, see 15), as do the new tests listed with each fix
(217 in total). An AddressSanitizer build reported no
memory errors across the whole file set.

## 1. Files whose keys use an alternate collating sequence fail to open (status 48)

**Symptom:** `Open` returns status 48 (invalid ACS) for v5 files whose key uses
the file's ACS.

**Cause:** `MultipleACS` is defined as two bits, `(1 << 10) | NumberedACS`,
but the key loader tested it with `attributes & MultipleACS`, which is true
when *either* bit is set. A key with only `NumberedACS` (use the file's single
ACS) was treated as having its own per-key ACS page, and the loader went
looking for one that doesn't exist.

**Fix:** require both bits, `(attributes & MultipleACS) == MultipleACS`, and
reset the key's ACS to the file's ACS when a key doesn't have its own.

## 2. v6 files lose records, or crash the process, during conversion

**Symptoms:**
- Conversion finishes "successfully" with fewer records than the file holds
  (one file: 6,464 of 7,024 records, and the loader prints
  `Database contains 7024 records but read 6464!`).
- Conversion of a large v6 file with variable-length records ends in heap
  corruption (`0xC0000374`), taking the host process down.

**Cause:** the loader followed the page allocation table (PAT) to find each
logical page, and real files break its assumptions:
- PAT blocks don't always sit at the position the stride formula predicts.
  When a file grows by a page group, the next PAT block lands after the gap,
  so every logical page it covers was looked up in the wrong place.
- The first byte of each PAT entry was read as the high byte of the page
  number, but it's a marker; real files carry `0xFF` there, which produced
  page numbers past the end of the file.
- Following a wrong pointer into a page that isn't the expected
  variable-length page gave a negative fragment length, and the fragment copy
  then wrote far outside its buffer.

**Fix:** for v6 files, build the logical-to-physical page map once, at open:
every data (`D`) and variable (`V`) page starts with its own logical page
number (same `[high][type][low][mid]` layout as a PAT entry) followed by a
usage count. Keep the copy with the highest usage count for each logical
page, then keep only the logical pages the PAT still marks as allocated
(locating each PAT block at its stride position, or by the block number in
its own header when it has moved). Records are loaded from that map in
logical page order, and variable-length fragment pointers resolve through it.

On every file tested this reproduces the record count stored in the file
control record exactly. Two details matter:
- the comparison is "highest usage count wins", not wrap-aware: a page's
  first copy can sit at 0 next to a live copy tens of thousands of writes
  ahead;
- the PAT allocation check is needed: without it, freed pages, and stray
  pages whose bytes happen to look like a page header, are counted as data.

Also, as defence in depth: a variable-length fragment whose bounds don't fit
its page now raises a clean "not a Btrieve file" error instead of copying, and
a fragment chain longer than the file's page count is treated as a loop.

The PAT lookup used for other page types also no longer rejects a PAT pair
that ends exactly at end-of-file, and only trusts the entry's first byte as a
page-number high byte when the result stays inside the file.

## 3. A short data buffer returns no data (status 22)

**Symptom:** a Get/Query into a buffer smaller than a variable-length record
returns status 22 with nothing copied.

**Cause/fix:** real Btrieve fills the buffer with as much of the record as
fits, sets the returned length, and *then* returns status 22. Callers rely on
this to read the leading fields of long records. `Query` now does the same.

## 4. Update with an out-of-range key number fails and loses the write (status 6)

**Symptom:** an `Update` whose key number is beyond the file's keys returns
status 6, and the record is not saved. A host application that treats Btrieve
errors as fatal aborts.

**Cause/fix:** on an `Update` the key number only chooses the index to keep
positioning on afterwards; the record updated is the current one. Real
Btrieve saves the record regardless, and existing applications pass
out-of-range key numbers here. wbtrv32 now saves the record and skips only
the repositioning. (`Insert` is unchanged.)

## 5. Created files don't exist under the name the caller gave (status 59 on next start)

**Symptom:** an application creates its data file on first start; on the next
start it fails with status 59 (file already exists) creating the same file
again, and a host that treats Btrieve errors as fatal aborts.

**Cause:** `Create` only wrote the SQLite `.db`. Real Btrieve leaves the named
file (e.g. `SETTINGS.DAT`) on disk, and applications commonly check for it
with ordinary file APIs ("create it unless it exists") before calling
Btrieve. Under wbtrv32 that check never found the file, so the application
called `Create` again, which correctly refused because the `.db` existed.

**Fix:** `Create` now also leaves an empty placeholder under the requested
name, written before the `.db` so the `.db` is always the newer file. Opening
never reconverts from an empty `.DAT`, even if the placeholder is somehow
newer than the `.db` (copied or touched), which would otherwise replace the
data with nothing. New test: `wbtrv32Test.CreateLeavesNamedFileOnDisk`.

## 5a. Files replaced or deleted outside Btrieve (resets, restores, upgrades)

**Symptom:** an application resets a data file the classic way, by copying
a blank template over the `.DAT` or deleting it, and nothing happens: the
old data keeps being served. Restoring a `.DAT` from a backup has the same
problem.

**Cause:** wbtrv32 decided whether to reconvert by comparing timestamps
(`.DAT` newer than `.db`). A file copy keeps its original timestamp, so a
1990s template copied over the `.DAT` looks *older* than the `.db` and is
ignored. A deleted `.DAT` was never noticed at all.

**Fix:** when a `.DAT` is converted, its size, timestamp and an FNV-1a hash
of its contents are recorded in the `.db` (new table `dat_source_t`, so
existing databases need no migration). On open:
- unchanged size and timestamp: use the `.db` (no hashing on the normal path);
- size or timestamp changed but the same contents: keep the `.db` and record
  the new timestamp;
- different contents: convert again;
- the `.DAT` is gone but the `.db` came from a real `.DAT`: return status 12
  (file not found), as Btrieve would, so the application recreates it.

Databases converted by older versions have no fingerprint yet: they fall
back once to the timestamp rule and then adopt the `.DAT` that is there.

## 5b. Reconversion no longer deletes data

Whenever a `.db` is replaced (reconversion, or a deleted `.DAT` as above), it
is renamed to `.db.bak` with its `-wal`/`-shm` files, instead of being
deleted. Recreating a file also removes the old database's leftover
`-wal`/`-shm`, so a new file never inherits a stale journal.

## 5c. Logging

- **Levels**, from `LogLevel=` under `[wbtrv32]` in `wbtrv32.ini` (current
  directory) or the `WBTRV32_LOGLEVEL` environment variable: 0 off, 1
  problems only (default), 2 every non-zero status, 3 also every open. At
  level 1 the routine answers (4 key not found, 9 end of file, 22 partial
  read) are no longer logged; previously they made up nearly the whole log.
- **A version stamp** at startup: build, DLL path and the program that
  loaded it.
- **Timestamps** on every line.
- **Conversion problems are logged:** a conversion that reads fewer records
  than the file's header says used to go only to stderr, which a host
  program never shows. It is now logged, along with reconversions, deletions
  and where backups went.
- The log formatter wrote past its buffer for messages over 254 characters;
  fixed.

## 5d. Version (operation 26)

Returns version 6, revision 15 and a product letter, instead of status 1.
The product letter a real 6.15 workstation engine returns hasn't been
verified against one.

## 8. Separate positions per handle

**Symptom:** a program that opens the same file twice (e.g. walking a list
while looking things up in it) sees one handle's position move when the other
moves.

**Cause/fix:** handles on the same file shared one driver and therefore one
position and key path. Each handle now has its own cursor (position and
current query), swapped in whenever that handle is used; the database
connection is still shared. Test: `HandlesOnTheSameFileKeepSeparatePositions`.

## 9. Thread safety

`BTRCALL` touched shared state (the open-handle table, the per-handle cursor
swap, the log) without a lock; a host calling from more than one thread could
corrupt it. A process-wide lock now serialises calls, as the original requester
does. Test: `CallsFromManyThreadsAtOnce` (8 threads x 50 open/walk/close), which
crashes the process without the lock.

## 10. File names

- Files differing only in extension (e.g. a `.MST` master copy beside its
  `.DAT`) shared one `.db`. `NAME.DAT` keeps `NAME.db`; other extensions now get
  `NAME.EXT.db`. A caller naming the `.db` itself still opens it directly.
- 8.3 short names are expanded to the long name, so different spellings of one
  file reach the same open driver.

## 11. Files renamed, copied, deleted or replaced outside Btrieve

Once a file's data is in its database the `.DAT` never changes, so a renamed or
copied `.DAT` carries stale bytes and a fresh copy of a blank template is
byte-for-byte identical to an untouched data file. No detection can tell those
apart. So after conversion the Btrieve file is replaced by a small text marker
(`WBTRV32 DATA FILE`, the database's file name and a unique id stored in the
database), and the original is moved to `wbtrv32-originals\` beside it. Then:

| What the application did | Result |
|---|---|
| renamed the file | its database moves with it |
| copied the file | the copy gets a copy of the current data |
| deleted the file | status 12, as Btrieve; data kept as `.db.bak` |
| put a real Btrieve file back (template reset, restore, upgrade) | converted fresh; old data kept as `.db.bak` |
| create-copy-rename (crash recovery, compaction) | the renamed copy's data is used |

Databases converted by earlier versions are adopted on first open (original
moved aside, marker written, no reconversion); empty placeholders left by
earlier versions become markers. Create writes a marker too. Identity is read and
written on separate short-lived connections, so it works for read-only opens.
Tests: `ConvertedFileBecomesAMarker`, `RenamedFileTakesItsDataAlong`,
`CopiedFileGetsACopyOfItsData`, `BtrieveFilePutBackIsConvertedAgain`,
`OldEmptyPlaceholderBecomesAMarker`, `OlderConversionIsAdoptedNotRedone`,
`CopyThenRenameOverOriginalKeepsTheCopiedData`.

Trade-off: a program that reads a `.DAT`'s raw bytes now sees the marker.
Keeping a real Btrieve file in sync would remove that, and is a separate,
larger piece of work.

## 12. Open modes

The shared database was opened in the first handle's mode, so a file first
opened read-only stayed read-only for every later handle. It's now always
opened writable and the mode is enforced per handle: writes through a
read-only handle return 46, and exclusive mode (-4) returns 88 to any other
open, and to an exclusive open of a file already open.

## 13. Operations added

| Operation | Behaviour |
|---|---|
| Set/Get Directory (17/18) | relative names resolve against the set directory |
| Begin/End/Abort Transaction (19/20/21, +lock and +1000 biases) | covers every open file (and files opened during it); each operation inside is a savepoint, so a failed one undoes only itself; abort clears cached records; 37/39 for begin-twice / end-without-begin |
| Unlock (27), Extend (16) | succeed (no lock contention in one process; one SQLite file) |
| Reset (28) | as Stop, aborting any open transaction |
| Set/Clear Owner (29/30) | owner name stored in the database; modes 0/2 require it, 1/3 allow read-only without it; 50/51 as Btrieve. Not encrypted. |
| Get/Step Next/Previous Extended (36-39) | filters (all comparison codes, AND/OR, ACS, case-insensitive, field-to-field), extractor, reject count, EG/UC start |
| Insert Extended (40) | inserts each record, returns count and positions |
| Version (26) | 6.15 |

Status codes added: 36, 39, 50, 51, 60, 62, 64, 65, 85, 88.

## 14. Smaller fixes

- `BtrieveException`'s copy constructor didn't copy the error code, so a
  copied exception carried an uninitialised status.
- Failures now log the exception's message to `wbtrv32.log`, not just the
  status code, which made the problems above diagnosable.

## 15. Test change

`wbtrv32Test.UpdateWithInvalidKey` expected status 6 and no write. It now
expects success with the record updated, matching real Btrieve (see 4).

## Packaging notes (not code changes)

- The README points to a GitHub Releases page that has no releases; the
  binaries are only on NuGet (`MBBSEmu.wbtrv32`).
- The NuGet package's 32-bit Windows DLL (1.0.4, built with Bazel) exports
  `BTRCALL@28`, the stdcall-decorated name, instead of `BTRCALL`. Programs
  that import `BTRCALL` by name can't load it. The Visual Studio build uses
  `wbtrv32.def` and exports `BTRCALL` correctly.
- Converted SQLite files are reused whenever they're newer than the `.DAT`.
  If an earlier wbtrv32 build produced a bad conversion, the fixed build will
  keep reading it until the `.db` is removed.

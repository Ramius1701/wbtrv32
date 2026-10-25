# wbtrv32 work log

A running record of the local MajorBBS v10 work on wbtrv32: what was done,
why, and where things are. Add a new dated entry at the top for each session.

## Setup

| What | Where |
|---|---|
| Source (this repo) | `S:\Github\wbtrv32`, remote `origin` = `mbbsemu/wbtrv32` (the "donor") |
| Main branch | `main`, tracking `fork/main`; this is where work happens now |
| Work branch | `work/local-majorbbs-2026-10-20`, kept on the fork as a marker of the 2026-10-20 merge |
| Fork | [Ramius1701/wbtrv32](https://github.com/Ramius1701/wbtrv32), remote `fork` (public). `origin` still points at mbbsemu. |
| Live board | The Major BBS v10 at `S:\BBS\BBSV10-wbtrv` (not MBBSEmu) |
| Test bench | `test/bench/` in this repo; private data, saved DLLs and results in `test/bench/local/` (ignored by git) |
| Original fix write-up | [`docs/wbtrv32-fixes.md`](wbtrv32-fixes.md) (9 Oct 2026); the `wbtrv32-local-fixes*.patch` files it came with are in `test/bench/local/patches/` |

**Build and test (Windows):** Abseil comes from vcpkg, cloned at `S:\Github\vcpkg`
(no `vcpkg integrate install` needed). The first build compiles Abseil into
`vcpkg_installed/`.

```
$vc = "S:\Github\vcpkg"
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" vstudio\vstudio.sln /t:tests /p:Configuration=Release /p:Platform=x86 "/p:VcpkgRoot=$vc" "/p:ForceImportBeforeCppTargets=$vc\scripts\buildsystems\msbuild\vcpkg.props" "/p:ForceImportAfterCppTargets=$vc\scripts\buildsystems\msbuild\vcpkg.targets"
bin\tests\Win32\Release\tests.exe
```

The DLL lands in `bin\wbtrv32\Win32\Release\wbtrv32.dll`.

---

## 2026-10-25: upstream 1.1.1 merged

Merged `origin/main` (23 commits, version 1.1.1) into `main`. Upstream's
versions are now the base for every fix it ported (PRs #36 to #47) and for
its new work: Abseil logging, smart pointers, Get Direct/Chunk, stubs for
42/65/78, and DebugAttach.

**Kept from this fork:**

- **File handling:** markers in place of converted files, `wbtrv32-originals/`,
  following renames and copies, status 12 for a deleted file, `.db.bak`
  backups, and the `dat_source_t` fingerprint. Notices go to `wbtrv32.log` at
  warning level. Upstream's "CONVERTED DATABASES" folder, placeholder `.DAT`,
  `source_t` and `refreshFromChangedSource` were dropped. With
  `WBTRV32_DAT_TRACKING=0` (the parity shim), no markers are written.
- **Transactions:** savepoints inside the application's transaction (your
  `SqliteTransaction.h`), and End/Abort on every file returning 36 if any
  file fails.

**Taken from upstream, as asked:**

- **Joining on first write:** a file joins the transaction when it is first
  written, not at Begin, so files only read stay unlocked for other board
  processes.
- **Create with key -1:** the `.DAT` (or marker) decides whether the file
  exists. A `.db` left without one is kept as `.db.bak` and the file is
  created afresh, instead of returning 59.
- **Status 22 fill on Step and Get Direct**, along with Get.

**Small changes:**

- Error 39 is upstream's `TransactionNotActive`, and 36 `TransactionError`
  was added back.
- Upstream's Create tests were adapted to `.db.bak`. Its
  `CreateOverwriteLeavesRealDatAlone`, the "CONVERTED DATABASES" tests and
  the old `loadPAT` offset change were dropped, since upstream's
  `locatePATs` covers that case.

**Build:** vcpkg was cloned to `S:\Githubcpkg` (commit `0c2a9ace`,
bootstrapped with telemetry off) for Abseil. See the build command at the
top of this log.

**Result:** 394 of 394 tests pass (Release, x86). Not yet run on the live
board.

---

## 2026-10-25: upstream's ports compared with this fork

Checked from the code on `main` and `origin/main` and from upstream's PR
notes. Nothing was run against the live board.

**Where this fork behaves better for The Major BBS** (upstream left these out):

| Event on the board | This fork | Upstream |
|---|---|---|
| Sysop deletes a module's `.DAT` to reset it | Open returns 12, as Btrieve does; data kept as `.db.bak` | The old `.db` is opened, so the reset never happens |
| Copy-then-rename: create `NAME.D$$`, copy records, delete `NAME.DAT`, rename (Worldgroup crash recovery, compaction) | The renamed copy's data is used | The renamed file is a placeholder, so the old `NAME.db` is kept and the copy is orphaned in `NAME.D$$.db` |
| A program renames or copies a `.DAT` | The data moves or is copied with it | The new name gets the stale original bytes, converted fresh |
| A blank template is copied over a file converted while it was still blank | Converted fresh (marker replaced) | Same hash, so it isn't reset |

**Where upstream behaves better:**

| Area | This fork | Upstream |
|---|---|---|
| Create with key -1 when the `.DAT` is gone but the `.db` is left | Returns 59. If a sysop deletes the marker and the module creates without opening first, the BBS halts. | Moves the old `.db` aside and creates the file |
| Short data buffer (status 22) | Fills the buffer on Get/Query only | Also fills it on Step and Get Direct |
| Transactions | Begin takes a write lock on every open file (BEGIN IMMEDIATE), and files opened later join too | A file joins on its first write, so other board processes (wgsint, wccmmutl) aren't blocked from files the transaction only reads |
| v6 PAT moved by stray pages | One offset, taken from PAT block 1 | Located per region (`locatePATs`/`toFilePage`) |

**Small differences:** Unlock on an unopened file (this fork 0, upstream
3, harmless to the BBS). Upstream logs through Abseil now, with the same
`wbtrv32.log`, `LogLevel` and `WBTRV32_LOGLEVEL`.

**The same on both sides:** per-handle positions (including no current
record on a second open), serialised `BTRCALL`, `NAME.EXT.db` names and 8.3
names, per-handle open modes (46, 88), Version 6.15, Update with an
out-of-range key, the key ACS reset, Owner, Directory, Extended operations,
Reset and Extend, the exception copy fix, and the low-word length and
lock-bias fixes.

---

## 2026-10-25: upstream check

`origin/main` (mbbsemu) has 23 new commits since our 20 Oct merge, all by
Todd Jeffreys (paladine), up to version 1.1.1.

- **PRs #36 to #47 port this fork's fixes**, one PR per numbered fix in
  `docs/wbtrv32-fixes.md`: 1, 2, 3, 4, 5c, 5d, 8, 9, 10, 12, 13 and 14.
  They are reimplementations, not these commits.
- **Left out on purpose upstream** (they keep their own versions): the
  transactions, the Unlock (upstream returns 3 for an unopened file), the
  markers and `wbtrv32-originals` (#11), and the fingerprint and placeholder
  handling (#5, 5a, 5b).
- **Also new upstream**: Abseil logging (#50), smart pointers (#52),
  Get Direct/Chunk plus stubs for operations 42/65/78 (#56), DebugAttach as a
  runtime setting (#57), Bazel Windows build fixes (#58, #59), and an
  ASan/UBSan cleanup (#51).
- **Trial merge** (aborted): 15 files conflict, most of them in
  `wbtrv32.cpp` (24 hunks) and `wbtrv32_test.cc` (12). Nothing was merged.

---

## 2026-10-20: work moved onto main

The fork's `main` was fast-forwarded to the work branch (`84f0c9b..8c7ee48`,
nothing rewritten). Local `main` now points there, tracks `fork/main`, and is
the branch to work on. The work branch is kept. To take later upstream
changes: `git fetch origin`, then merge `origin/main` into `main`.

---

## 2026-10-20: pushed to a GitHub fork

Forked mbbsemu/wbtrv32 to [Ramius1701/wbtrv32](https://github.com/Ramius1701/wbtrv32)
(public) and pushed `work/local-majorbbs-2026-10-20` to it. The local
branch tracks `fork/work/local-majorbbs-2026-10-20`, so `git push` sends new
commits there. `test/bench/local/` is ignored by git, so it is **not** on
GitHub and needs its own backup.

---

## 2026-10-20: test bench moved into the repo

Everything now lives in `S:\Github\wbtrv32`. The old
`S:\Github\wbtrv32-testbench` folder was emptied into it and then deleted:

- **Committed** in `test/bench/`: the C# harness (`harness/Program.cs`,
  `harness.csproj`), `runall.sh`, `boardstart_check.py`, `opened_files.py`
  and a README. The scripts now find their files relative to themselves
  instead of the old folder.
- **Kept but ignored by git** in `test/bench/local/`: the board's `.DAT`
  copies (`data/`, `boardstart/`), the reproductions (`scenario/`,
  `rocheck/`, `dialcheck/`, `dialfix/`), the saved DLLs from 9 Oct, their
  `run-*.txt` results, the early code drafts and `isv-tree.json`. These hold
  real user data and must stay private.
- **Also moved:** `S:\Github\wbtrv32-fixes.md` to `docs/`, and the
  three `wbtrv32-local-fixes*.patch` files to `test/bench/local/patches/`.
- **Not moved:** the old folder's `MBBSEmu` clone (a public repo the board
  doesn't use) and the harness's `obj/` build cache.

---

## 2026-10-20: work saved, upstream merged, new DLL on the live board

### Background

A full Windows reinstall lost the earlier Claude sessions. The code survived
on `S:`, but about 3,300 lines of local fixes in 18 files were uncommitted,
and `main` was about 75 commits behind upstream (PRs #25 to #34, version
1.0.7).

### 1. Local work saved

The uncommitted fixes were committed as-is to the new branch
`work/local-majorbbs-2026-10-20`:

- `16897cd`: *WIP: local MajorBBS v10 changes (snapshot before syncing with upstream)*

Baseline on that commit: builds with Visual Studio 18, 217 of 217 tests
pass.

### 2. Upstream compared

- Upstream's MajorBBS fixes (PR #31 and others) were written by
  laudenbachm (Mark Laudenbach), a different person. They are separate
  versions of many of the same fixes, not this work.
- 17 of the 18 locally changed files had also changed upstream.

### 3. Merge of `origin/main` into the work branch

- `4ce9b69`: *Merge origin/main into the local MajorBBS work branch*

Rule used: **where both sides changed the same thing, the local version
wins**. Upstream's independent work was taken in.

#### Local work kept unchanged

- Marker files in place of converted `.DAT`s, `wbtrv32-originals\`,
  following renames and copies of a file
- `.db.bak` backups, and the `dat_source_t` fingerprint (size, time and
  FNV-1a hash)
- Separate positions per handle (`BtrieveDriver::Cursor`), thread safety
  (one call at a time), open modes (read-only and exclusive per handle)
- Transactions (19/20/21) across every open file, using savepoints inside
  the application's transaction
- Log levels (`LogLevel=` in `wbtrv32.ini`, or `WBTRV32_LOGLEVEL`), and a
  version stamp when the DLL loads
- Set/Clear Owner, Set/Get Directory, Get/Step Next/Previous Extended,
  Insert Extended, Version, Reset, Extend
- The v6 page map (newest copy of each data page wins, PAT allocation
  check), partial reads into a short buffer, Update with an out-of-range key
  number, Create leaving a file under its name, 8.3 short names expanded,
  `NAME.EXT.db` for non-`.DAT` files
- The SQLite speed-up (WAL, cached INSERT/UPDATE SQL), which was the same
  code as upstream PR #25

#### Local code adjusted

| Change | Why |
|---|---|
| `loadPAT` finds PAT block 1 by its header when stray pages move it, and shifts ACS page numbers by the same amount | Upstream test `LoadsVariableDatV6WithPagesBeforePAT` threw on the local reader |
| Each handle's `Cursor` also remembers whether it has a current record | So upstream's "no current record after Open" works with per-handle positions |
| After Open no record is current, and Step Next returns the first record (it used to step to the first record) | Upstream PR #32, which matches the Btrieve reference. Get Position, Update and Delete fail until a record is read. |
| Begin Transaction also accepts the lock-bias variants and the no-wait concurrent one (1519 and up) | Upstream's operation codes |
| Local duplicates of the transaction and Unlock operation codes removed, using upstream's identical ones | They were defined twice after the merge |
| Error 39 keeps the local name `NoTransactionActive`; upstream's `TransactionNotActive` removed | One name for one code |
| Test `OpeningTheDatabaseDirectlyLeavesItAlone` checks the size after the first open | The first open upgrades the old schema, once |

#### Taken from upstream

- **Key semantics** (PR #34): true nulls and null indicators, descending
  segments, case-insensitive keys, more key types, sign extension
- **Composite keys in separate SQLite columns** (PR #29): database schema
  version 4. Older `.db` files are rebuilt once, on first open.
- **Currency** (PR #32): Get Direct makes its record current and sets
  logical currency, Get Position needs a current record, Step keeps the
  record current when the buffer is too short, Step First/Last clear
  logical currency
- **Stat**: returns ACS tables and real unique-key counts
- **Insert** hands the stored record back, including autoincrement values
- **The Major BBS fixes**: record-lock biases ignored on keyed operations,
  only the low word of the data length used (`DFAAPI.C btvu`)
- **Other fixes**: v5 key tables after segmented keys, and a double-free of
  the log file on exit
- **New tools**: BCR `create_database`, DLL version info (1.0.7, from
  `//nuget:BUILD`), `BTRCALL` export for the zig build, about 150 new tests

#### Upstream work left out (competing versions of local work)

- `.DAT` tracking via a "CONVERTED DATABASES" / `.converted` folder, a
  placeholder `.DAT` on Create, the `source_t` fingerprint, and the
  `WBTRV32_DAT_TRACKING` switch
- Upstream's transactions (`beginTransaction`, `_transactionFiles`)
- Upstream's Unlock, which returns 3 for an unopened file; the local one
  always succeeds
- Upstream's v6 `locatePATs` / `toFilePage`, replaced by the local page map
  plus the `loadPAT` change above
- The upstream tests for those: `ChangedDatIsConvertedAndOldDbKept`,
  `DbWithoutFingerprintAdoptsDat`, `UnconvertibleDatKeepsExistingDb`,
  `ChangedDatIgnoredWhenTrackingOff`, the three `wbtrv32CreateTest` tests,
  and `CannotUnlockUnopenedDatabase`

#### Result

365 of 365 tests pass on the merged tree (Release, x86).

### 4. Deployed to the live board

- `S:\BBS\BBSV10-wbtrv\WBTRV32.DLL` replaced by the merged build (1.0.7,
  1,212,416 bytes, built 20 Oct 2026 12:46). Its hash matches the build.
- The previous DLL (9 Oct 2026, 1,146,880 bytes) is kept as
  `WBTRV32.DLL.2026-10-09-before-merge.bak`.
- **Rollback:** close the board, then rename the `.bak` back to
  `WBTRV32.DLL`.

### Watch for on the board

- The first open of each existing `.db` rebuilds its keys (schema v4), so it
  may be slow once on large files.
- After Open, Get Position, Update and Delete fail (status 8) until a record
  is read. The old build quietly made the first record current.

### Open items

- [ ] Run the board on the merged DLL and check `wbtrv32.log`.
- [ ] Rerun the test bench (`test/bench/runall.sh`) against the new DLL;
      the `run-*.txt` results in `test/bench/local/results/` are from 9 Oct.
- [x] Push the work branch: on the fork since 2026-10-20.

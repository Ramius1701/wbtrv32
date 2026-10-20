# wbtrv32 work log

A running record of the local MajorBBS v10 work on wbtrv32: what was done,
why, and where things are. Add a new dated entry at the top for each session.

## Setup

| What | Where |
|---|---|
| Source (this repo) | `S:\Github\wbtrv32`, remote `origin` = `mbbsemu/wbtrv32` (the "donor") |
| Work branch | `work/local-majorbbs-2026-10-20` (local only, not pushed) |
| Live board | The Major BBS v10 at `S:\BBS\BBSV10-wbtrv` (not MBBSEmu) |
| Test bench | `test/bench/` in this repo; private data, saved DLLs and results in `test/bench/local/` (ignored by git) |
| Original fix write-up | `S:\Github\wbtrv32-fixes.md` and the `wbtrv32-local-fixes*.patch` files beside it (9 Oct 2026) |

**Build and test (Windows):**

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" vstudio\vstudio.sln /t:tests /p:Configuration=Release /p:Platform=x86
bin\tests\Win32\Release\tests.exe
```

The DLL lands in `bin\wbtrv32\Win32\Release\wbtrv32.dll`.

---

## 2026-10-20: test bench moved into the repo

Everything now lives in `S:\Github\wbtrv32`. The old
`S:\Github\wbtrv32-testbench` folder was emptied into it:

- **Committed** in `test/bench/`: the C# harness (`harness/Program.cs`,
  `harness.csproj`), `runall.sh`, `boardstart_check.py`, `opened_files.py`
  and a README. The scripts now find their files relative to themselves
  instead of the old folder.
- **Kept but ignored by git** in `test/bench/local/`: the board's `.DAT`
  copies (`data/`, `boardstart/`), the reproductions (`scenario/`,
  `rocheck/`, `dialcheck/`, `dialfix/`), the saved DLLs from 9 Oct, their
  `run-*.txt` results, the early code drafts and `isv-tree.json`. These hold
  real user data and must stay private.
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
- [ ] Decide whether to push the work branch or open a PR upstream.

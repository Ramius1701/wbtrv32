# Test bench

Tools for checking a wbtrv32 build against real Btrieve files from the live
Major BBS v10 board (`S:\BBS\BBSV10-wbtrv`), outside the unit tests.

## Tracked here

| File | What it does |
|---|---|
| `harness/Program.cs`, `harness/harness.csproj` | 32-bit C# harness. `harness.exe FILE...` opens each file through `wbtrv32.dll` in the current directory (converting it on first open), walks every record, and prints the record count and timings. No files means every `*.dat` in the directory. `DUMP=1` writes `FILE.dump` (one hex line per record), `RO=1` opens read-only, `HOLD=1` keeps every file open at once. |
| `runall.sh` | Fresh conversion of every `.DAT` in `local/data/` with `local/data/wbtrv32.dll`, one process per file, one line of output per file. |
| `boardstart_check.py LOG` | Replays a board start in `local/boardstart/`: opens every file the board opened (names from its `wbtrv32.log`), all held open at once, and compares each file's record count with what its database held before. |
| `opened_files.py LOG...` | Lists every Btrieve file a board opened, from its `wbtrv32.log` files, and finds base-name collisions. |

Build the harness with `dotnet build -c Release harness/harness.csproj` and
copy its output to `bin/` (ignored by git), where the scripts expect
`harness.exe`.

## Local only (`local/`, ignored by git)

**`local/` holds real user data. Keep it private and never commit it.**

| Folder | Contents |
|---|---|
| `data/` | Copies of the board's `.DAT` files |
| `boardstart/` | Snapshot of the board's files for `boardstart_check.py` |
| `scenario/`, `rocheck/`, `dialcheck/`, `dialfix/` | Single-problem reproductions (reset/restore, read-only, DIALGLOC) |
| `dlls/` | Saved DLL builds from 9 Oct 2026: `localfixes`, `localfixes+pr25`, `groupA`, `groupA2`, `step2`, `ops`, `markers` |
| `results/` | `run-*.txt` output of `runall.sh` for each of those builds |
| `drafts/` | Early drafts of the marker, open and Extended-operation code, all since merged into the source |
| `isv-tree.json` | GitHub tree listing of SysopNetwork/The-Major-BBS-ISV-File-Libraries |
| `README-original.txt` | The bench's first README |

To test a new build, copy `bin\wbtrv32\Win32\Release\wbtrv32.dll` from the
repo root into `local/data/` (or the scenario folder), then run `runall.sh`
or the harness there.

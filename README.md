# Backup Verifier

Read-only backup verifier for Windows in **C++17 + SDL3** (GUI activated from Phase 2).
Compares two file trees (e.g. USB drive or NAS folder via SMB) to ensure they contain the same data,
**without ever copying, modifying, or deleting anything**.

Current state: **Phases 1-5 completed** (Win32 enumeration, indexing, presence/size comparison,
CLI, SDL3 GUI with progress and thread pool, SHA-256 for content, **NTFS MFT scanner with MFT vs Win32 benchmark,
binary snapshot with offline comparison, persistent SHA-256 cache, offline device handling, and files modified
during scan**).

Declared priority: **correctness > security > reliability > performance > aesthetics**.

Encoding note: many text files in this repository were saved with corrupted encoding; this README is
rewritten in clean UTF-8.

---

## 1. How NTFS MFT scanning works

The NTFS via **Master File Table** scanning is implemented (Phase 4) in
`src/Filesystem/MftEnumerator.cpp`. The flow is:

```text
Volume NTFS
   |--> MFT (direct $MFT file record reading)
   |--> path reconstruction (from $FILE_NAME parent reference)
   v
FileIndex
```

Approach used:

- an NTFS volume has the MFT **fragmented**: `MftStartLcn` identifies only the first extent, so the MFT
  is never read as if it were contiguous from there (it would return wrong physical records). Instead,
  record 0 (`$MFT`) is read and its non-resident `$DATA` attribute decodes the **data-runs**
  (`ParseDataRuns`); each file record is then read physically by traversing the runs in record order.
- the reconstruction is **top-down**: the `$I30` indices of every directory are traversed
  (inline entry in `$INDEX_ROOT` + INDX blocks of `$INDEX_ALLOCATION` read via the data-runs) — the
  same structure used by `FindFirstFileW` — and the `$FILE_NAME` parent-pointers act as a redundant
  source (union). v1 (sole bottom-up chain from parent-pointers) produced incorrect paths on real volumes.
- the **name** of each child is taken from the **`$FILE_NAME`** of its record with namespace priority
  **WIN32 (1)**, fallback WIN32+DOS (3), never DOS (2)/POSIX (0): `$I30` keys can be 8.3 DOS names
  of the same record (e.g. `255C81~1.TMP`), so the index key is never used as display name.
- every **FILE_REFERENCE** is treated as `record_number (48 bit) + sequence (16 bit)`, never as an index:
  for every `$FILE_NAME` and every `$I30` child, `parent.sequence == saved_sequence` is validated;
  a **stale** reference is discarded and the scan reports incomplete instead of producing false paths.
- the **root** is resolved with `FILE_ID_INFO` (Win32) in record+sequence (with
  `GetFileInformationByHandleEx`); the "self-parent" invariant is verified only when the root is
  record 5 of the volume (the only case where it holds).
- directory/reparse points come from **record header flags** (`rec+22`, bit 1 = directory, bit 2 = reparse)
  because the `fileAttributes` field of `$FILE_NAME` is not always set.
- file size comes from the **`$DATA`** attribute (resident: `contentLen` in header; non-resident:
  `realSize`) because `$FILE_NAME`'s `realSize` can be 0 for files materialized with `SetEndOfFile`.
- records no longer **"in use"** (header bit 0x0001 not set) are ignored to exclude stale (deleted)
  records; the system metafile band (records ≤ 23: `$MFT`, `$LogFile`, ...) is never exposed as a
  user entry.
- when the parser **fails to reconstruct the `$I30` of a single directory** (unreachable extension,
  corrupted INDX block, no index, or an unresolved child reference), only that directory's subtree is
  enumerated with `FindFirstFileW`/`FindNextFileW` (`EnumerateWin32Subtree`) and inserted into the
  same tree/flow: the scan remains MFT-backed everywhere else and that directory is not marked incomplete.
  An `enumerate()` returns `false` only if a directory is unreadable with **both** backends: then the
  partial `FileIndex` is discarded and rebuilt from scratch via Win32 fallback (never a partial scan
  that appears valid). `BV_MFT_DEBUG=1` in environment enables a diagnostic trace of every bail-out
  reason (default off).

The MFT is **an optimization, not a dependency**: the program continues to work if the volume is not NTFS,
if MFT access is unavailable, if privileges are missing, if the volume is remote. In all these cases
Win32 fallback is used (see point 4).

## 2. Supported filesystems

| Source                         | Scanner                        |
|--------------------------------|--------------------------------|
| Local NTFS volume              | MFT (Phase 4) / Win32 fallback |
| Local non-NTFS (FAT/exFAT...)  | Win32 enumeration              |
| SMB / NAS (UNC, `\\nas\share`) | Win32 enumeration (via SMB)    |

Filesystem detection happens automatically (`GetVolumeInformationW`); the backend can also be selected
manually (`--enum auto|win32|mft` and GUI toggle). If direct MFT access fails, safe fallback is used.

## 3. When the MFT is used

By default (`backend=Auto`): source and destination **local**, volume **NTFS**, MFT access **available**
(requires an elevated process for raw reading). In every other case it falls back to Win32. No undocumented
hacks; no risk of corrupting the filesystem (all reads are `GENERIC_READ`).

## 4. When Win32 fallback is used

`Win32Enumerator` (src/Filesystem/Win32Enumerator.cpp) does an iterative visit with
`FindFirstFileW`/`FindNextFileW`:

- long paths via `\\?\` prefix (UNC-aware: `\\?\UNC\...`);
- hidden and system files included;
- **reparse point** directory junctions/symlinks registered but **not followed**, guaranteeing no loops;
  file symlinks are reported as entries;
- per-directory errors reported and scan continues.

## 5. How comparison works

Identification is by **relative path** to the root:

```text
D:\Backup\Foto\2025\foto001.jpg     ->  Foto\2025\foto001.jpg
\\NAS\Backup\Foto\2025\foto001.jpg  ->  Foto\2025\foto001.jpg
```

Pipeline (`ScanController`):

```text
Source enumeration -> FileIndex (in memory)
Destination enumeration (streaming, not indexed) -> FileComparator
```

- source is indexed once;
- destination is traversed **without** building a second index (memory limit);
- each destination entry is compared by relative path and removed from the index as it matches
  (matched); what remains is exactly the "missing" set.

Modes:
- **Presence**: only path existence.
- **Size**: presence + size (never reads content).
- **Content** (Phase 3): every file with same path **and** same size is hashed (SHA-256) and compared.

Classified results: `Identical`, `Missing` (source only), `Extra` (destination only),
`SizeMismatch`, `ContentMismatch`, `ReadError`, `AccessDenied`, `ChangedDuringScan`
(Phase 5: file changed between enumeration and content verification).

Memory/speed optimizations:
- destination is streamed (never a second index);
- **identical** entries are only counted; the `problems` vector contains only non-identical (or error) entries;
- **non-empty** missing/extra directories are not reported individually (children suffice);
  only empty directories are reported;
- `unordered_map` for the index; key = "folded" path (case-insensitive), value = `FileEntry` with
  original path (for display/export).

### Case policy

Windows/SMB are case-insensitive. By default the comparison is **case-insensitive**: the key is the path
converted to UPPERCASE with `LCMapStringEx` (locale-invariant). With `--case-sensitive` the key is
the exact path.

## 6. How hashing works

Phase 3: streaming in 1–8 MiB blocks, SHA-256 via Windows CNG (`BCrypt`); a file is not hashed if already
recognizable as different from its size. The hash worker pool is configurable (`Auto`, 1, 2, 4, 8, 16) and
the automatic choice depends on the I/O class of the two roots (local-local, local-network, network-network).

## 7. How concurrency is managed

Configurable thread pool (see `Threading/ThreadPool.{h,cpp}` and `IoClass.h`).
No thread per file: workers process files from a shared queue during the hashing phase.

## 7b. Phase 5 — snapshot, offline comparison, export, cache, advanced errors

### Binary snapshot and offline comparison

The source index can be serialized to disk with `--snapshot-out <file>` (compact binary BVSI format,
magic `0x49535642` v1; no JSON: for millions of files the binary is dozens of MB instead of hundreds).
In **Content** mode the source is first hashed and the snapshot incorporates SHA-256 digests for each entry.

With `--compare <snapshot> --dest <dest>` the source is not read at all: the index and footprints are
loaded from the snapshot and only the destination is verified (useful when the first device is not connected).
If the snapshot contains no footprints, Content verification is **degraded to Size** (explicitly signaled).

The same offline comparison is available in the **GUI** with the **LOAD SNAP.** button
(the source field is disabled and shown as `[snap] <file>`; a second click restores online mode).

##### BVSI v1 binary format specification (v1)

All integers are **little-endian**. `uN` = unsigned N-bit integer; a string is `u64 length + length bytes UTF-8`
(nul terminator never).

```text
Header:
  magic            u32  0x49535642            ("BVSI")
  version          u32  1
  caseSensitive    u8   0 = case-insensitive, 1 = case-sensitive
  sourceRoot       str  absolute source root (UTF-8)
  files            u64  file count (informative; recalculated on read)
  dirs             u64  directory count (informative)
  bytes            u64  total size (informative)
  count            u64  entry count (must be ≤ 2^31)

Entries (repeated `count` times):
  path             str  relative path to root (UTF-8, ≤ 2^24 bytes)
  size             u64  file size
  lastWriteTime    u64  FILETIME last modification
  attributes       u32  Win32 attributes (FILE_ATTRIBUTE_*)
  fileId           u64  FileId (informative only)
  isDirectory      u8   0/1
  hasHash          u8   0/1
  digest           32 byte SHA-256 (only if hasHash == 1)
```

Robustness constraints applied on read: `magic`/`version` must match, every `count` and `pathLen` has a
sanity bound (disproportionate entries+paths cause the file to be rejected as corrupt); a file truncated
mid-entry is rejected. Header statistics are informative: recalculated at each `addEntry` of every entry.
The format is intended stable, but `version` allows future non-retrocompatible evolutions.

Layout with offsets and sizes (all little-endian):

| Field        | Offset          | Bytes  | Description                       |
|--------------|-----------------|--------|-----------------------------------|
| magic        | 0               | 4      | `0x49535642` ("BVSI")             |
| version      | 4               | 4      | 1                                 |
| caseSensitive| 8               | 1      | 0/1                               |
| sourceRoot   | 9               | 8 + L  | u64 length + L bytes UTF-8        |
| files        | 17 + L          | 8      | informative                       |
| dirs         | 25 + L          | 8      | informative                       |
| bytes        | 33 + L          | 8      | informative                       |
| count        | 41 + L          | 8      | entry count (≤ 2^31)              |

Where `L` = `sourceRoot` length. Entries follow immediately after the header; for each entry
(`len` = path length), relative offsets from the start of the entry:

| Field         | Relative offset | Bytes                | Description                    |
|---------------|-----------------|----------------------|--------------------------------|
| path          | 0               | 8 + len              | u64 length + path UTF-8        |
| size          | 8 + len         | 8                    | file size                      |
| lastWriteTime | 16 + len        | 8                    | FILETIME last modification     |
| attributes    | 24 + len        | 4                    | `FILE_ATTRIBUTE_*`             |
| fileId        | 28 + len        | 8                    | informative only               |
| isDirectory   | 36 + len        | 1                    | 0/1                            |
| hasHash       | 37 + len        | 1                    | 0/1                            |
| digest        | 38 + len        | 32 (if hasHash)      | SHA-256                        |

A full entry therefore occupies `38 + len + (hasHash ? 32 : 0)` bytes.

### Export CSV / JSON

`--export <file>` writes non-identical entries after scanning. Format inferred from extension
(`.json` = JSON, otherwise CSV) or forced with `--export-format`.
CSV: UTF-8 with BOM (Excel), columns `status,path,size_source,size_destination,hash_source,
hash_destination`, RFC 4180 escaping (comma/quote/newline in names).
JSON: streaming array (one entry at a time, limited memory), RFC 8259 escaping, no BOM; with JSON export
the `slowest_dirs` section is also included (see below).

Directory slower (top-N)

At the end of the scan the CLI prints the directories that took the most time, top 15 per column and
per side (A = source, B = destination), without summing different columns.
`list` and `walk` are top-N **exact** measured directories; `hash` is a top-N **estimated** (see below):
- `listSeconds`: time of FindFirst/FindNext syscalls for directory (Win32 backend);
- `walkSeconds`: time of $I30 resolve + walk-step for directory (MFT backend; **not**
  a pure listing, hence the different name);
- `hashSeconds` (Content mode only): sum of `FileTimings.totalTicks` of files actually read+hashed
  under each directory parent. Hash cache hits are excluded (cost ~zero) and counted separately as global
  `hash cache hits (total run)`, not per-directory.
The `hash` column uses a bounded aggregation (Space-Saving, 64 counters per side): the shown order is
for estimation and may differ from actual for close values; memory constraint is intentional. In offline
the A side comes from the index/snapshot (not from filesystem) and remains without timing; no timing is
saved in the snapshot. The same section is exported in JSON as `slowest_dirs` (`list_a/walk_a/list_b/walk_b/
hash_a/hash_b` + `hash_cache_hits`); CSV export remains unchanged.

### Partial content verification (heuristic)

In Content mode, `--verify-percent <1-100>` (default 100) reads only a fraction of each file above 2 MiB
and `--verify-pattern <edges|center|random>` (default `edges`) chooses the sampling: head+tail, MiB-aligned
central block, or one of the two randomly chosen once per run. Below threshold or at 100% reading is always
complete. `IDENTICO_PARZIALE` means "no difference in the read parts" (counted, not listed) and **does not**
equate to full verification; found differences become `CONTENUTO_DIVERSO_PARZIALE` with actual percentage/pattern.
The cache key includes the actual level (full reads always share the 100/Edges key); snapshot and offline
comparisons always read fully. The JSON reports the run-level section `verify` (`mode`, `percent_requested`,
`pattern`, `random`). The GUI offers three pattern toggles + draggable percentage slider with internal %
and -/+ keys from 1% (0% = Size only) and a run-level banner when the read was actually partial.

### Persistent hash cache

`--hash-cache <file>` activates a SHA-256 cache with key `(absolute path, size, last modification,
actual percentage and pattern)`: if the file is unchanged the digest is reused and the file **is not re-read**.
The cache is an optional optimization: it never changes a verdict (the key is calculated on the current
file before lookup). A corrupted cache file is ignored with a warning, never blocking.

### Advanced errors

- **File modified during scan** (`ChangedDuringScan`): before hashing size/timestamp are checked against
  the value recorded at enumeration; if they change the file is reported without a false verdict (neither
  "Identical" nor "ContentMismatch").
- **Device disconnected** (NAS/USB during operation): Win32 disconnection errors (59, 64, 67, 995, 1167,
  1222, 1231, 1236) abort enumeration and are reported for re-verification; `ACCESS_DENIED` (SMB ACL) is
  not considered a disconnection.

## 8. Known limitations

- Reparse point directories (junction/symlink) not followed and reported as single entry;
  their contents are not explored.
- An error on a directory (e.g. access denied) prevents seeing its children:
  the directory is reported with `ReadError`/`AccessDenied` and children are not counted.
- Raw MFT reading requires an **elevated** process (admin / `SeBackup`); without it the MFT backend
  reports "unavailable" and Win32 fallback is used.
- The snapshot incorporates digests only if captured in Content mode; a "Presence"/"Size" snapshot
  allows only presence/size comparison (degraded).
- Offline comparison is based on the state at snapshot time: files modified on the first device after
  capture are not detected (requires re-capture).
- UNC tests executable only with a true network share.

---

## Compilation

### With MSYS2 / MinGW-w64 (used in this repository)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

Produced executables in `build/`:
- `src/bv_cli.exe` — CLI verifier
- `tests/bv_tests.exe` — test suite
- `tests/bv_testgen.exe` — test tree generator
- `tests/bv_mftbench.exe` — MFT correctness vs Win32 benchmark
- `tests/bv_mftprobe.exe` — MFT/record NTFS diagnostics
- `tests/bv_mftdiag.exe` — MFT vs Win32 comparison on real volume + probe (elevated)

#### SDL3 GUI (Phase 2)

```powershell
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-ttf
cmake -S . -B build_gui -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
    -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DBUILD_GUI=ON -DBUILD_TESTS=ON
cmake --build build_gui
```

GUI executable: `build_gui/src/bv_gui.exe`. At startup the GUI must find the SDL DLLs: add
`C:\msys64\mingw64\bin` to PATH or copy them next to the `.exe`.

The GUI runs the scan on a separate thread, shows progress, progress bar, AVVIA / INTERROMPI / SNAPSHOT /
ESPORTA CSV / **CARICA SNAP.** (offline comparison), the filterable problems list (All / Identical /
Missing / Extra / Size / Content / Errors / **Timelines**) with scroll (mouse wheel +
draggable sidebar scrollbar: drag the thumb or click on the track) and the choice of the **enumeration
backend** (Auto / Win32 / MFT). The comparison mode is chosen only from the verification slider
(0% = Size, 1-99% = Partial Content, 100% = Full Content): Presence and case-sensitive remain available
only from CLI. The Timelines view shows the slowest directories of the run (A/B selector,
Enumeration + Hash panels with "Top-N estimated" note for hash, global hash cache hit count;
in offline the A side explains it comes from the snapshot). At the top the "MatchTable live" section
shows in real time, with two dynamic scale bars, A/B elements waiting to match, peaks, backpressure
threshold as a tick (text only if beyond scale) and the backpressure state (active/inactive,
interventions, total and maximum wait). Every row shows the **full path** of the file
(`FileResult.fullPath`: root + relative path, with only the filename in bold; Missing → source side,
Extra → destination side, errors → failed side if single, otherwise destination as fallback).
Right-click on a row opens a menu with "Open A in Explorer" and/or "Open B in Explorer" (A = source,
B = destination; only the row whose path exists) which opens the file's folder selected in Explorer.
The "Riscansiona" entry (only on live scan file rows) re-verifies that single file with the current GUI
settings (percentage/pattern current, not those of the original scan) and updates the row, without
re-running the full scan. In offline comparison the source does not exist: source line paths show the
registered paths from the snapshot. The SNAPSHOT button captures the source index into a binary file;
EXPORT CSV saves non-identical entries of the last scan (native Windows save dialogs). LOAD SNAP.
opens a selection dialog and verifies **only the destination** against the snapshot (the source is not
read): a second click or choosing a source with Browse returns to online mode.

### With Visual Studio / MSVC and CMake (Windows 11)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Or opening the project folder directly in Visual Studio 2022 (CMSIS detects `CMakeLists.txt`).
The code uses only standard C++17 + Win32 APIs.

---

## CLI usage

```text
bv_cli --source <path> --dest <path> [--mode presence|size|content]
       [--case-sensitive] [--enum auto|win32|mft] [--list-problems [--limit N]]
       [--verify-percent <1-100>] [--verify-pattern edges|center|random]
       [--snapshot-out <file>] [--compare <snapshot>] [--hash-cache <file>]
       [--export <file>] [--export-format csv|json] [--help]
```

Examples:

```powershell
# normal verification with export and cache
bv_cli --source D:\Backup --dest \\NAS\Backup --mode content --enum auto `
       --list-problems --hash-cache C:\temp\hash.bin --export C:\temp\out.csv

# source snapshot (content + digest)
bv_cli --source D:\Backup --mode content --snapshot-out D:\snap\backup.bin

# offline verification against snapshot (first device not needed)
bv_cli --compare D:\snap\backup.bin --dest E:\Backup --mode content
```

## Test tree generator

```text
bv_testgen <root> [--fixture] [--differing] [--stress N] [--large MB]
```

- `--fixture`: an tree identical to itself;
- `--differing`: creates `src/` and `dst/` with known differences;
- `--stress N`: N small files distributed across 100 directories;
- `--large MB`: sparse files from MB MiB in `src/` and `dst/`.

## Structure

```text
src/
  main_cli.cpp            CLI (Phase 1)
  main_gui.cpp            entry GUI SDL3 (Phase 2)
  ScanController.h/.cpp   orchestration scan (chooses/fallback backend)
  Errors.h                device disconnect error detection (Phase 5)
  UI/AppUI.{h,cpp}        SDL3 GUI (render, input, scan thread)
  UI/Utf.{h,cpp}          UTF-8/16 conversion for SDL
  Threading/ThreadPool.{h,cpp}, IoClass.h
  Filesystem/
    FileEntry.h           entry record
    FileIndex.h/.cpp      in-memory index (case policy)
    FileIndexSerializer.h/.cpp  binary BVSI snapshot (Phase 5)
    FileEnumerator.h      scanner interface
    Win32Enumerator.cpp   FindFirstFile/Win32 enumeration
    MftEnumerator.cpp     raw NTFS MFT enumeration (Phase 4)
    PathUtil.h/.cpp       path normalization, \\?\ prefix, case folding
  Comparison/
    ScanMode.h            Presence / Size / Content
    ComparisonResult.h    Status, FileResult (with fullPath: dest for Extra, source for Missing,
                          failed side for errors), Stats, ResultSet
    FileComparator.h/.cpp live and offline comparison (Phase 5)
  Hashing/
    Sha256.cpp            SHA-256 CNG/BCrypt (Phase 3)
    HashCache.h/.cpp      persistent SHA-256 cache (Phase 5)
  Export/
    ExportUtil.h/.cpp     token, CSV/JSON escaping, hex digest, format inference
    CsvExporter.h/.cpp    CSV export (UTF-8 BOM) (Phase 5)
    JsonExporter.h/.cpp   streaming JSON export (Phase 5)
tests/
  TestHarness.h, TestTree.h/.cpp, test_main.cpp
tools/
  testgen.cpp, mftbench.cpp, mftprobe.cpp, mftdiag.cpp
```

## Roadmap

All development phases (1-5) have been completed. The project is feature-complete with:
- CLI verification (Phase 1)
- SDL3 GUI with progress, filtering, thread pool, stop (Phase 2)
- SHA-256 content hashing (Phase 3)
- NTFS MFT scanner + MFT vs Win32 benchmark (Phase 4)
- Binary snapshot + offline comparison, persistent cache, device handling, files modified during scan (Phase 5)

Further work may include UI enhancements, additional filesystem support, or performance optimizations,
but the core verification pipeline is complete.

The detailed structure is also in `HANDOFF.md`.
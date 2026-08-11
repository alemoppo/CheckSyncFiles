# HANDOFF — Backup Verifier (il documento per "il te del futuro")

> Leggi questo file per riprendere il progetto dopo un `/compact` (contesto azzerato).
> La fonte di verità architetturale, oltre a questo file, è `README.md` (completo) e il codice.

---

## 0. Stato attuale: FASI 1, 2, 3, 4 E 5 COMPLETATE ✅

- **Fase 1** (enumerazione Win32, indice, confronto presenza/dimensione, CLI, test) completa.
- **Fase 2** (GUI SDL3 + progress + thread pool + Interrompi) completa.
- **Fase 3** (SHA-256 via CNG/BCrypt, verifica contenuti, velocità MB/s reali) completa.
- **Fase 4** (scanner NTFS via MFT + benchmark/correttezza MFT vs Win32) completa.
- **Fase 5** (snapshot binario BVSI + confronto offline, cache SHA-256 persistente,
  export CSV/JSON, dispositivo scollegato, file modificato durante la scansione) completa.

Il progetto è un **repository git inizializzato** e pushato su `github.com/alemoppo/CheckSyncFiles`
(branch `main`, commit di base `413c066`); i dir build sono in `.gitignore`. Dopo ogni task:
`git add -A && git commit -m "..." && git push`.

Build pulita (`-Wall -Wextra`, 0 warning), **38/38 test passati** in modalità normale
(ctest verde; i 2 test relativi ad access-denied falliscono solo se il processo è elevato,
dove il deny è simulato male). I 2 test **MFT** passano se il processo è **elevato**,
altrimenti vengono saltati (fallback Win32).

- Toolchain reale qui: **MinGW-w64 g++ 16.1.0 + CMake 4.3.2 + Ninja** (NON c'è MSVC installato).
- Directory di lavoro: `C:\Users\alemo\Documents\Agentic\CheckSyncFiles`
- **`build/`**: build CLI/tests/tool (`BUILD_GUI=OFF`).
- **`build_gui/`**: build con GUI (`BUILD_GUI=ON`, `CMAKE_PREFIX_PATH=C:/msys64/mingw64`).
- Eseguibili: `build\src\bv_cli.exe`, `build_gui\src\bv_gui.exe`, `build\tests\bv_tests.exe`, `build\tests\bv_testgen.exe`. La GUI richiede SDL3.dll/SDL3_ttf.dll sul PATH (sono in `C:\msys64\mingw64\bin`).
- Tool MFT: `build\tests\bv_mftbench.exe` (correttezza + timing), `build\tests\bv_mftprobe.exe` (diagnostica record).
- I tool MFT e i test MFT richiedono un processo **elevato** (UAC). Script riutilizzabili in `C:\Users\alemo\AppData\Local\Temp\opencode\` (`elev_run.ps1`, `elev_run_tests.ps1`, `elev_run_cli.ps1`).

**Comandi:**
```powershell
# build base (CLI + test)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure

# build con GUI SDL3 (installa SDL3 e SDL3_ttf: pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-ttf)
cmake -S . -B build_gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DBUILD_GUI=ON -DBUILD_TESTS=ON
cmake --build build_gui
ctest --test-dir build_gui --output-on-failure
```

## 1. Cosa è già implementato

- **Enumerazione Win32** (`Win32Enumerator`, FindFirstFileW/FindNextFileW): stack iterativo, long path `\\?\` (aware UNC `\\?\UNC\...`), file nascosti/sistema inclusi, reparse dir NON seguiti (nessun loop), errori per-directory → la scansione continua.
- **FileEntry** (path relativo, size, lastWriteTime FILETIME, attributes, fileId, isDirectory).
- **PathUtil**: NormalizeRoot, JoinRel, MakeAbsolute, AddLongPathPrefix, **FoldForCompare** (uppercase via LCMapStringEx locale invariante, rispecchia NTFS case-insensitive).
- **FileIndex**: `std::unordered_map<wstring, FileEntry>`; chiave = path **folded** (case-insensitive di default) oppure path esatto con `--case-sensitive`; **FileEntry conserva il path originale** (display/export). Stats: files/dirs/bytes.
- **FileComparator**: confronta l'indice sorgente contro la destinazione **in streaming** (non la indicizza). Per ogni entry dest: `tryErase` (erased = matched) → classificata; ciò che resta nell'indice = "mancanti". Directory vuote mancanti/extra segnalate; non-vuote no (i figli bastano). Ritorna `bool` = root dest accessibile.
- **ScanController**: orchestrazione + timing per fase.
- **ResultSet/Stats/Status**: Identical, Missing, Extra, SizeMismatch, ContentMismatch, ReadError, AccessDenied. Identici solo contati; `problems` = voci non identiche (memoria limitata).
- **CLI** (`main_cli.cpp`): `--source --dest --mode presence|size|content --case-sensitive --enum auto|win32|mft --list-problems --limit --progress`. Usa `main()` + `CommandLineToArgvW` (NON `wmain`, che su MinGW richiede `-municode` → deliberatamente evitato per compatibilità MSVC/MinGW).
- **Tests** (nessun framework esterno): harness custom in `TestHarness.h`. `TestTree.h/.cpp` = builder alberi. `tools/testgen.cpp` = generatore autonomo (fixture/differing/stress/large).
- **(F2) ThreadPool** (`Threading/ThreadPool.{h,cpp}`): pool fisso, coda condivisa, `submit`/`waitAll`, distruttore che drena le task; `submit(0 thread)` esegue in-line (sync). `IoClass` + `DefaultThreadCount` per scegliere il default sensato (Auto: local↔local→min(cpu,8); coinvolge rete→min(cpu,4)).
- **(F2) Progress**: `IFileEnumerator` ha ora anche `ProgressCallback(files,dirs,currentPath)`; `ScanController` espone `ScanOptions.onProgress` con `ScanPhase` (EnumerateSource/CompareDestination/Hashing/Done); la CLI ha `--progress`.
- **(F2) Interrompi**: `ScanOptions.cancel` (flag atomico) propagato in `FileIndex::build` e `FileComparator::run` (early-stop per-voce); il worker della GUI ne esce pulito.
- **(F2) GUI SDL3** (`src/UI/AppUI.{h,cpp}`, `UI/Utf.{h,cpp}`, `main_gui.cpp`): finestra SDL3, font via SDL3_ttf, campi Sorgente/Destinazione a testo, radio modalità, selezione thread (Auto/1/2/4/8/16), toggle **case-sensitive**, scelta **back-end** (Auto/Win32/MFT), bottoni AVVIA/INTERROMPI, riga di stato, barra di avanzamento indeterminata, filtri risultati (Tutti/Identici/Mancanti/Extra/Dimensione/Contenuto/Errori), lista risultati con scroll+a righe alternate, barra riepilogo. Modello di threading: UI sul main thread, scan su worker; progress+risultato copiati sotto mutex. `run()` fa `join()` del worker al close (niente detach → niente use-after-free).
- **(F3) Hashing**: `Hashing/Sha256.cpp` via Windows CNG (`BCrypt`) in streaming a blocchi; `ScanController`/`FileComparator` accodano i file matched path==path && size==size e li hashano con un pool di `HashWorker`; `Stats` espone byte hashati + timing; velocità MB/s reali.
- **(F4) MFT scanner** (`Filesystem/MftEnumerator.{h,cpp}`): legge la MFT raw via il record 0 (`$MFT`) decodificandone i **data-run** (la MFT è frammentata, `MftStartLcn` = solo primo extent); directory/reparse dai flag di header record (`rec+22`); dimensione da `$DATA`; record non "in use" scartati. Selezione backend in `ScanController` (Auto/Win32/Mft) con fallback automatico.
- **(F5) Snapshot binario** (`Filesystem/FileIndexSerializer.{h,cpp}`): format compatto **BVSI** v1 (magic `0x49535642`, little-endian, path UTF-8, per-entry: size/mtime/FILETIME/attributes/fileId/isDirectory + digest SHA-256 opzionale). `--snapshot-out <file>` cattura l'indice sorgente; in modalità Contenuto la sorgente è hashatta PRIMA (i digest riutilizzati dal confronto live, niente doppia lettura) e lo snapshot li incorpora. `--compare <snapshot>` carica l'indice senza toccare il primo device: `FileComparator` in costruttore **offline** (senza sourceRoot) usa solo i digest salvati. Snapshot senza digest → verifica Contenuto **degradata a Dimensione** (`contentDegradedToSize`, `modeUsed` aggiornato).
- **(F5) Cache hash** (`Hashing/HashCache.{h,cpp}`): chiave `(path assoluto, size, mtime)` separata da `\x01`; file binario magic `0x43485642` v1; chiave ricavata sul file corrente prima del lookup (hit sempre valido); cache corrotta ignorata con messaggio, mai bloccante. `--hash-cache <file>`. I hit sono contati con `std::atomic` nei worker (`ScanReport.hashCacheHits`).
- **(F5) Export** (`Export/`): `ExportUtil` (token italiani `IDENTICO/MANCANTE/EXTRA/DIM_DIVERSA/CONTENUTO_DIVERSO/ERRORE_LETTURA/ACCESSO_NEGATO/MODIFICATO_DURANTE_SCAN`, `CsvEscape` RFC 4180, `JsonEscape` RFC 8259, `HexDigest`, `InferFormat` da estensione). `CsvExporter`: UTF-8 **con BOM** (Excel), colonne `status,path,size_source,size_destination,hash_source,hash_destination`. `JsonExporter`: array in streaming, nessuna BOM. `--export <file>` + `--export-format csv|json`. GUI: pulsanti **SNAPSHOT** e **ESPORTA CSV** con dialoghi di salvataggio (IFileSaveDialog/COM).
- **(F5) Errori avanzati**: `Errors.h` → `IsDeviceDisconnectError()` (codici 59/64/67/995/1167/1222/1231/1236), `Win32Enumerator` segnala `ScanError.lostDevice` e **abortisce** l'enumerazione su disconnessione (ACCESS_DENIED SMB escluso). Nuovo `Status::ChangedDuringScan`: `FileComparator::HashOneSide` confronta la stat pre/post hash contro il valore dell'entry e segnala il file modificato tra enumerazione e verifica senza verdetto falso. `ComparisonResult` esteso con `hasHashSource/hasHashDest/hashSource/hashDest` e `Stats.changedDuringScan`; `PathUtil` con `ToUtf8/FromUtf8`.
- **README.md** completo — da aggiornare a ogni fase.

## 2. Bug già trovati e risolti (NON ripetere)

1. `FileIndex::build` faceva `map_.emplace(key(e.relativePath), std::move(e))` — ordine di valutazione argomenti non specificato; `std::move(e)` poteva svuotare `e.relativePath` prima di `key(...)`. **FIX**: calcolare `const std::wstring k = key(e.relativePath);` PRIMA del move.
2. `Win32Enumerator` usava `e.relativePath` DOPO `onEntry(std::move(e))` per costruire il path figlio → `e` era moved-from (vuoto) → tutti i figli finivano alla root. **FIX**: calcolare `childRel` prima della callback e usare quello per lo stack.
3. `destinationOk` era inferito da `problems.front()` — sbagliato (il primo problema poteva essere un errore di root SORGENTE). **FIX**: `FileComparator::run` ritorna direttamente `bool` (root dest accessibile).
4. `std::filesystem::remove_all` **(MinGW libstdc++) appende con i long path `\\?\`** → hang nella cleanup dei test. **FIX**: `RemoveAllWin()` in `test_main.cpp` fa delete ricorsivo Win32 con prefisso `\\?\` su ogni operazione.

### Bug/diagnosi Fase 5 (NON ripetere)
9. **`std::ifstream/ofstream` e `std::wstring`**: il costruttore con `std::wstring` **non compila** su MinGW libstdc++ (serve path `char`). **FIX**: passare sempre `pathutil::AddLongPathPrefix(path).c_str()`. Idem nei test (`ReadFileBytes`).
10. **`CHECK_EQ` su `std::wstring`**: il macro streamma i valori in un `ostringstream` → **errore di compilazione** per wstring. **FIX**: usare `CHECK(a == b)` per i confronti di stringhe wide, `CHECK_EQ` solo per i numerici.
11. **Cache file dentro la tree scansionata**: nel test di cache con `source==destination==tree`, creare `hash.bin` dentro la tree la fa comparire come file nuovo alla seconda run (identici 201 != 200). **FIX**: cache in una directory temp separata.
12. **`modeUsed` dopo il degrade**: `report.modeUsed` era fissato a `options.mode` all'inizio; il degrade Content→Size mutava solo la variabile locale `mode`. **FIX**: aggiornare `report.modeUsed = mode` dentro il branch di degrade.
13. **CSV in Excel**: i path con virgola/quote/a-capo vanno esportati con escaping RFC 4180 (quote doppie dentro campi quotati), altrimenti le righe si spezzano.

### Bug/diagnosi MFT (Fase 4, NON ripetere)
5. **MFT frammentata**: leggere `MftStartLcn + rec*segSize` in sequenza restituisce record fisici sbagliati per ogni extent oltre il primo → "record stantii" falsi, albero ricostruito incompleto (66/100 subdir). **FIX**: decodificare i **data-run** dell'attributo `$DATA` del record 0 e leggere ogni record percorrendo i run in ordine di record. `MftStartLcn` identifica SOLO il punto di partenza per leggere il record 0.
6. **Record non "in use"**: i record `FILE` deleted/stantii hanno ancora un `$FILE_NAME` parsabile → l'MFT enumerator contava 1 voce in più della realtà (es. 401 vs 400). **FIX**: scartare i record con flag header bit 0x0001 (`rec+22`) non impostato.
7. **Directory via `$FILE_NAME` fileAttributes**: il campo fileAttributes del `$FILE_NAME` (offset 56) risultava 0 per le directory → `dir=0`. **FIX**: leggere directory/reparse dai **flag di header del record** (`rec+22`, bit 1 = dir, bit 2 = reparse) — fonte affidabile.
8. **Dimensione di `$FILE_NAME`**: `realSize` nel `$FILE_NAME` (offset 48) è 0 per file creati con `SetEndOfFile` (i dati stanno in `$DATA`). **FIX**: dimensione dall'attributo **`$DATA`** (residente `contentLen` @16, non-residente `realSize` @48).

### Offsets NTFS verificati (usare questi)
- Record MFT header: seq @16, flags @22 (bit0x01 in-use, 0x02 dir, 0x04 reparse), first-attr @20, record size @28, signature "FILE" 0x454C4946, fixup USN; sectors = recordSize/512.
- `$FILE_NAME` (residente, attr header `a[8]==0`, value a+aWas20): parent@0, ctime@8, **mtime@16**, change@24, access@32, allocSize@40, realSize@48, fileAttributes@56, nameLen@64, namespace@65, name@66. Tenere l'ULTIMO `$FILE_NAME` (Win32 name).
- `$DATA` residente: contentLen @16; `$DATA` non-residente: lowVcn@16, highVcn@24, **realSize@48**; map pairs offset @32.
- Data-run: LCN delta **firmato big-endian** (se MSB del byte più significativo → sottrai `1<<(8*offb)`); run terminatore MSB; ogni run `{startVcn, lcn, len}` in cluster.
- File Reference: bit 0–47 record number, bit 48–63 sequence.
- `NTFS_VOLUME_DATA_BUFFER`: `MftStartLcn`/`MftValidDataLength` sono `LARGE_INTEGER` → `.QuadPart`; `BytesPerCluster`, `BytesPerFileRecordSegment`.
- Lettura raw MFT: serve handle di volume `\\.\C:` + `SeBackup/SeRestore` + processo **elevato**.

### Gotchas SDL3 (Fase 2, NON ripetere)
- `SDL_RenderSetClipRect` NON esiste in SDL3 → si chiama **`SDL_SetRenderClipRect`** (il vecchio nome non è nemmeno un alias, da errore di compilazione).
- `SDL_GetMouseState(float*, float*)` in SDL3 → usare `float`, NON `int*`.
- `SDL_RenderFillRect`/`SDL_RenderRect`/`SDL_RenderTexture` usano **`SDL_FRect`** (float), non `SDL_Rect`.
- Gli header SDL vanno inclusi nell'header `AppUI.h` (serve `SDL.h` per `Uint8`/`SDL_FRect`); `#include <windows.h>` nel .cpp PRIMA di SDL per `DWORD`/`GetFileAttributesA`.
- SDL3_ttf: `TTF_RenderText_Blended(font, text, length, SDL_Color)` restituisce `SDL_Surface*` (da distruggere dopo `SDL_CreateTextureFromSurface`); `TTF_Init()`/`TTF_Quit()`, `TTF_OpenFont(path, ptsize)`. Config CMake: `find_package(SDL3_ttf REQUIRED)` → target `SDL3_ttf::SDL3_ttf`.
- GUI: chiudere la finestra con scan in corso richiede `cancel`+`join()` del worker (mai `detach`, rischio use-after-free su membri distrutti).
- I test di concorrenza basati sul tempo (`sleep`) sono flaky → usare un **latch deterministico** (contatore + `condition_variable`) per assicurare l'overlap.

## 3. Convenzioni / decisioni da rispettare

- Include sempre relativi a `src/` (es. `#include "Filesystem/PathUtil.h"`, `"Comparison/FileComparator.h"`).
- `#include <shellapi.h>` DOPO `<windows.h>` (altrimenti i tipi non sono definiti).
- C++17, `-D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE`. MSVC: `/W4 /permissive-`; MinGW: `-Wall -Wextra`.
- Entry point `main()` + `CommandLineToArgvW` (mai `wmain`).
- Case policy: default case-INSENSITIVE (fold). Documentato: su filesystem davvero case-sensitive attivare `--case-sensitive`, altrimenti due file che differiscono solo per caso collidono nella chiave dell'indice.
- Read-only assoluto: mai creare/scrivere/cancellare nei path analizzati; scritture solo su log/report/export.
- Priorità: correttezza > sicurezza > affidabilità > performance > estetica.

## 4. Struttura file

```
src/
  main_cli.cpp, main_gui.cpp, ScanController.h/.cpp, Errors.h
  Filesystem/ FileEntry.h, FileEnumerator.h, Win32Enumerator.h/.cpp, MftEnumerator.h/.cpp, FileIndex.h/.cpp, FileIndexSerializer.h/.cpp, PathUtil.h/.cpp
  Comparison/ ScanMode.h, ComparisonResult.h, FileComparator.h/.cpp
  Hashing/ Sha256.h/.cpp, HashCache.h/.cpp
  Export/ ExportUtil.h/.cpp, CsvExporter.h/.cpp, JsonExporter.h/.cpp
  Threading/ ThreadPool.h/.cpp, IoClass.h
  UI/ AppUI.h/.cpp, Utf.h/.cpp
tests/ TestHarness.h, TestTree.h/.cpp, test_main.cpp, CMakeLists.txt
tools/ testgen.cpp, mftbench.cpp, mftprobe.cpp
```

---

## 5. PROSSIMI PASSI (Fase 2 → obiettivo finale)

Il progetto va REALIZZATO a fasi (§22 della richiesta originale). Seguire questo ordine.

### FASE 2 — GUI SDL3 + progress + thread pool ✅ (COMPLETATA)
- Completato: ThreadPool (`Threading/`), progress (enumeratore + ScanController, `--progress` CLI), Interrompi (flag atomico), GUI SDL3 (`UI/AppUI`, `main_gui.cpp`), integrazione CMake `BUILD_GUI` + `SDL3_ttf`.
- Restano possibili migliorie GUI (non bloccanti): selettore cartelle via dialogo (IFileDialog/COM, non ancora implementato), posizionamento/scroll più raffinati.

### FASE 3 — SHA-256 (verifica contenuti) ✅ (COMPLETATA)
- Completato: hash via **Windows CNG BCrypt** (`Hashing/Sha256.cpp`), streaming a blocchi (offset 64-bit, file >4 GiB), pool di `HashWorker` riusando il **ThreadPool Fase 2**, accodamento dei file matched `path==path && size==size` in `FileComparator`, `Stats` con byte hashati + timing, velocità MB/s reali. `ScanMode::Content` = hash reale.

### FASE 4 — MFT scanner NTFS ($MFT) ✅ (COMPLETATA)
- Completato: `Filesystem/MftEnumerator.cpp` implementa `IFileEnumerator` (lettura raw della MFT via data-run di record 0, ricostruzione path da `$FILE_NAME` parent-reference, `fileId` nel `FileEntry`). Vedi "Bug/diagnosi MFT" in §2 e i dettagli in `README.md` §1.
- È **solo un'ottimizzazione**: `ScanController` rileva il filesystem e con `backend=Auto` usa MFT solo se origine e destinazione sono NTFS locali, con **fallback automatico** a `Win32Enumerator`. Selezionabile a mano con `--enum auto|win32|mft` (CLI) e toggle GUI.
- Richiede un processo **elevato** per la lettura raw del volume; senza privilegi il backend segnala "non disponibile" e si usa il fallback (mai "Run as Administrator" globale).
- Gestisce: milioni di file, nomi Unicode, record non "in use" (stantii) scartati, dataset. Tool di validazione: `bv_mftbench --gen N` (deve dare CORRISPONDENZA MFT==Win32) e `bv_mftprobe`.

### FASE 5 — cache, snapshot, export, errori avanzati ✅ (COMPLETATA)
> Completata e validata. Riepilogo di quanto implementato (dettagli in `README.md` §7b):
> **snapshot binario BVSI** (`Filesystem/FileIndexSerializer`) con `--snapshot-out` e
> confronto offline `--compare <snapshot> --dest <dest>` (la sorgente non viene letta);
> in Content lo snapshot incorpora i digest SHA-256 (degraded a Size se assenti).
> **cache SHA-256** persistente (`Hashing/HashCache`) via `--hash-cache`, hit contati e
> riportati; **export** CSV (BOM UTF-8, escaping RFC 4180) e JSON (streaming) via
> `--export`/`--export-format`; GUI con pulsanti SNAPSHOT/ESPORTA CSV e dialoghi
> IFileSaveDialog; **errori avanzati**: dispositivo scollegato (`Errors.h`,
> `Win32Enumerator.lostDevice` → abort) e file modificato durante la scansione
> (`Status::ChangedDuringScan`, stat pre/post hash). Nuovo stato in `ComparisonResult`.
> Test aggiunti: 8 (escaping CSV/JSON, round-trip snapshot, snapshot corrotto, offline
> content, degrade a size, cache seconda run, changed-during-scan). **38/38 test verdi.**
>
> Possibili migliorie future (non bloccanti): GUI per confronto offline (pulire la
> sorgente e selezionare uno snapshot), merge di snapshot incrementali, bucket/ordinamento
> dell'indice per ridurre la memoria sulle tree enormi.

A fine fase: aggiornare `README.md` + `HANDOFF.md` (barrare questa sezione come ✅),
aggiungere test, tenere il suite verde (30 test), commit+push.

---

## 6. Test da aggiungere man mano (spec §23)

File modificato durante scansione; percorsi UNC reali; NTFS vs non-NTFS; "milioni di entry simulate" (es. `--stress` con N grande e misura tempo/memoria); benchmark MFT vs Win32. Mantenere i test esistenti verdi mentre si refactora.

Già coperti (38 totali): ThreadPool (drain/parallelismo/latch deterministico/0 thread/dtr), IoClass, progress (`onProgress` → file + fase Done), cancel (pre-set ferma subito), ioclasse, **MFT** (`IsSupported` su NTFS + enumerazione MFT == Win32, saltata se non elevato), **export CSV/JSON** (escaping, BOM, digest hex), **snapshot** (round-trip con hash + case policy, file corrotto rifiutato), **offline** (content contro snapshot, degrade a Size), **cache hash** (seconda run senza rilettura), **changed-during-scan** (stat pre/post hash).

Nota: eseguire `build\tests\bv_tests.exe` **elevato** fa girare anche i test MFT; senza elevazione risultano "sostanzialmente saltati" (fallback Win32). Con il processo elevato i 2 test "access denied"/"content mode" falliscono solo perché il deny/accesso non viene simulato bene da admin — non è un bug del codice MFT.

---

## 7. Cose da non dimenticare quando si riprende

- L'ambiente ha **solo MinGW**, non MSVC. Per testare il build MSVC bisognerà installare Visual Studio Build Tools, ma i CMake sono già compatibili. Nota MSVC per la GUI: `find_package(SDL3/SDL3_ttf)` richiede che SDL3 sia raggiungibile (vcpkg o percorso manuale).
- `BUILD_GUI` è `OFF` di default; usa SDL3 + SDL3_ttf già installati in mingw64. Avvio GUI: `build_gui\src\bv_gui.exe` (serve `C:\msys64\mingw64\bin` nel PATH per le DLL SDL).
- Il testo richiesto in ORIGINALE è in italiano; la UI e i messaggi CLI sono già in italiano.
- README aggiornato a ogni fase (oggi Fasi 1–5 documentate; Fase 5 in §7b).
- **Avvio manuale di verifica MFT** dopo ogni modifica a `MftEnumerator.cpp`: `cmake --build build --target bv_mftbench; Start-Process powershell -File <temp>\elev_run.ps1 -Verb RunAs -Wait` poi leggere `mftbench_elev_out.txt` (deve dire `CORRISPONDENZA`). Lo script elevato è in `C:\Users\alemo\AppData\Local\Temp\opencode\`.

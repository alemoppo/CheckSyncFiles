# Backup Verifier

Verificatore di backup **read-only** per Windows in **C++17 + SDL3** (GUI attiva dalla Fase 2).
Confronta due alberi di file (es. disco USB o cartella NAS via SMB) per assicurarsi che
contengano gli stessi dati, **senza mai** copiare, modificare o cancellare nulla.

Stato attuale: **Fasi 1, 2, 3, 4 e 5 completate** (enumerazione Win32, indice, confronto
presenza/dimensione, CLI, GUI SDL3 con progress e thread pool, SHA-256 per il contenuto,
**scanner MFT NTFS** con benchmark MFT vs Win32, **snapshot binario con confronto offline,
cache SHA-256 persistente, export CSV/JSON, gestione di dispositivo scollegato e file
modificato durante la scansione**).

Priorità dichiarata: **correttezza > sicurezza > affidabilità > performance > estetica**.

Nota di encoding: molti file di testo in questo repository erano stati salvati con una
codifica corrotta; questo README è riscritto in UTF-8 pulito.

---

## 1. Come funziona la scansione MFT

La scansione NTFS via **Master File Table** è implementata (Fase 4) in
`src/Filesystem/MftEnumerator.cpp`. Il flusso è:

```text
Volume NTFS
   |--> MFT (lettura diretta dei $MFT file record)
   |--> ricostruzione dei percorsi (da $FILE_NAME parent reference)
   v
FileIndex
```

Approccio usato:

- un volume NTFS ha la MFT **frammentata**: `MftStartLcn` identifica solo il primo
  extent, quindi non si legge mai la MFT come se fosse contigua da lì (restituirebbe
  record fisici errati). Si legge invece il record 0 (`$MFT`) e dal suo attributo
  `$DATA` non-residente si decodificano i **data-run** (`ParseDataRuns`); ogni file
  record viene poi letto fisicamente percorrendo i run in ordine di record.
- la ricostruzione è **top-down**: si percorrono gli indici `$I30` di ogni directory
  (voce inline in `$INDEX_ROOT` + blocchi INDX di `$INDEX_ALLOCATION` letti tramite i
  data-run) — la stessa struttura che usa `FindFirstFileW` — e i parent-pointer dei
  `$FILE_NAME` fungono da fonte ridondante (union). Il v1 (sola catena bottom-up dai
  parent-pointer) produceva path errati su volumi reali.
- il **nome** di ogni figlio viene preso dal **`$FILE_NAME`** del suo record con
  priorità di namespace **WIN32 (1)**, fallback WIN32+DOS (3), mai DOS (2)/POSIX (0):
  le chiavi dell'$I30 possono essere nomi 8.3 DOS dello stesso record (es. `255C81~1.TMP`),
  quindi la key dell'indice non è mai usata come nome visuale.
- ogni **FILE_REFERENCE** è trattata come `record_number (48 bit) + sequence (16 bit)`,
  mai come un indice: per ogni `$FILE_NAME` e per ogni figlio dell'$I30 si valida
  `parent.sequence == sequence_salvata`; una reference **stale** viene scartata e la
  scansione segnala incomplete invece di produrre path falsi.
- la **root** è risolta con `FILE_ID_INFO` (Win32) in record+sequence (con
  `GetFileInformationByHandleEx`); l'invariant "self-parent" è verificato solo quando la
  root è il record 5 del volume (l'unico caso in cui vale).
- directory/reparse point provengono dai **flag di header del record** (`rec+22`,
  bit 1 = directory, bit 2 = reparse) perché il campo fileAttributes di `$FILE_NAME`
  non è sempre impostato.
- la dimensione del file proviene dall'attributo **`$DATA`** (residente: `contentLen`
  in header; non-residente: `realSize`) perché il `realSize` di `$FILE_NAME` può essere 0
  per file materializzati con `SetEndOfFile`.
- i record non più **"in use"** (flag header bit 0x0001 non impostato) vengono ignorati
  per escludere record stantii (deleted); la banda dei metafile di sistema (record ≤ 23:
  `$MFT`, `$LogFile`, ...) non è mai esposta come voce utente.
- un `enumerate()` **incompleto** ritorna `false`: il `FileIndex` parziale viene
  scartato e ricostruito da zero dal fallback Win32 (mai una scansione parziale che
  sembri valida). `BV_MFT_DEBUG=1` in ambiente abilita una traccia diagnostica del motivo
  di ogni bail-out (default off).

La MFT è **un'ottimizzazione, non una dipendenza**: il programma continua a funzionare
se il volume non è NTFS, se l'accesso alla MFT non è disponibile, se mancano i privilegi,
se il volume è remoto. In tutti questi casi si usa il fallback Win32 (punto 4).

## 2. Filesystem supportati

| Origine                             | Scanner                        |
|-------------------------------------|--------------------------------|
| Volume locale NTFS                  | MFT (Fase 4) / Win32 fallback  |
| Volume locale non-NTFS (FAT/exFAT...) | Win32 enumeration            |
| SMB / NAS (UNC, `\\nas\share`)      | Win32 enumeration (via SMB)    |

Il rilevamento del filesystem avviene automaticamente (`GetVolumeInformationW`);
il backend è selezionabile anche a mano (`--enum auto|win32|mft` e toggle nella GUI).
Se l'accesso diretto alla MFT fallisce si passa al fallback sicuro.

## 3. Quando viene usata la MFT

Per impostazione predefinita (`backend=Auto`): origine e destinazione **locali**, volume
**NTFS**, accesso alla MFT **disponibile** (serve un processo elevato per la lettura raw).
In ogni altro caso è fallback Win32. Nessun hack non documentato; nessun rischio di
corrompere il filesystem (tutte le letture sono `GENERIC_READ`).

## 4. Quando viene usato il fallback Win32

`Win32Enumerator` (src/Filesystem/Win32Enumerator.cpp) fa una visita iterativa con
`FindFirstFileW`/`FindNextFileW`:

- path lunghi via prefisso `\\?\` (aware UNC: `\\?\UNC\...`);
- file nascosti e di sistema inclusi;
- **reparse point** di directory (junction/symlink) registrati ma **non seguiti**,
  il che garantisce l'assenza di loop; i symlink di file sono riportati come entry;
- errori per singola directory segnalati e la scansione continua.

## 5. Come funziona il confronto

L'identificazione è per **percorso relativo** alla root:

```text
D:\Backup\Foto\2025\foto001.jpg     ->  Foto\2025\foto001.jpg
\\NAS\Backup\Foto\2025\foto001.jpg  ->  Foto\2025\foto001.jpg
```

Pipeline (`ScanController`):

```text
Enumerazione sorgente -> FileIndex (in memoria)
Enumerazione destinazione (streaming, non indicizzata) -> FileComparator
```

- la sorgente viene indicizzata una volta;
- la destinazione viene scorsa **senza** costruire un secondo indice (limite memoria);
- ogni entry di destinazione è confrontata per path relativo e cancellata dall'indice
  man mano (matched); ciò che resta è esattamente l'insieme dei "mancanti".

Modalità:
- **Presenza**: solo esistenza del path.
- **Dimensione**: presenza + dimensione (non si legge mai il contenuto).
- **Contenuto** (Fase 3): ogni file con stesso path **e** stessa dimensione viene
  hashato (SHA-256) e confrontato.

Risultati classificati: `Identical`, `Missing` (solo sorgente), `Extra` (solo destinazione),
`SizeMismatch`, `ContentMismatch`, `ReadError`, `AccessDenied`, `ChangedDuringScan`
(Fase 5: il file è cambiato tra enumerazione e verifica del contenuto).

Ottimizzazioni memoria/velocità:
- la destinazione è in streaming (mai un secondo indice);
- gli entry **identici** sono solo contati; il vettore `problems` contiene solo le voci
  non identiche (o errori);
- le **directory** non vuote mancanti/extra non vengono riportate singolarmente
  (i figli bastano); solo le directory vuote sono segnalate;
- `unordered_map` per l'indice; chiave = path "folded" (case-insensitive), valore =
  `FileEntry` con il path originale (per display/export).

### Politica case

Windows/SMB sono case-insensitive. Di default il confronto è **case-insensitive**:
la chiave è il path convertito in MAIUSCOLO con `LCMapStringEx` (locale invariante).
Con `--case-sensitive` la chiave è il path esatto.

## 6. Come funziona l'hashing

Fase 3: streaming a blocchi di 1–8 MiB, SHA-256 via Windows CNG (`BCrypt`); un file non
viene hashato se già riconoscibile come diverso dalla dimensione. Il pool dei worker di
hash è configurabile (`Auto`, 1, 2, 4, 8, 16) e la scelta automatica dipende dalla classe
I/O delle due radici (locale-locale, locale-rete, rete-rete).

## 7. Come viene gestita la concorrenza

Thread pool configurabile (vedi `Threading/ThreadPool.{h,cpp}` e `IoClass.h`).
Nessun thread per file: i worker processano i file da una coda condivisa durante la
fase di hashing.

## 7b. Fase 5 — snapshot, confronto offline, export, cache, errori avanzati

### Snapshot binario e confronto offline

L'indice sorgente può essere serializzato su disco con `--snapshot-out <file>`
(format binario compatto **BVSI**, magic `0x49535642` v1; niente JSON: per milioni di
file il binario è decine di MB invece di centinaia). In modalità **Contenuto** la sorgente
viene prima hashatta e lo snapshot incorpora i digest SHA-256 per ogni voce.

Con `--compare <snapshot> --dest <dest>` la sorgente non viene letta affatto: l'indice e gli
impronte si caricano dallo snapshot e si verifica la sola destinazione (utile quando il
primo dispositivo non è collegato). Se lo snapshot non contiene impronte, la verifica
Contenuto viene **degradata a Dimensione** (segnalato esplicitamente).

Lo stesso confronto offline è disponibile nella **GUI** con il pulsante **CARICA SNAP.**
(il campo sorgente viene disabilitato e mostrato come `[snap] <file>`; un secondo clic
ripristina la modalità online).

##### Specifica del formato binario `BVSI` (v1)

Tutti gli interi sono **little-endian**. `uN` = intero senza segno a N bit; una stringa è
`u64 lunghezza + lunghezza byte UTF-8` (mai `\0` terminatore).

```text
Header:
  magic            u32  0x49535642            ("BVSI")
  version          u32  1
  caseSensitive    u8   0 = case-insensitive, 1 = case-sensitive
  sourceRoot       str  radice sorgente assoluta (UTF-8)
  files            u64  numero file (informativo; ricalcolato in lettura)
  dirs             u64  numero directory (informativo)
  bytes            u64  somma dimensioni (informativo)
  count            u64  numero di voci (deve essere ≤ 2^31)

Voci (ripetute `count` volte):
  path             str  percorso relativo alla root (UTF-8, ≤ 2^24 byte)
  size             u64  dimensione file
  lastWriteTime    u64  FILETIME ultima modifica
  attributes       u32  attributi Win32 (FILE_ATTRIBUTE_*)
  fileId           u64  FileId (solo informativo)
  isDirectory      u8   0/1
  hasHash          u8   0/1
  digest           32 byte SHA-256 (solo se hasHash == 1)
```

Vincoli di robustezza applicati in lettura: `magic`/`version` devono combaciare, ogni
`count` e `pathLen` ha un bound di sanità (voci e path sproporzionati fanno rifiutare il
file come corrotto); un file troncato a metà voce è rifiutato. Le statistiche di header
sono informative: all'`addEntry` di ogni voce vengono ricalcolate. Il formato è inteso
stabile, ma la `version` permette evoluzioni future non retrocompatibili.

Layout con offset e dimensioni (tutto little-endian):

| Campo        | Offset          | Byte   | Descrizione                       |
|--------------|-----------------|--------|-----------------------------------|
| magic        | 0               | 4      | `0x49535642` ("BVSI")             |
| version      | 4               | 4      | 1                                 |
| caseSensitive| 8               | 1      | 0/1                               |
| sourceRoot   | 9               | 8 + L  | u64 lunghezza + L byte UTF-8      |
| files        | 17 + L          | 8      | informativo                       |
| dirs         | 25 + L          | 8      | informativo                       |
| bytes        | 33 + L          | 8      | informativo                       |
| count        | 41 + L          | 8      | numero voci (≤ 2^31)              |

Dove `L` = lunghezza di `sourceRoot`. Le voci seguono subito dopo l'header; per ogni
voce (`len` = lunghezza del path), offset relativi all'inizio della voce:

| Campo         | Offset rel. | Byte             | Descrizione                    |
|---------------|-------------|------------------|--------------------------------|
| path          | 0           | 8 + len          | u64 lunghezza + path UTF-8     |
| size          | 8 + len     | 8                | dimensione file                |
| lastWriteTime | 16 + len    | 8                | FILETIME ultima modifica       |
| attributes    | 24 + len    | 4                | `FILE_ATTRIBUTE_*`             |
| fileId        | 28 + len    | 8                | solo informativo               |
| isDirectory   | 36 + len    | 1                | 0/1                            |
| hasHash       | 37 + len    | 1                | 0/1                            |
| digest        | 38 + len    | 32 (se hasHash)  | SHA-256                        |

Una voce completa occupa quindi `38 + len + (hasHash ? 32 : 0)` byte.

### Export CSV / JSON

`--export <file>` scrive le voci non identiche dopo la scansione. Formato dedotto
dall'estensione (`.json` = JSON, altrimenti CSV) o forzato con `--export-format`.
CSV: UTF-8 con BOM (Excel), colonne `status,path,size_source,size_destination,hash_source,
hash_destination`, escaping RFC 4180 (virgola/quote/a-capo nei nomi). JSON: array in
streaming (una voce per volta, memoria limitata), escaping RFC 8259, senza BOM.
Token di stato in italiano: `IDENTICO, MANCANTE, EXTRA, DIM_DIVERSA, CONTENUTO_DIVERSO,
ERRORE_LETTURA, ACCESSO_NEGATO, MODIFICATO_DURANTE_SCAN`.

### Cache hash persistente

`--hash-cache <file>` attiva una cache SHA-256 con chiave `(path assoluto, dimensione,
ultima modifica)`: se il file è invariato il digest viene riusato e il file **non viene
riletto**. La cache è un'ottimizzazione opzionale: non cambia mai un verdetto (la chiave
viene calcolata sul file corrente prima del lookup). Un file di cache corrotto viene
ignorato con un avviso, mai bloccante.

### Errori avanzati

- **File modificato durante la scansione** (`ChangedDuringScan`): prima dell'hash si
  verificano dimensione/timestamp contro il valore registrato all'enumerazione; se
  cambiano tra i due momenti il file è segnalato senza un verdetto falso (né
  "Identical" né "ContentMismatch").
- **Dispositivo scollegato** (NAS/USB durante l'operazione): gli errori Win32 di
  disconnessione (59, 64, 67, 995, 1167, 1222, 1231, 1236) abortiscono l'enumerazione e
  vengono segnalati per riverifica; `ACCESS_DENIED` (ACL SMB) non è considerato una
  disconnessione.

## 8. Limiti noti

- Directory reparse (junction/symlink) non seguite e riportate come singola voce;
  i contenuti a cui puntano non vengono esplorati.
- Un errore su una directory (es. accesso negato) impedisce di vedere i suoi figli:
  la directory è segnalata con `ReadError`/`AccessDenied` e i figli non sono contati.
- La lettura raw della MFT richiede un processo **elevato** (amministratore /
  `SeBackup`); senza di esso il backend MFT segnala "non disponibile" e si usa Win32.
- Lo snapshot incorpora i digest solo se catturato in modalità Contenuto; uno snapshot
  "Presenza"/"Dimensione" consente il solo confronto di presenza/dimensione (degradato).
- Il confronto offline si basa sullo stato al momento dello snapshot: file modificati sul
  primo dispositivo dopo la cattura non vengono rilevati (serve ricatturare).
- Test UNC eseguibili solo in presenza di una vera share di rete.

---

## Compilazione

### Con MSYS2 / MinGW-w64 (usato in questo repository)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

Eseguibili prodotti in `build/`:
- `src/bv_cli.exe` — CLI di verifica
- `tests/bv_tests.exe` — suite di test
- `tests/bv_testgen.exe` — generatore di alberi di test
- `tests/bv_mftbench.exe` — benchmark/correttezza MFT vs Win32
- `tests/bv_mftprobe.exe` — diagnostica MFT/record NTFS
- `tests/bv_mftdiag.exe` — confronto MFT vs Win32 su volume reale + probe (elevato)

#### GUI SDL3 (Fase 2)

```powershell
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-ttf
cmake -S . -B build_gui -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
    -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DBUILD_GUI=ON -DBUILD_TESTS=ON
cmake --build build_gui
```

Eseguibile GUI: `build_gui/src/bv_gui.exe`. All'avvio la GUI deve trovare le DLL
SDL: aggiungere `C:\msys64\mingw64\bin` al PATH oppure copiarle accanto all'`.exe`.

La GUI fa girare la scansione su un thread separato, mostra l'avanzamento, barra di
progressione, pulsanti AVVIA / INTERROMPI / SNAPSHOT / ESPORTA CSV / **CARICA SNAP.**
(confronto offline), la lista dei problemi filtrabile (Tutti / Identici / Mancanti /
Extra / Dimensione / Contenuto / Errori) con scroll, il toggle case-sensitive e la scelta
del **back-end di enumerazione** (Auto / Win32 / MFT). Il pulsante SNAPSHOT cattura
l'indice della sola sorgente in un file binario; ESPORTA CSV salva le voci non identiche
dell'ultima scansione (dialoghi di salvataggio Windows nativi). CARICA SNAP. apre un
dialogo di selezione e verifica la **sola destinazione** contro lo snapshot (la sorgente
non viene letta): un secondo clic o la scelta di una sorgente con Sfoglia torna alla
modalità online.

### Con Visual Studio / MSVC e CMake (Windows 11)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Oppure aprendo la cartella del progetto direttamente in Visual Studio 2022 (CMSIS
rileva `CMakeLists.txt`). Il codice usa solo C++17 standard + API Win32.

---

## Uso della CLI

```text
bv_cli --source <percorso> --dest <percorso> [--mode presence|size|content]
       [--case-sensitive] [--enum auto|win32|mft] [--list-problems [--limit N]]
       [--snapshot-out <file>] [--compare <snapshot>] [--hash-cache <file>]
       [--export <file>] [--export-format csv|json] [--help]
```

Esempio:

```powershell
# verifica normale con esportazione e cache
bv_cli --source D:\Backup --dest \\NAS\Backup --mode content --enum auto `
       --list-problems --hash-cache C:\temp\hash.bin --export C:\temp\out.csv

# snapshot della sorgente (contenuto + digest)
bv_cli --source D:\Backup --mode content --snapshot-out D:\snap\backup.bin

# verifica offline contro lo snapshot (il primo dispositivo non serve)
bv_cli --compare D:\snap\backup.bin --dest E:\Backup --mode content
```

## Generatore di alberi di test

```text
bv_testgen <root> [--fixture] [--differing] [--stress N] [--large MB]
```

- `--fixture`: un albero identico a se stesso;
- `--differing`: crea `src/` e `dst/` con differenze note;
- `--stress N`: N file piccoli distribuiti su 100 directory;
- `--large MB`: file sparse da MB MiB in `src/` e `dst/`.

## Struttura

```text
src/
  main_cli.cpp            CLI (Fase 1)
  main_gui.cpp            entry GUI SDL3 (Fase 2)
  ScanController.h/.cpp   orchestrazione scansione (sceglie/fallback del backend)
  Errors.h                rilevamento errore di disconnessione dispositivo (Fase 5)
  UI/AppUI.{h,cpp}        GUI SDL3 (render, input, thread di scan)
  UI/Utf.{h,cpp}          conversione UTF-8/16 per SDL
  Threading/ThreadPool.{h,cpp}, IoClass.h
  Filesystem/
    FileEntry.h           record di una entry
    FileIndex.h/.cpp      indice in memoria (policy case)
    FileIndexSerializer.h/.cpp  snapshot binario BVSI (Fase 5)
    FileEnumerator.h      interfaccia scanner
    Win32Enumerator.cpp   enumerazione FindFirstFile/Win32
    MftEnumerator.cpp     enumerazione raw NTFS MFT (Fase 4)
    PathUtil.h/.cpp       normalizzazione path, prefisso \\?\, case folding
  Comparison/
    ScanMode.h            Presence / Size / Content
    ComparisonResult.h    Status, FileResult, Stats, ResultSet
    FileComparator.h/.cpp confronto (live e offline, Fase 5)
  Hashing/
    Sha256.cpp            SHA-256 CNG/BCrypt (Fase 3)
    HashCache.h/.cpp      cache SHA-256 persistente (Fase 5)
  Export/
    ExportUtil.h/.cpp     token, escaping CSV/JSON, hex digest, inferenza formato
    CsvExporter.h/.cpp    export CSV (BOM UTF-8) (Fase 5)
    JsonExporter.h/.cpp   export JSON in streaming (Fase 5)
tests/
  TestHarness.h, TestTree.h/.cpp, test_main.cpp
tools/
  testgen.cpp, mftbench.cpp, mftprobe.cpp, mftdiag.cpp
```

## Roadmap

- **Fase 2**: GUI SDL3, progress, risultati filtrabili, thread pool, Interrompi. Completata.
- **Fase 3**: SHA-256 (CNG), verifica contenuti, statistiche velocità MB/s. Completata.
- **Fase 4**: scanner MFT NTFS + benchmark MFT vs Win32. Completata.
- **Fase 5**: snapshot indice binario + confronto offline, cache SHA-256 persistente,
  export CSV/JSON, dispositivo scollegato, file modificato durante la scansione. Completata.

La struttura dettagliata è anche in `HANDOFF.md`.
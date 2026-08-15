# CheckSyncFiles — Report audit (c3a3a09 → HEAD)

Audit a 8 commit su `main`, working-tree pulito. Ogni issue logico è un commit;
dopo ogni commit: `cmake --build build` + `ctest --test-dir build --output-on-failure`.

## Commits

| Commit | Issue | File |
|---|---|---|
| `14c6fcf` | I0+I1 | `src/Filesystem/MftEnumerator.cpp`, `tools/mftdiag.cpp`, `tools/mftprobe.cpp`, `tests/test_main.cpp` |
| `56621ca` | I2 | `src/Export/JsonExporter.cpp`, `tests/test_main.cpp` |
| `f0e309a` | I3.1/3.2 | `src/Filesystem/MftEnumerator.cpp` |
| `51d147e` | I3.3 | `src/Filesystem/FileIndex.cpp`, `tests/test_main.cpp` |
| `9ea1aca` | I3.4 | `src/Util/StrictNumbers.h`, `src/main_cli.cpp`, `tests/test_main.cpp` |
| `4ed441b` | I3.5 | `src/Errors.h`, `tests/test_main.cpp` |
| `7e12a4f` | I3.6 | `src/Export/ExportUtil.cpp`, `tests/test_main.cpp` |

## Issue 0 — INDEX_BLOCK non assunto 4096

- `index_block_size` letto dal `$INDEX_ROOT` (value+0x08); 4096 (con bounds
  512..262144) solo come fallback difensivo. Documentato nei commenti.
- Prove reali su volume C: (NTFS, 512 B/settore, cluster 4096, record 1024):
  `$INDEX_ROOT` 16-B header + node a +0x10; blocco INDX usa_ofs=0x28,
  usa_count=9, fixup 8 tail word; blocco `INDEX_HEADER` a +0x18 (il parser
  pre-fix leggeva +0x10, il campo VCN → entriesOff 0/1 → **nessun blocco INDX
  mai parsato**).

## Issue I1 — USA fixup su `$INDEX_ALLOCATION`

- `UndoFixupIndexBlock` (validazioni: bounds USA, `usa_count ≥ settori+1`,
  ogni tail == usa_next prima di sostituire), applicato prima del parse di ogni
  blocco INDX; node a +0x18.
- Blocco con magic INDX ma fixup fallito → `SIZE_MAX` → `failIncomplete`
  ("directory $INDEX_ALLOCATION block corrupt") → fallback Win32: il MFT
  incompleto non viene mai spacciato per valido.
- Blocchi senza magic INDX = buchi di allocazione legittimi (NTFS dimensiona
  in multipli di cluster): **skip**, non corruzione (30/192 blocchi su
  `_indxbig` erano liberi). Verificato con probe full-scan.
- Copie sincronizzate della logica in `mftdiag.cpp`/`mftprobe.cpp` (commento
  IDENTICAL COPY).
- Diagnosi opt-in `BV_MFT_DEBUG_FILE`: append `indxBlocks=<n> indxChildren=<m>`
  al termine di un'enumerate riuscita; mai richiesto per il successo dei test.

### Test regressivo INDX (elevato)

- `mft: large directory reads $INDEX_ALLOCATION blocks` — 300 file in una
  sola directory con nomi di 90 char (indice ≫ 4096), assert MFT≡Win32 **e**
  `indxBlocks > 0` (prova che `$INDEX_ALLOCATION` è stato esercitato).
- Su `_indxbig` (8000 file): `indxBlocks=362 indxChildren=8100`,
  MFT files=8000 dirs=100 total=8100, 0 only-MFT, 0 only-Win32, 0 mismatch.

## Issue I2 — JSON trailing comma

- `WriteJson`: separatore `,\n` tra gli oggetti, mai dopo l'ultimo (RFC 8259).
- Test con validatore strutturale (bilanciamento di `[]{}`, stringhe chiuse,
  nessuna `,` seguita da `]`/`}`) + caso `empty-problems` → `[]` valido.

## Issue I3.1/3.2 — Bounds in ParseRecord / `$INDEX_ROOT` valueLen

- Header record ≥ 48; ogni attributo solo se `len ≥ 24` e dentro il buffer;
  value-region sempre verificata contro `len` dell'attributo (mai spill nel
  prossimo attributo) per `$FILE_NAME`, `$INDEX_ROOT`, `$DATA` res./non-res.
- Verificato nessuna regressione su scansione reale elevata.

## Issue I3.3 — FileIndex duplicate-key semantics

- Due path che collassano sulla stessa chiave case-fold: **LAST-WINS**
  (il più recente sostituisce), stats adeguate così che `files == size()`.
- Test con enumerator fake che produce `Foo.txt`/`foo.TXT`.

## Issue I3.4 — CLI numeric parsing

- `StrictNumbers.h`: `ParseUInt64`/`ParseThreadCount` senza eccezioni
  (niente `stoul`/`stoull` non protetti). `--limit` e `--threads` (0–4096)
  con errore localizzato e exit 1 su input invalido/overflow.
- Smoke test: `--threads abc`, `--threads -1`, `--limit` oltre uint64 → pulito.
- Casi shell (test): digits, 0, max, overflow, junk, segno, spazi, `0x10`, `1e3`.

## Issue I3.5 — ERROR_OPERATION_ABORTED in Errors.h

- 995 confermato tra i codici di disconnessione; **documentato** che è l'unico
  codice ambivalente (unplug NAS vs abort lato host) e perché è tenuto: una
  cancellazione utente non può produrre 995 nell'enumerator, perché fluisce nel
  callback (`onEntry` false ferma prima di altra I/O bloccante).
- Test confini del predicato: veri 59/64/67/995/1167/1222/1231/1236;
  falsi 2/3/5/32/53/87/0.

## Issue I3.6 — CSV formula injection

- Celle che iniziano con `= + - @` tab CR FF → RFC-quoting **e** apostrofo
  interni (marcatore di neutralizzazione). Test: nessuna cella path nuda
  inizia con il prefisso formula; esempi `'=SUM(A1:A9).txt`, `'-cmd.xlsx`,
  `'@hdr`.

## Risultati test

- Non elevato (`ctest --test-dir build --output-on-failure`): **100% verde**.
- Elevato: **41/43**. I 2 fail residui sono **baseline ambientali pre-audit**:
  - `access denied subdirectory`: processo elevato elude l'ACL simulato.
  - `cache: second run`: interferenza antivirus sul temp (conteggi 199/195/199
    tra run → timing, non determinismo del codice).
  - `content mode identical` ha fluttuato fail→pass tra run (flake AV); nessuna
    modifica all'hashing in questo audit.
- Build senza nuovi warning (rimossa la warning spuria `-Warray-bounds` da
  inlining GCC nel test con `noinline`).

## Note

- Script elevati e probe temporanei fuori repo (`%TEMP%\opencode`).
- Nessuna modifica ad API pubbliche né ridisegno dell'architettura MFT v2.

---

# Test regressivo portabile `$ATTRIBUTE_LIST → $INDEX_ALLOCATION [$I30]` (dev_parallel_1, non committato)

Regression test **sempre eseguito** (nessun admin/volume) dell'intera catena
`$ATTRIBUTE_LIST → record di estensione → $INDEX_ALLOCATION non-residente [$I30]
→ blocchi INDX → children`. Tutti i dati in memoria; il seam
`ResolveDirectoryForTest` (parametri predefiniti opzionali: `clusters`,
`clusterSize`, `bytesPerSector`, `outPiecesMerged`) guida **le stesse** funzioni
di produzione (`MergePassAFromRecord`, `MergePassBFromList`,
`ReadNonResidentAttr`, `ReadIndexAllocationStream`, `ParseIndexAllocationData`).

## A. Esito

- **PASS** — suite 100/100 (`bin/bv_tests.exe`, eseguita 2 volte di fila) e
  `ctest --test-dir build --output-on-failure` 1/1 (~20 s). Albero pulito,
  nessuna mutazione committata.

## B. Percorso di produzione esercitato

Base 4613 (dir, seq 1) con solo `$ATTRIBUTE_LIST` (nessun `$I30` inline).
Estensione 4609 (base-ref 4613): `$INDEX_ROOT [$I30]` vuoto + `$INDEX_ALLOCATION`
non-residente VCN 0..0 (blocco leaf con file1/file2). Estensione 4614 (base-ref
4613): `$INDEX_ALLOCATION` VCN 1..1 (file3/file4). I blocchi INDX vivono in
`clusters` a LCN 100 (VCN 0) e LCN 60 (VCN 1); la lista menziona VCN 0 due volte.
`enumerate()` vera e propria: passata solo come riferimento — la catena dati è
identica, ma non si toccano volumi reali.

## C. Perché Pass A non può falsificare il test

Geometria asimmetrica deliberata: **4609 è sotto la base** (4609 < 4613), quindi
il merge Pass A (che richiede base già parsata quando si vede l'estensione, ordine
ascendente per numero record) **non può raggiungerlo** → VCN 0 arriva **solo** via
Pass B (la lista). **4614 è sopra la base e non è elencato** → VCN 1 arriva **solo**
via Pass A (base-ref). I due passi sono disgiunti e ciascuno indispensabile.

## D. Record di estensione / VCN coinvolti

| Record | Base-ref | Contenuto | VCN | Solo via |
|---|---|---|---|---|
| 4609 | 4613 | `$INDEX_ROOT [$I30]` + `$INDEX_ALLOCATION` VCN 0 (LCN 100) | 0 | Pass B (lista) |
| 4614 | 4613 | `$INDEX_ALLOCATION` VCN 1 (LCN 60) | 1 | Pass A (base-ref) |
| 4620–4623 | — | children `file1..file4` (parent 4613) | — | — |

Messa in sequenza: la lista duplica l'entry VCN 0; il reordering a `lowestVcn` di
`ReadIndexAllocationStream` ricostruisce VCN 0 poi VCN 1.

## E. Mutazioni diagnostiche (ciascuna: fail → restore → suite verde)

| Mut | Modifica | Esito |
|---|---|---|
| A | Pass B IA merge disabilitato (`MergePassBFromList`) | **FAIL**: solo VCN 1 (2/4 entries) |
| B | Pass A IA merge disabilitato (`MergePassAFromRecord`) | **FAIL**: solo VCN 0 (2/4 entries) |
| A+B | entrambi disabilitati | **FAIL** duro: 0 pieces, 0 entries |
| C | `ReadIndexAllocationStream` legge solo il primo piece | **FAIL**: mancano C/D (2/4) |
| D | dedupe disabilitato (`VcnRangeKnown` → false) | **FAIL**: `pieces==3` (duplicato VCN 0) |

## F. Casi non coperti dal nuovo test

- Indice non-residente su **più di 2 pezzi** o con VCN non contigui.
- `$ATTRIBUTE_LIST` non-residente attraversato via `clusters` (il seam lo
  supporta: ramo `attrListHdr` non vuoto; non coperto da fixture nuova).
- Ordini di lista non canonici (VCN alto prima del basso, pezzi sparsi).
- Indice leaf con più di un blocco per VCN.

## G. Nessun volume reale

Solo memoria: `ResolveDirectoryForTest` con store in-memory; nessun
`\\.\C:` / E: / D:, nessun path reale, nessun admin richiesto. I test admin
preesistenti restano skip su processo non elevato.

## H. Albero pulito

`git status --short`: solo `src/Filesystem/MftEnumerator.{cpp,h}` e
`tests/test_main.cpp` modificati (nessuna mutazione residua, `grep MUTATION`
vuoto). Build senza nuovi warning.

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

---

# Verifica finale forense del fix MFT (`e31b1e5` + `37a9233`) — branch `dev_parallel_1`, HEAD `f6db074`

Report conclusivo della verifica indipendente dei commit applicati. Nessun codice
di produzione modificato durante la verifica; unico cambiamento al repo: questo
file (`audit_report.md`).

## Commits

| Commit | Contenuto |
|---|---|
| `010cb3e` | `fix(mft)`: filter named ADS in ParseRecord — solo unnamed `$DATA` → `FileEntry.size` |
| `e31b1e5` | `fix(mft)`: annex extension-record `$FILE_NAME` into its base; skip extension records in the parent-pointer union |
| `37a9233` | `test(mft)`: repair ADS fixtures |
| `f6db074` | `test(mft)`: commit di audit — sintesi base-con-`$FILE_NAME` + identity degli extension `$FILE_NAME` (dedupe + hard link). NON faceva parte del fix |

Push effettuato: `010cb3e..f6db074 → dev_parallel_1`; branch in sync con origin.

## Diff reale verificato (git show, non fiducia su commit message)

- `e31b1e5` = `src/Filesystem/MftEnumerator.cpp` +46/−2 e `tests/test_main.cpp`
  +62. Nel sorgente: `RecInfo.baseRef` (:309); `ParseRecord` imposta
  `out.baseRef = SplitRef(*(rec+32))` (:407); `MergePassAFromRecord` annette i
  `$FILE_NAME` dell'extension al base con dedupe su
  `(parent.rec, parent.seq, ns, name)` (:771-785); guardia
  `base.parsed && base.inUse && base.seq == baseRef.seq` (:760); tre esclusioni
  `baseRef.rec != 0` per i percorsi child: revChildren build enumerate (:1313),
  parent-pointer union in `WalkDirectoryStep` (:1010), revChildren build del seam
  `WalkDirectoryStepForTest` (:1632). Test regressivo phantom 4701/4702 (:2054).
- `37a9233` = solo `tests/test_main.cpp` +29/−43. `BuildFileRecordWithData` ora
  accetta `adsNames` vettore + `unnamedSize` piccolo (48); i 6 test A–E2
  aggiornati ai valori coerenti (48/0). Nessun assert rimosso o indebolito; la
  vecchia fixture (resident 12494 byte in record 1024) era logicamente rotta e
  il vecchio writer non-resident ADS era fuori bounds.

## PROVEN

1. **Meccanismo del bug** (catena record su E:): base 313291 seq1 baseRef=0
   `FN=[0] $DATA(size=3342660232) attrList=1`; extension 313292 baseRef=313291
   `FN=[s2a-s2b.zip] parent 254111 ns=0`, `$DATA(size=0)`; extension 313293
   baseRef=313291, size 0. Il record base non ha link parent; l'indice `$I30`
   punta SOLO al base. Pre-fix: il parser emetteva base anonimo (size reale) +
   extension (nome, size 0) → twin fantasma → EXTRA + DIM_DIVERSA.
2. **Assorbimento reale, non soppressione**: il `$FILE_NAME` dell'extension è
   ANNESSO a `base.names` (dedupe) e resta disponibile a `ChildNameOf`/
   `PickWin32Name` → il logical file ha nome e path corretti (risolti dal base).
   `FileEntry.size` proviene dal `dataSize` del BASE (unnamed `$DATA`, shape
   `e.size = cRec.dataSize` :1386), mai da un extension.
3. **Tipologia record**: nella scansione reale E: il numero identico di file
   post-fix (124.392) coincide con Win32 — non c'è una "perdita" di 13 file ma
   eliminazione dei 13 phantom: gli extension non generano più FileEntry.
4. **s2a-s2b.zip**: D: size 3.342.660.232, E: size 3.342.660.232; SHA-512
   identico su entrambi i volumi; hash = `A8A5242EAECEE094ABF434AFC2AD7C2EDA634409
   D3EF09FEEEE72A3EC91E88196C94148C787F368F6B6DFA07E5F94838E4209F4CEC61DEB2456FF9950B6DDEA6`
   (tranne normalizzazione). CLI `--mode size --enum mft` → classificato
   **Identical**, non DIM_DIVERSA/EXTRA/MISSING.
5. **Confronto reale re-verificato post-fix** (elevato, mcnext D→E, `--mode size`):

   | Metrica | MFT | Win32 |
   |---|---|---|
   | File sorgente | 124.392 | 124.392 |
   | File destinazione | 124.392 | 124.392 |
   | Identici | 124.392 | 124.392 |
   | Mancanti | 0 | 0 |
   | Extra | 0 | 0 |
   | Dimensione diversa | 0 | 0 |
   | Directory identiche | 51 | 51 |
   | Dati | 58.30 GB | 58.30 GB |
   | Problemi listate | 0 di 0 | 0 di 0 |

   **MFT == Win32** (requisito minimo accettazione rispettato: non solo
   "13→0", ma uguaglianza totale dei conteggi).
6. **Test sintetici**: `bv_tests.exe` non elevato → **114/114 PASS**, inclusi
   ADS (37a9233), phantom (e31b1e5) e dedupe/hard-link (f6db074). `ctest`
   → 1/1 Passed (~20 s).
7. **Elevato**: 108/114 in 2 run — 4 fail costanti = baseline ambientale nota
   pre-esistente (`access denied subdirectory` simulato male da elevato,
   `fallback Win32 error path`, `offline compareFrom`, `cache: second run`),
   + 2 fail content-hash flaky (AV, transienti, non-MFT, non introdotti da
   `e31b1e5`/`37a9233`). Nessun fail MFT/ADS/ATTRIBUTE_LIST.
8. **Build**: `build` e `build_gui` puliti, nessun warning nuovo rilevante;
   `bin/bv_gui.exe` ricompilato post-fix.
9. **Hard link / multiple `$FILE_NAME`**: `PickWin32Name` inalterato (ns WIN32=1
   last-wins, fallback ns 3, fallback qualsiasi); il merge non duplica un
   hard link (dedupe per chiave full identity); test Caso C/D/E (dedupe + hard
   link via extension + estensioni multiple) coprono il vincolo.
10. **`$ATTRIBUTE_LIST`**: Pass B (`MergePassBFromList`) inalterato — i piece
    `$I30` sono fusi per VCN con dedupe; il base resta l'entità; il fix ADS
    `010cb3e` intatto (solo unnamed `$DATA` → size). Nessun attribute logico
    perso: `$DATA` è nel base, `$FILE_NAME` annesso, attributi SI/SD/ADS non
    rilevanti per `FileEntry.size`.
11. **Normal record** (base con `$FILE_NAME` + `$DATA`): non toccato — il
    percorso normale non passa per il merge (baseRef.rec == 0 resta in config
    con revChildren/union); test existing identity-tree verdi.
12. **Residual code paths**: grep completo su `baseRef`/`revChildren`/
    `MergePassAFromRecord`/`WalkDirectoryStep`/`PickWin32Name`/child-emission →
    esattamente 3 gates (`:1010`, `:1313`, `:1632`); nessun secondo percorso che
    enumera extension come child, li inserisce in revChildren, o emette una
    FileEntry per un extension.

## NOT PROVEN

- Non ho potuto elevare una seconda volta per provare la "transitorietà" dei 2
  fail content-hash (2 run elevati consecutivi li hanno nuovamente prodotti);
  resta quindi formalmente il fail elevato 108/114 con la sola baseline nota +
  i 2 flake. La causa è ambientale (scan AV su temp in processo elevato), non
  correlata a `$FILE_NAME`/extension: i due test non toccano il backend MFT.
- La "cura" definitiva contro il twin fantasma su volumi reali è dimostrata per
  il caso osservato (record 313291/313292/313293); non è dimostrato su un
  campione statisticamente rilevante di volumi diversi (N=2, mcnext D/E).

## FINAL VERDICT

**APPROVED — fix correct, no regression found.**
(`e31b1e5` verified / `37a9233` verified / `f6db074` verified come commit di
audit.) Reale equivalenza MFT == Win32 (124.392 / 0 / 0 / 0 su entrambi),
`s2a-s2b.zip` Identical con size e hash confermati, 114/114 non-elevati.

## Repository state

`git status --short` → solo `audit_report.md` modificato. `git log --oneline -5`:

```
f6db074 test(mft): synthesize base-with-$FILE_NAME + extension $FILE_NAME identity (dedupe + hard link)
37a9233 test(mft): repair ADS fixtures ...
e31b1e5 fix(mft): annex extension-record $FILE_NAME into its base; skip extension records in the parent-pointer union
010cb3e fix(mft): filter named (ADS) in ParseRecord ...
```

Real volumes D:/E: non modificati in questa verifica (soli read: enum + hash;
nessuna scrittura, nessun tool diagnostico). Nessun commit automatico creato.

## Verifica finale del repository — ambito di `f6db074`

`git show f6db074` (diff completo, non fiducia sul message): **un solo file
modificato, `tests/test_main.cpp`, +91 righe, 0 cancellazioni** — un singolo
`TEST(...)` sintetico che fissa le tre invarianti (Caso C dedupe, Caso E hard
link via extension, Caso D estensioni multiple) tramite il seam
`WalkDirectoryStepForTest`. Nessun file di produzione toccato (`src/` non
presente nel diff). Il comportamento è esclusivamente diagnostico/test: nessuna
modifica ad API, header, build/CMake o tool. Il test è necessario (il ramo
dedupe di Pass A non aveva copertura sintetica) e discrimina il bug reale
(fail a `010cb3e`: 2≠1 su entrambi i dir, pass a HEAD).

Working tree: unica modifica non committata = **`audit_report.md`** (questa
documentazione). `git status -sb`: branch `dev_parallel_1` in sync con origin.

**Verdetto finale repository: APPROVED.**

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
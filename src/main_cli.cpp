// BackupVerifier CLI - Phase 1 (extended in Phase 5).
//
// Usage:
//   bv_cli --source <path> --dest <path> [--mode presence|size]
//          [--case-sensitive] [--enum auto|win32|mft] [--list-problems [--limit N]] [--help]
//   bv_cli --source <path> --snapshot-out <file> [--mode content]    (snapshot only)
//   bv_cli --compare <snapshot> --dest <path> [--mode content]       (offline compare)

#include <algorithm>
#include <cwchar>
#include <iostream>
#include <string>

#include "ScanController.h"
#include "Profiling/HashProfile.h"
#include "Util/StrictNumbers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace {

const wchar_t* kUsage =
    L"BackupVerifier CLI\n"
    L"\n"
    L"Uso:\n"
    L"  bv_cli --source <percorso> --dest <percorso> [opzioni]\n"
    L"  bv_cli --source <percorso> --snapshot-out <file> [opzioni]   (solo snapshot)\n"
    L"  bv_cli --compare <snapshot> --dest <percorso> [opzioni]      (confronto offline)\n"
    L"\n"
    L"Opzioni:\n"
    L"  --mode <presence|size|content>   modalita di confronto (default: presence)\n"
    L"  --threads <N>                    thread per la verifica contenuti (default: auto)\n"
    L"  --enum <auto|win32|mft>          backend di enumerazione (default: auto)\n"
    L"  --case-sensitive         confronto percorsi case-sensitive (default: insensibile)\n"
    L"  --list-problems          stampa le voci non identiche\n"
    L"  --limit <N>              numero massimo di voci stampate (default: 100)\n"
    L"  --progress               mostra l'avanzamento (fase, conteggi)\n"
    L"  --export <file>          esporta i problemi in CSV o JSON (estensione .json => JSON)\n"
    L"  --export-format <csv|json>  formato di esportazione esplicito\n"
    L"  --snapshot-out <file>    salva l'indice della sorgente (con hash in modalita content)\n"
    L"  --compare <snapshot>     confronta --dest contro uno snapshot (niente --source)\n"
    L"  --hash-cache <file>      riusa le impronte SHA-256 non scaricate (percorso+dim+data)\n"
    L"  --profile-hash           raccoglie e stampa le statistiche del profilo hash\n"
    L"  --profile-hash-jobs      come sopra e in piu' una riga per ogni file hashato\n"
    L"  -h, --help               mostra questo aiuto\n";

struct Args {
    std::wstring source;
    std::wstring dest;
    bv::ScanMode mode = bv::ScanMode::Presence;
    bool caseSensitive = false;
    bool listProblems = false;
    size_t limit = 100;
    unsigned int threads = 0; // 0 = auto
    bv::EnumeratorBackend backend = bv::EnumeratorBackend::Auto;
    bool progress = false;
    bool help = false;
    std::wstring exportPath;
    bv::exporting::ExportFormat exportFormat = bv::exporting::ExportFormat::Auto;
    std::wstring snapshotOut;
    std::wstring compareFrom;
    std::wstring hashCacheFile;
    bool profileHash = false;
    bool profileHashJobs = false;
};

bool ParseArgs(int argc, wchar_t** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--source" && i + 1 < argc) {
            out.source = argv[++i];
        } else if (a == L"--dest" && i + 1 < argc) {
            out.dest = argv[++i];
        } else if (a == L"--mode" && i + 1 < argc) {
            const std::wstring m = argv[++i];
            if (m == L"presence") out.mode = bv::ScanMode::Presence;
            else if (m == L"size") out.mode = bv::ScanMode::Size;
            else if (m == L"content") { out.mode = bv::ScanMode::Content; }
            else { std::wcerr << L"Modalita sconosciuta: " << m << L"\n"; return false; }
        } else if (a == L"--threads" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            if (!bv::util::ParseThreadCount(v, out.threads)) {
                std::wcerr << L"Valore non valido per --threads (atteso 0-"
                           << bv::util::kMaxThreads << L"): " << v << L"\n";
                return false;
            }
        } else if (a == L"--enum" && i + 1 < argc) {
            const std::wstring b = argv[++i];
            if (b == L"auto") out.backend = bv::EnumeratorBackend::Auto;
            else if (b == L"win32") out.backend = bv::EnumeratorBackend::Win32;
            else if (b == L"mft") out.backend = bv::EnumeratorBackend::Mft;
            else { std::wcerr << L"Backend sconosciuto: " << b << L"\n"; return false; }
        } else if (a == L"--case-sensitive") {
            out.caseSensitive = true;
        } else if (a == L"--list-problems") {
            out.listProblems = true;
        } else if (a == L"--progress") {
            out.progress = true;
        } else if (a == L"--limit" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            uint64_t parsed = 0;
            if (!bv::util::ParseUInt64(v, parsed)) {
                std::wcerr << L"Valore non valido per --limit: " << v << L"\n";
                return false;
            }
            out.limit = static_cast<size_t>(parsed);
        } else if (a == L"--export" && i + 1 < argc) {
            out.exportPath = argv[++i];
        } else if (a == L"--export-format" && i + 1 < argc) {
            const std::wstring f = argv[++i];
            if (f == L"csv") out.exportFormat = bv::exporting::ExportFormat::Csv;
            else if (f == L"json") out.exportFormat = bv::exporting::ExportFormat::Json;
            else { std::wcerr << L"Formato export sconosciuto: " << f << L"\n"; return false; }
        } else if (a == L"--snapshot-out" && i + 1 < argc) {
            out.snapshotOut = argv[++i];
        } else if (a == L"--compare" && i + 1 < argc) {
            out.compareFrom = argv[++i];
        } else if (a == L"--hash-cache" && i + 1 < argc) {
            out.hashCacheFile = argv[++i];
        } else if (a == L"--profile-hash") {
            out.profileHash = true;
        } else if (a == L"--profile-hash-jobs") {
            out.profileHash = true;
            out.profileHashJobs = true;
        } else if (a == L"-h" || a == L"--help") {
            out.help = true;
        } else {
            std::wcerr << L"Argomento sconosciuto: " << a << L"\n";
            return false;
        }
    }
    if (out.help) return true;

    if (!out.compareFrom.empty()) {
        if (!out.source.empty()) {
            std::wcerr << L"Errore: con --compare non si usa --source (la sorgente e lo snapshot).\n\n";
            return false;
        }
        if (out.dest.empty()) {
            std::wcerr << L"Errore: --compare richiede --dest.\n\n";
            return false;
        }
    } else if (!out.snapshotOut.empty()) {
        if (out.source.empty()) {
            std::wcerr << L"Errore: --snapshot-out richiede --source.\n\n";
            return false;
        }
    } else if (out.source.empty() || out.dest.empty()) {
        std::wcerr << L"Errore: --source e --dest sono obbligatori.\n\n";
        return false;
    }
    return true;
}

std::wstring Group(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    std::wstring out;
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (count == 3) {
            out.push_back(L'.');
            count = 0;
        }
        out.push_back(*it);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring HumanBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    std::wstring s = (u == 0) ? std::to_wstring(bytes) : std::to_wstring(static_cast<long long>(v * 10) / 10.0);
    // keep two meaningful decimals for the non-B units
    if (u > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%.2f", v);
        s = buf;
    }
    return s + L" " + units[u];
}

std::wstring FormatTime(double seconds) {
    const long long total = static_cast<long long>(seconds + 0.5);
    const long long h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    wchar_t buf[32];
    swprintf(buf, 32, L"%02lld:%02lld:%02lld", h, m, s);
    return buf;
}

// Bytes / seconds as a human rate string.
std::wstring FormatRate(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return L"n/d";
    double v = static_cast<double>(bytes) / seconds;
    const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    return buf;
}

// Seconds as a fixed-width wall-clock string with millisecond resolution.
std::wstring FmtSec(double seconds) {
    const int sec = static_cast<int>(seconds);
    const int h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    const int ms = static_cast<int>((seconds - sec) * 1000.0 + 0.5);
    if (h > 0) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%02d:%02d:%02d.%03d", h, m, s, ms);
        return buf;
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d.%03d s", s, ms);
    return buf;
}

// Peak rate as MB/s (1 MB = 1024^2 bytes).
std::wstring FmtMBS(uint64_t bytes, double seconds) {
    if (seconds <= 0.0) return L"n/d";
    const double mbs = (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
    wchar_t buf[64];
    swprintf(buf, 64, L"%.1f MB/s", mbs);
    return buf;
}

std::wstring FmtPct(double pct) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f %%", pct);
    return buf;
}

// Fixed number of decimal places, decimal comma.
std::wstring FormatFixed(double value, int decimals) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, value);
    std::wstring s = buf;
    return s;
}

void PrintHashProfile(const bv::profiling::HashProfileReport& p) {
    using bv::profiling::QpcToSeconds;
    const auto& a = p.side[static_cast<int>(bv::profiling::Side::Source)];
    const auto& b = p.side[static_cast<int>(bv::profiling::Side::Dest)];

    std::wcout << L"\n=== CONTENT HASH PROFILING ===\n";
    std::wcout << L"\nHash jobs:\n";
    std::wcout << L"  total:                 " << Group(p.tasks) << L"\n";
    std::wcout << L"  failed (threw):        " << Group(p.taskFailed) << L"\n";
    std::wcout << L"  active at end:         " << p.activeJobsAtEnd << L" (deve essere 0)\n";

    std::wcout << L"\nWorkers:\n";
    std::wcout << L"  max active jobs:       " << p.maxActiveJobs << L"\n";

    auto printSide = [&](const wchar_t* name, const bv::profiling::SideAggregate& s) {
        const double readSec = QpcToSeconds(s.readTicks);
        const double hashSec = QpcToSeconds(s.hashTicks);
        const double totalSec = QpcToSeconds(s.totalTicks);
        std::wcout << L"\nSide " << name << L":\n";
        std::wcout << L"  files:                 " << Group(s.files) << L"\n";
        std::wcout << L"  bytes (letti):         " << HumanBytes(s.bytes) << L"\n";
        std::wcout << L"  falliti:               " << Group(s.failed) << L"\n";
        std::wcout << L"  read time:             " << FmtSec(readSec) << L"\n";
        std::wcout << L"  hash time:             " << FmtSec(hashSec) << L"\n";
        std::wcout << L"  total time:            " << FmtSec(totalSec) << L"\n";
        std::wcout << L"  MB/s read:             " << FmtMBS(s.bytes, readSec) << L"\n";
        std::wcout << L"  MB/s sha:              " << FmtMBS(s.bytes, hashSec) << L"\n";
        std::wcout << L"  MB/s total:            " << FmtMBS(s.bytes, totalSec) << L"\n";
    };
    printSide(L"A (sorgente)", a);
    printSide(L"B (destinazione)", b);

    auto printStat = [&](const wchar_t* name, const bv::profiling::SideAggregate& s) {
        const double t1 = QpcToSeconds(s.statT1Ticks);
        const double t2 = QpcToSeconds(s.statT2Ticks);
        const double t12 = t1 + t2;
        const double avgMs = s.files > 0 ? t12 / s.files * 1000.0 : 0.0;
        std::wcout << L"\n" << name << L":\n";
        std::wcout << L"  T1 (StatFile pre-hash): " << Group(s.statT1Count) << L" chiamate  "
                   << FmtSec(t1) << L"\n";
        std::wcout << L"  T2 (StatFile post-hash):" << Group(s.statT2Count) << L" chiamate  "
                   << FmtSec(t2) << L"\n";
        std::wcout << L"  T1+T2:                  " << Group(s.statT1Count + s.statT2Count)
                   << L" chiamate  " << FmtSec(t12) << L"  media/file: "
                   << FormatFixed(avgMs, 3) << L" ms\n";
    };
    const double totStatTicks = a.statT1Ticks + a.statT2Ticks + b.statT1Ticks + b.statT2Ticks;
    const uint64_t totStatCalls = a.statT1Count + a.statT2Count + b.statT1Count + b.statT2Count;
    std::wcout << L"\n=== COSTO CONTROLLO T1/T2 (StatFile -> GetFileInformationByHandle) ===";
    printStat(L"Side A", a);
    printStat(L"Side B", b);
    std::wcout << L"\nTotale A+B: " << Group(totStatCalls) << L" chiamate  "
               << FmtSec(QpcToSeconds(totStatTicks))
               << L"  (confrontare con il Tempo totale della scansione qui sopra)\n";

    const double minActive = std::min(p.activeSecondsA, p.activeSecondsB);
    std::wcout << L"\nConcurrency:\n";
    std::wcout << L"  max A attivi:          " << p.maxActiveA << L"\n";
    std::wcout << L"  max B attivi:          " << p.maxActiveB << L"\n";
    std::wcout << L"  max A+B attivi:        " << p.maxAB << L"\n";
    std::wcout << L"  A attivo:              " << FmtSec(p.activeSecondsA) << L"\n";
    std::wcout << L"  B attivo:              " << FmtSec(p.activeSecondsB) << L"\n";
    std::wcout << L"  A+B sovrapposti:       " << FmtSec(p.overlapSeconds)
               << L"  (" << FmtPct(minActive > 0.0
                                      ? p.overlapSeconds / minActive * 100.0
                                      : 0.0)
               << L" del tempo del lato meno attivo)\n";

    std::wcout << L"\nBackpressure (ThreadPool):\n";
    std::wcout << L"  wait count:            " << Group(p.backpressureWaits) << L"\n";
    std::wcout << L"  total wait:            " << FmtSec(p.backpressureWaitSeconds) << L"\n";
    std::wcout << L"  waitAll count:         " << Group(p.waitAllCount) << L"\n";
    std::wcout << L"  total waitAll:         " << FmtSec(p.waitAllSeconds) << L"\n";
    std::wcout << L"  max outstanding:       " << p.maxOutstandingTasks << L"\n";
    std::wcout << L"  max queue depth:       " << p.maxQueueDepth << L"\n";
}

void PrintResults(const bv::ResultSet& r) {
    const bv::Stats& s = r.stats;
    std::wcout << L"\n=== RISULTATO ===\n";
    std::wcout << L"File sorgente:         " << Group(s.sourceFiles) << L"\n";
    std::wcout << L"File destinazione:     " << Group(s.destFiles) << L"\n";
    std::wcout << L"\n";
    std::wcout << L"Identici (file):       " << Group(s.identicalFiles) << L"\n";
    std::wcout << L"Mancanti (file):       " << Group(s.missingFiles) << L"\n";
    std::wcout << L"Extra (file):          " << Group(s.extraFiles) << L"\n";
    std::wcout << L"Dimensione diversa:    " << Group(s.sizeMismatch) << L"\n";
    std::wcout << L"Contenuto diverso:     " << Group(s.contentMismatch) << L"\n";
    std::wcout << L"Modificati durante scan:" << Group(s.changedDuringScan) << L"\n";
    std::wcout << L"Errori di lettura:     " << Group(s.readErrors) << L"\n";
    std::wcout << L"Accesso negato:        " << Group(s.accessDenied) << L"\n";
    std::wcout << L"\n";
    std::wcout << L"Directory identiche:   " << Group(s.identicalDirs) << L"\n";
    std::wcout << L"Directory mancanti:    " << Group(s.missingDirs) << L"\n";
    std::wcout << L"Directory extra:       " << Group(s.extraDirs) << L"\n";
    std::wcout << L"\n";
    std::wcout << L"Dati sorgente:         " << HumanBytes(s.bytesSource) << L"\n";
    std::wcout << L"Dati destinazione:     " << HumanBytes(s.bytesDest) << L"\n";
}

} // namespace

int MainImpl(int argc, wchar_t** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        std::wcerr << kUsage;
        return 1;
    }
    if (args.help) {
        std::wcout << kUsage;
        return 0;
    }

    bv::ScanOptions options;
    options.source = args.source;
    options.destination = args.dest;
    options.mode = args.mode;
    options.caseSensitive = args.caseSensitive;
    options.hashThreads = args.threads;
    options.backend = args.backend;
    options.snapshotOut = args.snapshotOut;
    options.compareFrom = args.compareFrom;
    options.exportPath = args.exportPath;
    options.exportFormat = args.exportFormat;
    options.hashCacheFile = args.hashCacheFile;

    // Caller-owned profiler: the controller feeds it and copies the aggregates
    // into ScanReport::hashProfile; we keep ownership to read the verbose
    // per-job records afterwards.
    bv::profiling::HashProfiler hashProfiler(args.profileHashJobs);
    if (args.profileHash || args.profileHashJobs) {
        hashProfiler.setEnabled(true);
        options.hashProfiler = &hashProfiler;
    }

    if (args.progress) {
        options.onProgress = [](const bv::ScanProgress& p) {
            const wchar_t* phase = p.phase == bv::ScanPhase::EnumerateSource
                                       ? L"enum sorgente"
                                       : p.phase == bv::ScanPhase::CompareDestination
                                             ? L"destinazione+confronto"
                                             : p.phase == bv::ScanPhase::Hashing
                                                   ? L"verifica contenuti"
                                                   : L"fine";
            std::wcout << L"\r[" << phase << L"] file=" << Group(p.files)
                       << L" dir=" << Group(p.dirs);
        };
    }

    if (!args.compareFrom.empty()) {
        std::wcout << L"Sorgente:      snapshot -> " << args.compareFrom << L"\n";
    } else {
        std::wcout << L"Sorgente:      " << options.source << L"\n";
    }
    if (!options.destination.empty()) {
        std::wcout << L"Destinazione:  " << options.destination << L"\n";
    }
    if (!args.snapshotOut.empty()) {
        std::wcout << L"Snapshot out:  " << args.snapshotOut << L"\n";
    }
    if (!args.hashCacheFile.empty()) {
        std::wcout << L"Cache hash:    " << args.hashCacheFile << L"\n";
    }
    std::wcout << L"Modalita:      "
               << (options.mode == bv::ScanMode::Presence ? L"Presenza"
                   : options.mode == bv::ScanMode::Size ? L"Dimensione"
                                                        : L"Contenuto")
               << L"\n";
    std::wcout << L"Case:          " << (options.caseSensitive ? L"sensibile" : L"insensibile") << L"\n";
    std::wcout << L"Backend:       "
               << (args.backend == bv::EnumeratorBackend::Mft
                       ? L"MFT (forza)"
                       : args.backend == bv::EnumeratorBackend::Win32
                             ? L"Win32 (forza)"
                             : L"auto (MFT se NTFS)") << L"\n\n";

bv::ScanController controller(options.caseSensitive);
    bv::ScanReport report;

    if (!args.compareFrom.empty()) {
        std::wcout << L"Caricamento snapshot...\n";
    } else {
        std::wcout << L"Enumerazione sorgente...\n";
    }
    if (!options.destination.empty()) {
        std::wcout << L"Enumerazione destinazione + confronto...\n";
    }
    report = controller.run(options);

    PrintResults(report.results);

    std::wcout << L"Backend usato:        "
               << (report.backendUsed == bv::EnumeratorBackend::Mft ? L"MFT"
                    : report.backendUsed == bv::EnumeratorBackend::Win32 ? L"Win32"
                                                                          : L"?") << L"\n";

    for (const std::wstring& note : report.notes) {
        std::wcout << L"NOTA: " << note << L"\n";
    }

    if (report.usedSnapshot) {
        std::wcout << L"Sorgente offline:     snapshot caricato (" << report.results.stats.sourceFiles
                   << L" voci)\n";
    }
    if (report.snapshotWritten) {
        std::wcout << L"Snapshot salvato:     " << options.snapshotOut << L"\n";
    }
    if (report.contentDegradedToSize) {
        std::wcout << L"ATTENZIONE: lo snapshot non contiene impronte SHA-256: la verifica\n"
                      L"           dei contenuti e stata abbassata al confronto per dimensione.\n";
    }
    if (report.exportWritten && !options.exportPath.empty()) {
        std::wcout << L"Esportazione:        " << options.exportPath << L" ("
                   << Group(report.results.problems.size()) << L" righe)\n";
    } else if (!options.exportPath.empty()) {
        std::wcout << L"Esportazione FALLITA: " << report.exportError << L"\n";
    }
    if (report.hashCacheHits > 0) {
        std::wcout << L"Cache hash:          " << Group(report.hashCacheHits)
                   << L" file non riletti\n";
    }
    if (report.hashingErrors > 0) {
        std::wcout << L"Errori hash (worker):" << Group(report.hashingErrors) << L"\n";
    }

    std::wcout << L"\nTempo totale:            " << FormatTime(report.secondsTotal) << L"\n";
    std::wcout << L"  - enum sorgente:       " << FormatTime(report.secondsEnumerateSource) << L"\n";
    std::wcout << L"  - destinazione+confr:  " << FormatTime(report.secondsDestinationPass) << L"\n";
    std::wcout << L"  - verifica contenuti:  " << FormatTime(report.secondsHashing) << L"\n";
    std::wcout << L"Thread (hash):           "
               << (report.hashThreadsUsed == 0 && options.mode != bv::ScanMode::Content
                       ? L"non usati"
                       : std::to_wstring(report.hashThreadsUsed))
               << L"\n";
    std::wcout << L"Velocita effettiva:      "
               << FormatRate(report.results.stats.bytesSource, report.secondsTotal) << L"\n";

    if (options.hashProfiler) {
        PrintHashProfile(report.hashProfile);
        if (args.profileHashJobs) {
            std::wcout << L"\n=== HASH JOB LOG (una riga per file hashato) ===\n";
            for (const auto& r : hashProfiler.jobRecords()) {
                std::wcout << L"job=" << r.jobId << L" side=" << bv::profiling::SideName(r.side)
                           << L" size=" << Group(r.expectedSize)
                           << L" bytes=" << Group(r.bytesRead)
                           << L" read=" << FmtSec(bv::profiling::QpcToSeconds(r.readTicks))
                           << L" hash=" << FmtSec(bv::profiling::QpcToSeconds(r.hashTicks))
                           << L" total=" << FmtSec(bv::profiling::QpcToSeconds(r.totalTicks))
                           << L" ok=" << (r.ok ? 1 : 0) << L" path=" << r.path << L"\n";
            }
        }
    }

    if (!report.sourceOk) {
        std::wcout << L"\nATTENZIONE: la radice sorgente non e accessibile.\n";
    }
    if (!report.destinationOk) {
        std::wcout << L"\nATTENZIONE: la radice destinazione non e accessibile.\n";
    }
    if (report.pathCollisions > 0) {
        std::wcout << L"ATTENZIONE: " << Group(report.pathCollisions)
                   << L" percorsi distinti coincidono dopo il case-fold (policy last-wins: "
                      L"resta l'ultima voce incontrata).\n";
    }

    if (args.listProblems) {
        const auto& problems = report.results.problems;
        const size_t n = std::min(problems.size(), args.limit);
        std::wcout << L"\n=== PROBLEMI (prime " << n << L" di " << problems.size() << L") ===\n";
        const wchar_t* names[] = {
            L"IDENTICO", L"MANCANTE", L"EXTRA", L"DIM_DIVERSA",
            L"CONTENUTO_DIVERSO", L"ERRORE_LETTURA", L"ACCESSO_NEGATO",
            L"MODIFICATO_DURANTE_SCAN"};
        for (size_t i = 0; i < n; ++i) {
            const bv::FileResult& p = problems[i];
            const wchar_t* name = names[static_cast<int>(p.status)];
            std::wcout << name << L"\t" << (p.isDirectory ? L"[dir] " : L"")
                       << p.relativePath;
            if (!p.errorMessage.empty()) std::wcout << L"  (" << p.errorMessage << L")";
            std::wcout << L"\n";
        }
    }

    return 0;
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const int rc = MainImpl(argc, argv);
    LocalFree(argv);
    return rc;
}

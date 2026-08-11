// BackupVerifier CLI - Phase 1.
//
// Usage:
//   bv_cli --source <path> --dest <path> [--mode presence|size]
//          [--case-sensitive] [--enum auto|win32|mft] [--list-problems [--limit N]] [--help]

#include <algorithm>
#include <cwchar>
#include <iostream>
#include <string>

#include "ScanController.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace {

const wchar_t* kUsage =
    L"BackupVerifier CLI (Fase 1)\n"
    L"\n"
    L"Uso:\n"
    L"  bv_cli --source <percorso> --dest <percorso> [opzioni]\n"
    L"\n"
    L"Opzioni:\n"
    L"  --mode <presence|size|content>   modalita di confronto (default: presence)\n"
    L"  --threads <N>                    thread per la verifica contenuti (default: auto)\n"
    L"  --enum <auto|win32|mft>          backend di enumerazione (default: auto)\n"
    L"  --case-sensitive         confronto percorsi case-sensitive (default: insensibile)\n"
    L"  --list-problems          stampa le voci non identiche\n"
    L"  --limit <N>              numero massimo di voci stampate (default: 100)\n"
    L"  --progress               mostra l'avanzamento (fase, conteggi)\n"
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
            out.threads = static_cast<unsigned int>(std::stoul(argv[++i]));
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
            out.limit = std::stoull(argv[++i]);
        } else if (a == L"-h" || a == L"--help") {
            out.help = true;
        } else {
            std::wcerr << L"Argomento sconosciuto: " << a << L"\n";
            return false;
        }
    }
    if (!out.help && (out.source.empty() || out.dest.empty())) {
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

    std::wcout << L"Sorgente:      " << options.source << L"\n";
    std::wcout << L"Destinazione:  " << options.destination << L"\n";
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

    std::wcout << L"Enumerazione sorgente...\n";
    report = controller.run(options);

    std::wcout << L"Enumerazione destinazione + confronto...\n";

    PrintResults(report.results);

    std::wcout << L"Backend usato:        "
               << (report.backendUsed == bv::EnumeratorBackend::Mft ? L"MFT"
                    : report.backendUsed == bv::EnumeratorBackend::Win32 ? L"Win32"
                                                                         : L"?") << L"\n";

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

    if (!report.sourceOk) {
        std::wcout << L"\nATTENZIONE: la radice sorgente non e accessibile.\n";
    }
    if (!report.destinationOk) {
        std::wcout << L"\nATTENZIONE: la radice destinazione non e accessibile.\n";
    }

    if (args.listProblems) {
        const auto& problems = report.results.problems;
        const size_t n = std::min(problems.size(), args.limit);
        std::wcout << L"\n=== PROBLEMI (prime " << n << L" di " << problems.size() << L") ===\n";
        const wchar_t* names[] = {
            L"IDENTICO", L"MANCANTE", L"EXTRA", L"DIM_DIVERSA",
            L"CONTENUTO_DIVERSO", L"ERRORE_LETTURA", L"ACCESSO_NEGATO"};
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

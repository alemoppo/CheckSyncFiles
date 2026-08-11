// MFT vs Win32 enumeration benchmark + correctness check (Phase 4).
//
// Enumerates the same root with both back-ends and verifies they produce the
// exact same set of (relativePath, size, isDirectory). Prints timing for both.
//
// Usage:
//   bv_mftbench --root <path>
//   bv_mftbench --root <path> --gen <N>     create an N-file stress tree first

#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "Filesystem/MftEnumerator.h"
#include "Filesystem/PathUtil.h"
#include "Filesystem/Win32Enumerator.h"
#include "TestTree.h"

namespace {

using namespace bv;

struct Node {
    uint64_t size = 0;
    bool isDir = false;
};

using Snapshot = std::map<std::wstring, Node>;

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Enumerates `root` with `en` into a snapshot. Returns false if the root could
// not be enumerated (back-end not usable).
bool Enumerate(IFileEnumerator& en, const std::wstring& root, Snapshot& out) {
    const double t0 = NowSeconds();
    const bool ok = en.enumerate(
        root,
        [&](FileEntry&& e) {
            Node n;
            n.size = e.size;
            n.isDir = e.isDirectory;
            out.emplace(e.relativePath, n);
            return true;
        },
        [&](const ScanError&) {});
    const double dt = NowSeconds() - t0;
    std::wcout << L"      elapsed: " << dt << L" s\n";
    return ok;
}

int Compare(const Snapshot& a, const Snapshot& b) {
    if (a.size() != b.size()) {
        std::wcout << L"      MISMATCH sizes: win32=" << a.size() << L" mft=" << b.size() << L"\n";
    }
    int diffs = 0;
    auto ia = a.begin();
    auto ib = b.begin();
    while (ia != a.end() && ib != b.end()) {
        if (ia->first != ib->first || ia->second.size != ib->second.size ||
            ia->second.isDir != ib->second.isDir) {
            if (diffs < 10) {
                std::wcout << L"      diff: win32=[" << ia->first << L"](" << ia->second.size
                           << L"," << ia->second.isDir << L") mft=[" << ib->first << L"]("
                           << ib->second.size << L"," << ib->second.isDir << L")\n";
            }
            ++diffs;
        }
        if (ia->first < ib->first) {
            ++ia;
        } else if (ib->first < ia->first) {
            ++ib;
        } else {
            ++ia;
            ++ib;
        }
    }
    return diffs;
}

} // namespace

int MainImpl(int argc, wchar_t** argv) {
    std::wstring root;
    size_t gen = 0;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--root" && i + 1 < argc) {
            root = argv[++i];
        } else if (a == L"--gen" && i + 1 < argc) {
            gen = std::stoull(argv[++i]);
        } else {
            std::wcerr << L"Argomento sconosciuto: " << a << L"\n";
            return 1;
        }
    }

    if (gen > 0) {
        const std::wstring tmp = L"C:\\Users\\alemo\\AppData\\Local\\Temp\\bv_mftbench_gen";
        if (root.empty()) root = tmp;
        testgen::CreateStressTree(root, gen);
    }
    if (root.empty()) {
        std::wcerr << L"Specificare --root <path> (o --gen <N>).\n";
        return 1;
    }

    std::wcout << L"Root: " << root << L"\n";

    std::wcout << L"Win32:\n";
    Snapshot win;
    Win32Enumerator wen;
    if (!Enumerate(wen, root, win)) {
        std::wcerr << L"Win32 fallito\n";
        return 1;
    }

    std::wcout << L"MFT:  (supported=" << (MftEnumerator::IsSupported(root) ? L"yes" : L"no") << L")\n";
    Snapshot mft;
    MftEnumerator men;
    const bool mftOk = Enumerate(men, root, mft);
    if (!mftOk) {
        std::wcout << L"      MFT non utilizzabile -> fallback Win32 (atteso per UNC/non-NTFS).\n";
        return 0;
    }

    std::wcout << L"Win32 files=" << win.size() << L"  MFT files=" << mft.size() << L"\n";
    const int diffs = Compare(win, mft);
    if (diffs == 0) {
        std::wcout << L"RISULTATO: CORRISPONDENZA (MFT == Win32)\n";
        return 0;
    }
    std::wcout << L"RISULTATO: DIFFERENZE=" << diffs << L"\n";
    return 2;
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const int rc = MainImpl(argc, argv);
    LocalFree(argv);
    return rc;
}
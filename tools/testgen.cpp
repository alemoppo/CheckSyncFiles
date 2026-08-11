// Test tree generator. Creates trees under a given root for manual testing
// and benchmarking.
//
// Usage:
//   bv_testgen <root> [--fixture] [--differing] [--stress N] [--large MB]
//
// Examples:
//   bv_testgen C:\temp\demo --fixture
//   bv_testgen C:\temp\demo --differing
//   bv_testgen C:\temp\demo --stress 50000
//   bv_testgen C:\temp\demo --large 2048   (creates a 2 GiB sparse file in src/dst)

#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>

#include "TestTree.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;

int MainImpl(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcout << L"Uso: bv_testgen <root> [--fixture] [--differing] [--stress N] [--large MB]\n";
        return 1;
    }

    const std::wstring root = argv[1];
    fs::create_directories(root);

    bool any = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--fixture") {
            bv::testgen::CreateFixture(root);
            std::wcout << L"Fixture creata in " << root << L"\n";
            any = true;
        } else if (a == L"--differing") {
            bv::testgen::CreateDifferingTrees(root);
            std::wcout << L"Alberi src/dst con differenze creati in " << root << L"\n";
            any = true;
        } else if (a == L"--stress" && i + 1 < argc) {
            const size_t n = std::stoull(argv[++i]);
            const size_t created = bv::testgen::CreateStressTree(root + L"\\stress", n);
            std::wcout << L"Creati " << created << L" file (stress)\n";
            any = true;
        } else if (a == L"--large" && i + 1 < argc) {
            const uint64_t mb = std::stoull(argv[++i]);
            fs::create_directories(root + L"\\src");
            fs::create_directories(root + L"\\dst");
            bv::testgen::CreateFileOfSize(root + L"\\src\\big.bin", mb * 1024 * 1024);
            bv::testgen::CreateFileOfSize(root + L"\\dst\\big.bin", mb * 1024 * 1024);
            std::wcout << L"Creato big.bin da " << mb << L" MiB in src e dst (sparse)\n";
            any = true;
        } else {
            std::wcerr << L"Opzione sconosciuta: " << a << L"\n";
            return 1;
        }
    }

    if (!any) {
        std::wcerr << L"Nessuna azione richiesta.\n";
        return 1;
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

#include "../RuntimeFiles.h"
#include "audastra_japi.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <atomic>

namespace {
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
struct Module {
    HMODULE handle = nullptr;
    using Set = void(__cdecl*)(int); using Probe = int(__cdecl*)();
    Set set = nullptr; Probe probe = nullptr;
    Probe initialize = nullptr, installCount = nullptr;
    explicit Module(const std::filesystem::path& path) {
        handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(handle != nullptr, "load private owner fixture");
        set = reinterpret_cast<Set>(GetProcAddress(handle, "SetOwnerFixtureState"));
        probe = reinterpret_cast<Probe>(GetProcAddress(handle, "ProbeOwnerFixture"));
        initialize = reinterpret_cast<Probe>(GetProcAddress(handle, "WarVKAIInit"));
        installCount = reinterpret_cast<Probe>(GetProcAddress(handle, "OwnerFixtureInstallCount"));
        Require(set && probe && initialize && installCount, "resolve owner fixture exports");
    }
    ~Module() { if (handle) FreeLibrary(handle); }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::temp_directory_path() / (L"audastra-owner-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()));
    try {
        Require(std::filesystem::create_directory(root), "private fixture directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
        for (const auto name : {L"a.dll", L"b.dll", L"c.dll", L"different.dll"})
            Require(std::filesystem::copy_file(argv[1], root / name), "copy fixture under independent paths");
        Module a(root / L"a.dll");
        Require(a.probe() == 0, "unactivated runtime has no owner");
        Module b(root / L"b.dll");
        Require(b.probe() == 0, "loaded same-identity copies are not yet owners");
        std::array<int, 8> firstResults{}; std::vector<std::thread> firstWorkers;
        std::atomic<bool> begin{false};
        for (std::size_t i = 0; i < firstResults.size(); ++i) firstWorkers.emplace_back([&,i] {
            while (!begin.load()) std::this_thread::yield();
            firstResults[i] = (i % 2 ? a : b).initialize();
        });
        begin.store(true);
        for (auto& worker : firstWorkers) worker.join();
        for (auto result : firstResults) Require(result == 123, "cross-copy initialization never inverts module and process locks");
        Require(a.installCount() + b.installCount() == 1, "simultaneous first initializations install exactly one owner");
        a.set(0); b.set(0);
        a.set(1);
        Require(b.probe() == 123, "same content at another path reuses the owner");
        std::array<int, 8> results{}; std::vector<std::thread> workers;
        for (std::size_t i = 0; i < results.size(); ++i) workers.emplace_back([&,i] { results[i] = b.probe(); });
        for (auto& worker : workers) worker.join();
        for (auto value : results) Require(value == 123, "concurrent duplicate initialization resolves one owner");
        Module c(root / L"c.dll"); b.set(1);
        Require(c.probe() == -AJ_POLICY, "ambiguous existing owners rejected");
        b.set(0); a.set(2);
        Require(b.probe() == -AJ_POLICY, "failed owner cannot be replaced");
        a.set(1);
        {
            std::ofstream file(root / L"different.dll", std::ios::binary | std::ios::app);
            file.put('X'); Require(file.good(), "make distinct file content with identical exports");
        }
        Module different(root / L"different.dll");
        Require(different.probe() == -AJ_POLICY, "different SHA cannot reuse an existing owner");
        Require(a.probe() == -AJ_POLICY, "unknown competing content is never invoked");
        std::cout << "process owner fixtures passed; no game hooks or AI files used\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

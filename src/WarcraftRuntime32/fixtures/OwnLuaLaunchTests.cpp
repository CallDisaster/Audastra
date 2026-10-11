#include "../RuntimeFiles.h"
#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace Audastra::Runtime32;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }
std::string Read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
struct Result { DWORD exit = STILL_ACTIVE; std::string receipt; };
struct Fixture {
    std::filesystem::path root, temporaryRoot, launcher, bootstrap;
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    unsigned sequence = 0;
    std::string sha;
    static constexpr wchar_t map[] = L"Maps\\Audastra\\fixture.w3x";
    Fixture(wchar_t** argv) {
        std::array<wchar_t, 4096> temporary{};
        Check(GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) != 0, "temporary root");
        temporaryRoot = std::filesystem::weakly_canonical(temporary.data());
        root = temporaryRoot / (L"audastra-owned-launch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Check(PlainPath(root) && CreateDirectoryW(root.c_str(), nullptr), "exclusive fixture root");
        std::filesystem::create_directories(root / L"Maps" / L"Audastra");
        launcher = argv[1]; bootstrap = argv[2];
        for (const auto& [source, name] : std::array<std::pair<std::filesystem::path, std::filesystem::path>, 2>{
            std::pair{argv[3], L"war3.exe"}, std::pair{argv[4], L"Game.dll"}}) {
            const auto destination = root / name;
            Check(std::filesystem::copy_file(source, destination), "copy original fixture binary");
            Record(destination);
        }
        { std::ofstream output(root / map, std::ios::binary); output << "original synthetic map identity fixture"; }
        Record(root / map); sha = files.back().second;
    }
    void Record(const std::filesystem::path& file) {
        Handle input(OpenLockedFile(file));
        const auto hash = input.value == INVALID_HANDLE_VALUE ? std::string{} : Sha256(input.value);
        Check(hash.size() == 64, "freeze created fixture identity");
        files.emplace_back(file, hash);
    }
    std::wstring Owned(std::string_view mapHash = {}) const {
        const auto hash = mapHash.empty() ? sha : std::string(mapHash);
        return L" --runtime-mode owned-lua --lua-map-sha256 " + std::wstring(hash.begin(), hash.end())
            + L" --lua-session-nonce aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
    Result Run(std::wstring arguments, HANDLE mailbox = nullptr, bool inheritMailbox = true, std::wstring_view requestedMap = map) {
        const auto receipt = root / (L"receipt-" + std::to_wstring(++sequence) + L".json");
        auto command = Quote(launcher) + L" --game " + Quote(root / L"war3.exe") + L" --map \"" + std::wstring(requestedMap)
            + L"\" --bootstrap " + Quote(bootstrap) + L" --receipt " + Quote(receipt) + arguments;
        if (mailbox) command += L" --lua-mailbox-handle " + std::to_wstring(reinterpret_cast<ULONG_PTR>(mailbox));
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Check(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)), "owned launch Job");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        std::vector<std::byte> attributeBytes;
        if (mailbox && inheritMailbox) {
            SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            attributeBytes.resize(bytes);
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
            Check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)
                && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    &mailbox, sizeof(mailbox), nullptr, nullptr), "exact mailbox inheritance");
        }
        PROCESS_INFORMATION process{};
        const auto created = CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, mailbox && inheritMailbox,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | (startup.lpAttributeList ? EXTENDED_STARTUPINFO_PRESENT : 0),
            nullptr, root.c_str(), &startup.StartupInfo, &process);
        if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
        Check(created, "create original launcher fixture");
        Handle child(process.hProcess), thread(process.hThread);
        struct StopOriginal {
            HANDLE process;
            ~StopOriginal() {
                if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                    TerminateProcess(process, 1); (void)WaitForSingleObject(process, 5000);
                }
            }
        } stop{child.value};
        Check(AssignProcessToJobObject(job.value, child.value), "original launcher Job membership");
        Check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "resume original launcher");
        Check(WaitForSingleObject(child.value, 10000) == WAIT_OBJECT_0, "bounded launcher exit");
        Result result;
        Check(GetExitCodeProcess(child.value, &result.exit), "launcher exit evidence");
        if (std::filesystem::exists(receipt)) { result.receipt = Read(receipt); Record(receipt); }
        return result;
    }
    void Cleanup() {
        Check(std::filesystem::weakly_canonical(root).parent_path() == temporaryRoot && PlainPath(root), "cleanup root identity");
        for (const auto& [path, hash] : files) {
            Check(PlainPath(path) && path.lexically_relative(root).native().find(L"..") == std::wstring::npos, "cleanup member location");
            Handle input(OpenLockedFile(path)); Check(input.value != INVALID_HANDLE_VALUE && Sha256(input.value) == hash, "cleanup original file identity");
        }
        for (const auto& [path, hash] : files) { (void)hash; Check(std::filesystem::remove(path), "cleanup created file"); }
        Check(std::filesystem::remove(root / L"Maps" / L"Audastra") && std::filesystem::remove(root / L"Maps")
            && std::filesystem::remove(root), "cleanup only empty owned directories");
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 5, "fixture arguments");
        Fixture fixture(argv);
        const auto independent = fixture.Run(fixture.Owned());
        Check(independent.exit != 0 && independent.receipt.find("owned-lua-game-identity") != std::string::npos
            && independent.receipt.find("\"runtimeMode\":\"owned-lua\"") != std::string::npos
            && independent.receipt.find("\"processCreated\":true") != std::string::npos
            && independent.receipt.find("runtime-preflight") == std::string::npos,
            "owned mode bypasses six external DLLs but rejects unverified Game image");
        const auto minimized = std::filesystem::path(L"Maps\\Audastra\\minimized.w3x");
        Check(std::filesystem::copy_file(fixture.root / Fixture::map, fixture.root / minimized), "original startup-policy fixture map");
        fixture.Record(fixture.root / minimized);
        const auto inactive = fixture.Run(fixture.Owned() + L" --owned-timeout-ms 1000", nullptr, true, minimized.wstring());
        Check(inactive.receipt.find("owned-lua-game-identity") != std::string::npos
            && inactive.receipt.find("\"minimizedNoActivateRequested\":true") != std::string::npos,
            "observation child receives actual minimized no-activate STARTUPINFO before Game loads");
        Check(independent.receipt.find("\"minimizedNoActivateRequested\":false") != std::string::npos,
            "default launch keeps original display behavior");
        const auto mismatch = fixture.Run(fixture.Owned(std::string(64, 'b')));
        Check(mismatch.receipt.find("owned-lua-map-sha") != std::string::npos
            && mismatch.receipt.find("\"processCreated\":false") != std::string::npos, "wrong frozen map refused before game");
        Check(fixture.Run(fixture.Owned() + L" --runtime \"" + fixture.root.wstring() + L"\"").exit == 64,
            "owned mode rejects mixed external runtime");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(LuaTestMailbox), nullptr));
        auto* mailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LuaTestMailbox)));
        Check(mailbox != nullptr, "anonymous fixture mailbox");
        ::new (mailbox) LuaTestMailbox{};
        strcpy_s(mailbox->sessionNonce, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        strcpy_s(mailbox->mapSha256, fixture.sha.c_str());
        mailbox->version = 1;
        Check(fixture.Run(fixture.Owned(), mapping.value).receipt.find("owned-lua-mailbox-identity") != std::string::npos,
            "old mailbox version refused by actual launcher");
        mailbox->version = kLuaMailboxVersion;
        mailbox->sessionNonce[0] = 'b';
        Check(fixture.Run(fixture.Owned(), mapping.value).receipt.find("owned-lua-mailbox-identity") != std::string::npos,
            "foreign mailbox nonce refused");
        mailbox->sessionNonce[0] = 'a';
        Check(fixture.Run(fixture.Owned(), mapping.value).receipt.find("owned-lua-game-identity") != std::string::npos
            && mailbox->runtimeState == static_cast<LONG>(LuaRuntimeState::Starting), "inherited mailbox not falsely marked ready");
        const auto absent = fixture.Run(fixture.Owned(), mapping.value, false);
        Check(absent.exit != 0 && absent.receipt.find("\"processCreated\":false") != std::string::npos,
            "non-inherited numeric handle cannot launch game");
        UnmapViewOfFile(mailbox);
        fixture.Cleanup();
        std::cout << "Owned Lua launch identity fixture passed; no actual Game natives executed; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

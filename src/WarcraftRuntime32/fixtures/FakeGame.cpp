#include "../RuntimeProtocol.h"
#include <filesystem>
#include <string>

extern "C" int StartFakeGame() {
    auto* shared = Audastra::Runtime32::OpenChannel();
    if (!shared) return 43;
    // 裸模式不加载任何扩展插件；其余模式要求两个插件都真正初始化。
    const bool bare = shared->runtimeMode == Audastra::Runtime32::RuntimeMode::Bare;
    const auto success = shared->hookInstalled && shared->gameObserved && shared->runtimeInitialized
        && (bare ? shared->fixturePluginBits == 0 && !shared->modules[4].initializeCalled && !shared->modules[5].initializeCalled
            : shared->fixturePluginBits == 3 && shared->modules[4].initializeCalled && shared->modules[5].initializeCalled);
    if (success && shared->probeEnabled) {
        if (!shared->probeWriteGuardInstalled || !shared->probeObserverInstalled) return 61;
        const std::wstring command(GetCommandLineW());
        if (command.find(L"probe-blocked-") != std::wstring::npos) {
            auto path = std::filesystem::path(shared->probeNonfatalBlockedPaths[0].data());
            if (command.find(L"probe-blocked-other") != std::wstring::npos || command.find(L"probe-blocked-third") != std::wstring::npos)
                path = std::filesystem::path(shared->probeWriteRoot).parent_path() / L"forbidden-write.txt";
            else if (command.find(L"probe-blocked-author") != std::wstring::npos)
                path = std::filesystem::path(shared->runtimeRoot) / L"forbidden-author.txt";
            const bool two = command.find(L"probe-blocked-two") != std::wstring::npos;
            const auto attempts = command.find(L"probe-blocked-overflow") != std::wstring::npos ? 35 : (two ? 2 : 1);
            for (int index = 0; index < attempts; ++index) {
                if (two && index == 1) path = std::filesystem::path(shared->probeNonfatalBlockedPaths[1].data());
                const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return 62; }
                if (GetLastError() != ERROR_ACCESS_DENIED) return 66;
            }
            if (shared->failed) Sleep(INFINITE);
        }
        if (command.find(L"probe-outside.w3x") != std::wstring::npos) {
            const auto path = std::filesystem::path(shared->probeWriteRoot).parent_path() / L"forbidden-write.txt";
            const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return 62; }
            Sleep(INFINITE); // 宿主在观察到 writeDenied 后只能结束这个精确新进程。
        }
        if (command.find(L"probe-hardlink.w3x") != std::wstring::npos) {
            const auto path = std::filesystem::path(shared->probeLogPath).parent_path() / L"aliased-write.txt";
            // CREATE_ALWAYS 本身会截断，必须在进入原 NT 创建函数前拒绝既有 alias。
            const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return 62; }
            Sleep(INFINITE);
        }
        if (command.find(L"probe-redirect.w3x") != std::wstring::npos || command.find(L"probe-sandbox.w3x") != std::wstring::npos) {
            const auto path = command.find(L"probe-redirect.w3x") != std::wstring::npos
                ? std::filesystem::path(shared->runtimeRoot) / L"logs" / L"war3.log"
                : std::filesystem::path(shared->probeLogPath).parent_path() / L"owned-write.txt";
            const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return 63;
            const char text[] = "original isolated fixture log"; DWORD written = 0;
            const auto ok = WriteFile(file, text, sizeof(text) - 1, &written, nullptr); CloseHandle(file);
            if (!ok || written != sizeof(text) - 1) return 64;
        }
        const auto lua = GetModuleHandleW(L"lua53.dll");
        const auto create = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(lua, "CreateFakeLuaProbeState"));
        const auto call = reinterpret_cast<int (*)(void*, int, int, int, std::intptr_t, void*)>(GetProcAddress(lua, "lua_pcallk"));
        const auto destroy = reinterpret_cast<void (*)(void*)>(GetProcAddress(lua, "DeleteFakeLuaProbeState"));
        if (!create || !call || !destroy) return 65;
        const auto marker = std::string("AUDASTRA_LUA_API_BEGIN_V1:")
            + (command.find(L"probe-nonce.w3x") == std::wstring::npos ? shared->probeNonce : "00000000000000000000000000000000");
        const auto state = create(marker.c_str()); call(state, 0, 1, 0, 0, nullptr); destroy(state);
        Sleep(INFINITE);
    }
    UnmapViewOfFile(shared);
    return success ? 0 : 44;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

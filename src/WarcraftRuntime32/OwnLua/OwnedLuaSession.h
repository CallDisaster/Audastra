#pragma once

#include "GameImage127.h"
#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include <functional>

namespace audastra::japi { class Host; }

namespace Audastra::Runtime32::OwnLua {

struct OwnedLuaSessionConfig {
    std::filesystem::path mapPath;
    std::string mapSha256;
    std::string nonce;
    LuaTestMailbox* mailbox = nullptr;
    bool hotReload = false;
    bool experimentalJapi = false; // 未通过真实游戏验收，默认不改派字符串 native。
    // 运行时自带的 JAPI 模块（界面库等），每个新 VM 的 Host 建好后装入；只在 experimentalJapi 时使用。
    std::function<std::int32_t(const GameImage127&, audastra::japi::Host&)> japiModules;
};

struct OwnedLuaObservation {
    std::uint32_t threadId = 0;
    std::uint32_t nativeCount = 0;
    std::uint32_t modules = 0;
    std::uint32_t callbacks = 0;
    std::uint32_t polls = 0;
    std::uint32_t errors = 0;
    std::uint64_t nativeCalls = 0;
    bool ready = false;
};

struct OwnedLuaSessionEvents {
    std::function<void(const OwnedLuaObservation&)> observe;
    // 必须停止当前测试进程；VM 失效后不能让游戏继续使用自有 code 身份。
    std::function<void(std::string_view)> fatal;
};

// 只安装主线程入口观察，不在这里执行地图。成功安装的会话随本次进程存活，
// 不在 DllMain 析构；原游戏主 VM 析构前清空 Lua 引用，禁止跨局复用。
[[nodiscard]] bool InstallOwnedLuaSession(std::shared_ptr<GameImage127> image,
    OwnedLuaSessionConfig config, OwnedLuaSessionEvents events, std::string& error);

} // namespace Audastra::Runtime32::OwnLua

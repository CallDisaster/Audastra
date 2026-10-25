#pragma once

#include "GameImage127.h"
#include "NativeRegistry.h"
#include "NativeProvider.h"
#include <functional>

namespace Audastra::Runtime32::OwnLua {
struct GameNativeContext {
    std::shared_ptr<GameImage127> image;
    std::shared_ptr<const NativeRegistry> registry;
    std::uint64_t registrationGeneration = 0;
    bool codeDispatchReady = false;
};
struct GameNativeHandlers {
    // 回调都在触发该动作的游戏线程调用；调用方不能在其中另建脚本执行线程。
    std::function<void()> reset;
    std::function<void(const GameNativeContext&, std::string_view module)> module;
    std::function<std::optional<NativeCallbackResult>(std::uint32_t vmIndex, std::uint32_t code, bool hasParameters)> code;
    std::function<void(std::uintptr_t vm)> beforeVmDestroy;
    std::function<void(std::string_view error)> failure;
    // 可选实验桥；仅传入自有前缀的有界字节，返回带 VM 引用的 string ID。
    std::function<std::uint32_t(const GameNativeContext&, std::string_view)> localizedString;
};

// 仅由新建游戏的 bootstrap 在 Game.dll LoadLibrary 返回后、游戏入口执行前安装一次。
// 观察器与 hook 随该测试进程存活；不在 DllMain 拆除 hook 或析构 Lua。
// 安装只观察实际注册与改派 Cheat；显式提供 localizedString 时也改派
// GetLocalizedString 的自有前缀。安装时绝不执行地图代码。
[[nodiscard]] bool InstallGameNativeObserver(std::shared_ptr<GameImage127> image,
    GameNativeHandlers handlers, std::string& error);
} // namespace Audastra::Runtime32::OwnLua

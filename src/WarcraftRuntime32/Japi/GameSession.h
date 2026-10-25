#pragma once
#include "NativeSession.h"
#include "../OwnLua/GameNativeObserver.h"

namespace Audastra::Runtime32::Japi {
// 内置模块装入器：拿到已核验的游戏映像，在新 Host 上装入运行时自带的模块。
using BuiltinModules = std::function<std::int32_t(const OwnLua::GameImage127&, audastra::japi::Host&)>;

// 已核验的 native 入口调用；既可来自启动前登记观察，也可来自独立 DLL 的晚附着。
// 必须在真实 JASS 字符串 native 所在线程调用，不能在 DLL attach/AI 初始化线程调用。
AttachResult AttachGameStringVm(std::shared_ptr<OwnLua::GameImage127> image,
    std::uint64_t generation, BuiltinModules builtins = {});

std::uint32_t DispatchGameString(NativeSession& session, const OwnLua::GameNativeContext& context,
    std::string_view input, const BuiltinModules& builtins = {});
}

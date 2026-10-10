#pragma once

#include "RuntimeProtocol.h"

namespace Audastra::Runtime32 {
// 仅显式探针启动安装；普通 F9 不安装写保护或 Lua 观察点。
bool InstallLuaProbeWriteGuard(SharedEvidence* shared);
bool InstallLuaProbeObserver(HMODULE luaModule);
} // namespace Audastra::Runtime32

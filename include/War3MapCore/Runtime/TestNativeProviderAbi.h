#pragma once

#include <windows.h>
#include <cstdint>

namespace Audastra::Runtime32 {
// x86 测试提供者 ABI v1，登记入口仅在游戏登记窗口同步调用。
// 回调只在本次入口有效，不能保存或转发给其他线程。
using AddTestNative = BOOL(WINAPI*)(void* context, std::uintptr_t function, const char* name, const char* signature);
using RegisterTestNatives = BOOL(WINAPI*)(AddTestNative add, void* context);
}

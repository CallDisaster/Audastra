#pragma once

#include <cstdint>
#include <span>

namespace Audastra::Runtime32::OwnLua {

struct NativeCallX86Result {
    bool succeeded = false;
    std::uint32_t value = 0;
    std::uint32_t exception = 0;
};

// 仅接收已从本次可信注册表冻结的函数地址。1.27a native 使用 cdecl 栈字参数，
// real 参数在上层转换成稳定 float32 存储的地址；包括 real 在内的返回值取 EAX。
// 本层不把字符串指针、C函数地址或数值猜成 JASS string/code/handle。
[[nodiscard]] NativeCallX86Result InvokeNativeX86(std::uintptr_t function,
    std::span<const std::uint32_t> arguments) noexcept;

} // namespace Audastra::Runtime32::OwnLua

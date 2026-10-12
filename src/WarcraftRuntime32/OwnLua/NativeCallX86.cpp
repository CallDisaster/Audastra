#include "NativeCallX86.h"
#include <windows.h>

#if !defined(_M_IX86)
#error NativeCallX86 requires the standalone Win32 runtime target.
#endif

namespace Audastra::Runtime32::OwnLua {
namespace {
// 原创调用适配器。保留被调用者保存寄存器和原栈；参数区按16字节对齐，
// 实际有效栈字从参数0开始紧凑排列。函数返回后调用者恢复整个参数区。
__declspec(naked) std::uint32_t __cdecl InvokeWords(std::uintptr_t,
    const std::uint32_t*, std::uint32_t) {
    __asm {
        push ebp
        mov ebp, esp
        push esi
        push edi
        mov ecx, [ebp + 16]
        mov edx, ecx
        shl edx, 2
        sub esp, edx
        and esp, -16
        mov edi, esp
        mov esi, [ebp + 12]
        rep movsd
        call dword ptr [ebp + 8]
        lea esp, [ebp - 8]
        pop edi
        pop esi
        pop ebp
        ret
    }
}
}

NativeCallX86Result InvokeNativeX86(std::uintptr_t function, std::span<const std::uint32_t> arguments) noexcept {
    NativeCallX86Result result;
    if (!function || arguments.size() > 32) return result;
    __try {
        result.value = InvokeWords(function, arguments.data(), static_cast<std::uint32_t>(arguments.size()));
        result.succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.exception = GetExceptionCode();
    }
    return result;
}

} // namespace Audastra::Runtime32::OwnLua

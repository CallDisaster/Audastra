#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>

// 仅测试宿主创建的有界只读证据；不包含请求槽、地址或执行入口。
namespace Audastra::Runtime32 {
inline constexpr wchar_t kRuntimeTestEnvironment[] = L"AUDASTRA_RUNTIME_TEST_NONCE";
inline constexpr wchar_t kRuntimeTestMappingPrefix[] = L"Local\\Audastra.RuntimeTest.1.";
struct RuntimeTestEvidence {
    std::uint32_t magic = 0x41525445, version = 1, bytes = sizeof(RuntimeTestEvidence);
    char nonce[33]{};
    char mapSha256[65]{};
    volatile long writer = 0, revision = 0;
    std::uint32_t pid = 0, createdLow = 0, createdHigh = 0, epoch = 0;
    std::uint64_t loadedMs = 0, installedMs = 0, helloMs = 0, reportMs = 0;
    std::int32_t installStatus = 8, state = 0, status = 8;
    char runtimeSha256[65]{}, expectedSha256[65]{};
};
static_assert(sizeof(RuntimeTestEvidence) == 312 && offsetof(RuntimeTestEvidence, loadedMs) == 136,
    "x86 runtime and x64 host must share the evidence layout");
inline bool RuntimeTestHex(std::string_view value, std::size_t bytes) {
    if (value.size() != bytes) return false;
    for (const auto c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool RuntimeTestHeader(const RuntimeTestEvidence& value) {
    return value.magic == 0x41525445 && value.version == 1 && value.bytes == sizeof(value)
        && value.nonce[32] == 0 && value.mapSha256[64] == 0
        && RuntimeTestHex({value.nonce, 32}, 32) && RuntimeTestHex({value.mapSha256, 64}, 64);
}
}

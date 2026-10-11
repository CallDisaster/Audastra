#pragma once

#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {

struct LuaHostLimits {
    std::size_t memoryBytes = 64 * 1024 * 1024;
    std::size_t sourceBytes = 4 * 1024 * 1024;
    std::size_t totalModuleBytes = 32 * 1024 * 1024;
    std::size_t moduleCount = 512;
    std::size_t requireDepth = 64;
    std::uint64_t instructions = 2'000'000;
    std::uint32_t milliseconds = 250;
};
struct LuaModuleRead {
    bool found = false;
    std::string source;
    std::string error;
};
// 生产提供者只读取冻结地图归档。宿主给出规范化成员名和最大字节数，不提供文件系统搜索器。
using LuaModuleReader = std::function<LuaModuleRead(std::string_view member, std::size_t maxBytes)>;
struct LuaHostResult {
    LuaReloadResult code = LuaReloadResult::HostFailure;
    std::string message;
    [[nodiscard]] bool Succeeded() const noexcept { return code == LuaReloadResult::Success; }
};

class LuaHost final {
public:
    explicit LuaHost(LuaModuleReader reader, LuaHostLimits limits = {});
    ~LuaHost();
    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;
    [[nodiscard]] LuaHostResult Initialize();
    // 调用线程绑定到 Initialize；不创建后台执行线程，不执行二进制 chunk。
    [[nodiscard]] LuaHostResult Execute(std::string_view source, std::string_view chunkName);
    [[nodiscard]] std::size_t MemoryBytes() const noexcept;
    [[nodiscard]] std::string_view ConsoleText() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Audastra::Runtime32::OwnLua

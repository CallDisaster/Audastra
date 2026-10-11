#pragma once

#include "LuaHost.h"
#include <filesystem>

namespace Audastra::Runtime32::OwnLua {

struct LuaArchiveOpen;

// 复用 Core 的 MPQ 解码器，持有同一冻结文件对象；不调用游戏全局文件搜索。
class LuaMapArchive final {
public:
    ~LuaMapArchive();
    LuaMapArchive(const LuaMapArchive&) = delete;
    LuaMapArchive& operator=(const LuaMapArchive&) = delete;
    [[nodiscard]] static LuaArchiveOpen Open(const std::filesystem::path& map,
        std::string_view expectedSha256);
    [[nodiscard]] LuaModuleRead Read(std::string_view member, std::size_t maxBytes) const;
private:
    struct Impl;
    explicit LuaMapArchive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct LuaArchiveOpen {
    std::shared_ptr<LuaMapArchive> archive;
    std::string error;
};

} // namespace Audastra::Runtime32::OwnLua

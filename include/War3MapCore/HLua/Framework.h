#pragma once

#include "War3MapCore/Services/LuaFolder.h"

namespace War3Map::HLua {

struct FrameworkLocation {
    std::filesystem::path repository;
    std::filesystem::path runtime;
    std::filesystem::path build;
};

// 正文、身份及读取租约来自同一次冻结；执行器不再按路径重新加载代码。
struct FrameworkSnapshot {
    Services::LuaFolderSnapshot runtime;
    Services::LuaFolderSnapshot build;
    std::string sha256;
};

class HLuaFrameworkLocator final {
public:
    [[nodiscard]] static Result<FrameworkSnapshot> Capture(const FrameworkLocation& location);
    [[nodiscard]] static Status Verify(const FrameworkSnapshot& snapshot);
};

[[nodiscard]] const Services::LuaFolderFile* FindLuaFile(
    const Services::LuaFolderSnapshot& snapshot, std::string_view relativePath);
[[nodiscard]] std::string_view LuaText(const Services::LuaFolderFile& file);

} // namespace War3Map::HLua

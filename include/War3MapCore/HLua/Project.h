#pragma once

#include "War3MapCore/HLua/Framework.h"

namespace War3Map::HLua {

struct ProjectSnapshot {
    Services::LuaFolderSnapshot source;
    std::vector<std::size_t> hslkFiles;
    std::vector<std::size_t> runtimeFiles;
    std::vector<std::string> jassModules;
};

class HLuaProjectScanner final {
public:
    [[nodiscard]] static Result<ProjectSnapshot> Capture(const std::filesystem::path& root);
};

// 只改真实调用语句；注释、字符串和 rawcode 中的相同文字不作为插入点。
[[nodiscard]] Result<std::string> InsertLuaEntry(std::string_view jass,
    std::string_view module = "h-lua-cli");

} // namespace War3Map::HLua

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

struct ReloadPlan {
    std::string hslkSha256;
    bool rebuildRequired = false;
    std::vector<Services::LuaFolderFile> authorModules;
};
// 只选择工程扫描器确认的作者运行期模块；物编改变时不发送任何模块。
[[nodiscard]] Result<ReloadPlan> PrepareReload(const ProjectSnapshot& project, std::string_view builtHslkSha256);

// 只改真实调用语句；注释、字符串和 rawcode 中的相同文字不作为插入点。
[[nodiscard]] Result<std::string> InsertLuaEntry(std::string_view jass,
    std::string_view module = "h-lua-cli");

} // namespace War3Map::HLua

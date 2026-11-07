#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/IO/FileBytes.h"

namespace War3Map::HLua {
Result<Services::MapCreateReceipt> CreateProject(const NewProjectRequest& request) {
    using Output = Result<Services::MapCreateReceipt>;
    if (request.directory.empty() || request.toolRoot.empty() || request.gameRoot.empty()
        || std::filesystem::exists(request.directory))
        return Output::Failure("HLua", "HLUA_NEW_PATH", "新建工程需要不存在的目录、工具数据根和游戏数据根。");
    auto framework = HLuaFrameworkLocator::Capture(request.framework);
    if (!framework.Succeeded()) return {std::nullopt, std::move(framework.diagnostics)};
    if (!std::filesystem::create_directory(request.directory))
        return Output::Failure("HLua", "HLUA_NEW_PATH", "无法创建独占工程目录。");
    Services::MapCreateRequest create;
    create.target = request.directory / "base.w3x"; create.workspace = request.directory / "map-create";
    create.gameRoot = request.gameRoot; create.toolRoot = request.toolRoot; create.uiRoot = request.uiRoot;
    create.options.name = IO::PathUtf8(request.directory.filename());
    auto created = Services::CreateMapCandidate(create);
    if (!created.Succeeded()) return created;
    const std::string source = "require('h-lua')\nfunction main()\n    print('Audastra project main')\nend\n";
    auto written = IO::WriteFileBytes(request.directory / "main.lua", {reinterpret_cast<const std::byte*>(source.data()), source.size()});
    if (!written.Succeeded()) return {std::nullopt, std::move(written.diagnostics)};
    return created;
}
} // namespace War3Map::HLua

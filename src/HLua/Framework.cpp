#include "War3MapCore/HLua/Framework.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
#include <array>

namespace War3Map::HLua {
namespace {
std::string Key(std::string value) {
    for (auto& c : value) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return value;
}
Status Required(const Services::LuaFolderSnapshot& source, std::span<const std::string_view> paths) {
    Status result;
    for (const auto path : paths) if (!FindLuaFile(source, path))
        result.diagnostics.push_back({DiagnosticSeverity::Error, "HLua", "HLUA_FRAMEWORK_FILE_MISSING",
            "用户框架缺少必需文件：" + std::string(path), {}});
    return result;
}
}

const Services::LuaFolderFile* FindLuaFile(const Services::LuaFolderSnapshot& snapshot, std::string_view path) {
    const auto key = Key(std::string(path));
    const auto found = std::find_if(snapshot.files.begin(), snapshot.files.end(),
        [&](const auto& file) { return Key(file.path) == key; });
    return found == snapshot.files.end() ? nullptr : &*found;
}
std::string_view LuaText(const Services::LuaFolderFile& file) {
    return {reinterpret_cast<const char*>(file.bytes->data()), file.bytes->size()};
}

Result<FrameworkSnapshot> HLuaFrameworkLocator::Capture(const FrameworkLocation& location) {
    if ((!location.repository.empty() && (!location.runtime.empty() || !location.build.empty()))
        || (location.repository.empty() && (location.runtime.empty() || location.build.empty())))
        return Result<FrameworkSnapshot>::Failure("HLua", "HLUA_FRAMEWORK_ARGUMENT",
            "请指定框架仓库根目录，或同时指定运行期目录和构建期目录。");
    const auto runtime = location.repository.empty() ? location.runtime : location.repository / "depend/h-lua";
    const auto build = location.repository.empty() ? location.build : location.repository / "depend/sdk/lib/embeds/slk";
    auto r = Services::CaptureLuaFolder(runtime, true, true);
    if (!r.Succeeded()) return {std::nullopt, std::move(r.diagnostics)};
    auto b = Services::CaptureLuaFolder(build, true, true);
    if (!b.Succeeded()) return {std::nullopt, std::move(b.diagnostics)};
    constexpr std::string_view requiredRuntime[]{"h-lua.lua", "builtIn/engine.lua", "builtIn/initialization.lua",
        "builtIn/blizzard.lua", "builtIn/echo.lua", "builtIn/id.lua", "builtIn/console/go.lua",
        "builtIn/console/test.lua", "builtIn/console/build.lua", "builtIn/console/dist.lua",
        "slk/pilot.lua", "slk/setter.lua", "slk/slk.lua", "slk/system.lua",
        "const/abilityTarget.lua", "const/attribute.lua", "const/cache.lua", "const/damageSource.lua",
        "const/event.lua", "const/hero.lua", "const/hotKey.lua", "const/item.lua", "const/monitor.lua",
        "const/target.lua", "const/unit.lua", "const/orderStr.lua", "foundation/color.lua",
        "foundation/array.lua", "foundation/math.lua", "foundation/string.lua", "foundation/mbstring.lua",
        "foundation/table.lua", "foundation/description.lua"};
    constexpr std::string_view requiredBuild[]{"json.lua", "string.lua", "table.lua", "go.lua",
        "setter.lua", "slk.lua", "system.lua"};
    auto status = Required(*r.value, requiredRuntime);
    const auto other = Required(*b.value, requiredBuild);
    status.diagnostics.insert(status.diagnostics.end(), other.diagnostics.begin(), other.diagnostics.end());
    if (!status.Succeeded()) return {std::nullopt, std::move(status.diagnostics)};
    if (r.value->missing || b.value->missing || r.value->skippedLinks || b.value->skippedLinks)
        return Result<FrameworkSnapshot>::Failure("HLua", "HLUA_FRAMEWORK_UNSAFE",
            "框架目录不存在或包含链接；构建输入不能跳过未审阅的来源。");
    auto identity = Sha256Text("hlua-framework-v1\n" + r.value->sha256 + "\n" + b.value->sha256);
    if (!identity.Succeeded()) return {std::nullopt, std::move(identity.diagnostics)};
    return Result<FrameworkSnapshot>::Success({std::move(*r.value), std::move(*b.value), *identity.value});
}
Status HLuaFrameworkLocator::Verify(const FrameworkSnapshot& snapshot) {
    auto status = Services::VerifyLuaFolder(snapshot.runtime);
    if (!status.Succeeded()) return status;
    return Services::VerifyLuaFolder(snapshot.build);
}
} // namespace War3Map::HLua

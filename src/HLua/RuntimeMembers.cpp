#include "BuildInternal.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Jass/JassChecker.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>

namespace War3Map::HLua::Detail {
namespace {
std::vector<std::byte> Bytes(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())}; }
std::string Module(std::string path) {
    path.resize(path.size() - 4);
    for (auto& c : path) if (c == '/' || c == '\\') c = '.';
    return path;
}
}
Result<std::vector<MapArchiveMember>> RuntimeMembers(const FrameworkSnapshot& framework,
    const ProjectSnapshot& project, const HslkOutput& hslk, BuildTarget target) {
    using Output = Result<std::vector<MapArchiveMember>>;
    std::vector<MapArchiveMember> output;
    std::map<std::string, std::string> names;
    std::array<std::vector<std::string>, 4> groups;
    const auto add = [&](std::string path, std::span<const std::byte> bytes) -> Status {
        const auto canonical = Resources::CanonicalImportPath(path);
        if (!canonical.Succeeded()) return {canonical.diagnostics};
        if (!names.emplace(*canonical.value, path).second)
            return Status::Failure("HLua", "HLUA_MODULE_COLLISION", "运行期成员重名：" + path);
        output.push_back({std::move(path), {bytes.begin(), bytes.end()}});
        return Status::Ok();
    };
    for (const auto& file : framework.runtime.files) {
        auto path = file.path; for (auto& c : path) if (c == '\\') c = '/';
        if (path.starts_with("const/")) groups[0].push_back(Module(path));
        else if (path.starts_with("foundation/")) groups[1].push_back(Module(path));
        if (path.starts_with("slk/")) {
            path = "h-lua-slk/" + path.substr(4); groups[2].push_back(Module(path));
        }
        const auto status = add(path, path == "h-lua-slk/slk.lua" ? std::span<const std::byte>(Bytes(hslk.injectedSlk)) : std::span<const std::byte>(*file.bytes));
        if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    }
    for (const auto index : project.hslkFiles) {
        const auto& file = project.source.files.at(index);
        auto path = file.path; for (auto& c : path) if (c == '\\') c = '/';
        path = "project-slk/" + path.substr(5); groups[3].push_back(Module(path));
        const auto status = add(path, *file.bytes);
        if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    }
    for (const auto index : project.runtimeFiles) {
        const auto& file = project.source.files.at(index);
        const auto status = add(file.path, *file.bytes);
        if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    }
    const char* console = target == BuildTarget::Test ? "test" : target == BuildTarget::Build ? "build" : "dist";
    const auto* selected = FindLuaFile(framework.runtime, std::string("builtIn/console/") + console + ".lua");
    if (!selected) return Output::Failure("HLua", "HLUA_FRAMEWORK_FILE_MISSING", "缺少所选目标的 console 文件。");
    auto status = add("h-lua-console.lua", *selected->bytes);
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    std::string entry = "require('h-lua-console')\n";
    for (auto& group : groups) {
        std::sort(group.begin(), group.end());
        for (const auto& module : group) entry += "require(" + nlohmann::json(module).dump() + ")\n";
    }
    entry += "require('main')\n"
        "assert(type(main)=='function', 'project main() missing')\n"
        "if cj then\n"
        "  local startup = cj.CreateTimer()\n"
        "  cj.TimerStart(startup, 0.1, false, function()\n"
        "    cj.DestroyTimer(startup)\n"
        "    main()\n"
        "  end)\n"
        "else\n  main()\nend\n";
    status = add("h-lua-cli.lua", Bytes(entry));
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Output::Success(std::move(output));
}
Status CheckScript(const MapArchiveSession& archive, std::string_view script, const Game::GameDataSource& game) {
    std::array<std::string, 2> declarations;
    const std::array<std::string, 2> paths{"Scripts/common.j", "Scripts/Blizzard.j"};
    std::vector<Jass::JassSource> sources;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto read = game.ReadFile(paths[i]);
        if (!read.Succeeded()) return {std::move(read.diagnostics)};
        declarations[i].assign(reinterpret_cast<const char*>(read.value->data()), read.value->size());
        sources.push_back({paths[i], declarations[i]});
    }
    sources.push_back({"war3map.j", script});
    const auto checked = Jass::CheckJass(sources);
    if (!checked.Succeeded()) return {checked.diagnostics};
    return archive.VerifySourceUnchanged();
}
} // namespace War3Map::HLua::Detail

#include "War3MapCore/HLua/Project.h"
#include "War3MapCore/Hash.h"
#include <set>

namespace War3Map::HLua {
Result<ReloadPlan> PrepareReload(const ProjectSnapshot& project, std::string_view built) {
    ReloadPlan result;
    std::string identity = "hlua-hslk-v1\n";
    std::set<std::size_t> indexes;
    for (const auto index : project.hslkFiles) {
        if (index >= project.source.files.size() || !indexes.insert(index).second)
            return Result<ReloadPlan>::Failure("HLua", "HLUA_RELOAD_SOURCE", "物编文件清单不完整或重复。");
        const auto& file = project.source.files[index];
        if (!file.bytes) return Result<ReloadPlan>::Failure("HLua", "HLUA_RELOAD_SOURCE", "物编文件正文未冻结。");
        identity += file.path + "\n" + file.sha256 + "\n";
    }
    auto hash = Sha256Text(identity);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    result.hslkSha256 = *hash.value;
    result.rebuildRequired = built.empty() || built != result.hslkSha256;
    if (!result.rebuildRequired) {
        for (const auto index : project.runtimeFiles) {
            if (index >= project.source.files.size() || !indexes.insert(index).second || !project.source.files[index].bytes)
                return Result<ReloadPlan>::Failure("HLua", "HLUA_RELOAD_SOURCE", "作者模块清单不完整或与物编重叠。");
            result.authorModules.push_back(project.source.files[index]);
        }
    }
    return Result<ReloadPlan>::Success(std::move(result));
}
} // namespace War3Map::HLua

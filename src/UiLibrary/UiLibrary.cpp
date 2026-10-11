#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <unordered_map>

namespace War3Map::UiLibrary {
namespace {

using Trigger::UiDefinitionKind;





// 包的判定只看文件是否存在，不解析内容；解析由 UiCatalog 负责。
bool IsPackageDirectory(const std::filesystem::path& directory) {
    static constexpr std::string_view kNames[] = {
        "define.txt", "event.txt", "condition.txt", "action.txt", "call.txt"};
    std::error_code error;
    for (const std::string_view name : kNames) {
        if (std::filesystem::is_regular_file(directory / name, error)) return true;
    }
    return std::filesystem::is_regular_file(directory / "ui" / "TriggerData.txt", error);
}

bool IsLibraryFile(const std::filesystem::path& path) {
    const std::string extension = Text::LowerCType(IO::GenericPathUtf8(path.extension()));
    return extension == ".txt" || extension == ".j" || extension == ".cfg";
}

// 聚合指纹：按小写相对路径排序后拼接“路径\0内容哈希\n”，再取 SHA-256。
// 只读取文本类库文件；包内程序、脚本与动态库不参与，也不会被打开。
Result<PackageInfo> Fingerprint(const std::filesystem::path& uiRoot, std::string id, bool listed, std::size_t order) {
    PackageInfo info{std::move(id), listed, order};
    const std::filesystem::path directory = uiRoot / IO::PathFromUtf8(info.id);
    std::map<std::string, std::string> files;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error) || !IsLibraryFile(it->path())) continue;
        auto hash = Sha256File(it->path());
        if (!hash.Succeeded()) {
            Result<PackageInfo> failure;
            failure.diagnostics = std::move(hash.diagnostics);
            return failure;
        }
        files.emplace(Text::LowerCType(IO::GenericPathUtf8(it->path().lexically_relative(directory))), *hash.value);
        info.byteCount += it->file_size(error);
    }
    if (error) {
        return Result<PackageInfo>::Failure("UiLibrary", "UI_PACKAGE_UNREADABLE", "无法遍历函数库包：" + info.id);
    }
    std::string digest;
    for (const auto& [path, hash] : files) {
        digest += path;
        digest.push_back('\0');
        digest += hash;
        digest.push_back('\n');
    }
    auto aggregate = Sha256Text(digest);
    if (!aggregate.Succeeded()) {
        Result<PackageInfo> failure;
        failure.diagnostics = std::move(aggregate.diagnostics);
        return failure;
    }
    info.fingerprint = *aggregate.value;
    info.fileCount = files.size();
    return Result<PackageInfo>::Success(std::move(info));
}

const PackageInfo* FindPackage(std::span<const PackageInfo> packages, std::string_view id) {
    const std::string key = Text::LowerCType(std::string(id));
    const auto found = std::find_if(packages.begin(), packages.end(),
        [&key](const PackageInfo& package) { return Text::LowerCType(package.id) == key; });
    return found == packages.end() ? nullptr : &*found;
}

} // namespace

Result<std::vector<PackageInfo>> DiscoverPackages(const std::filesystem::path& toolRoot) {
    Result<std::vector<PackageInfo>> result;
    std::vector<PackageInfo> packages;
    const std::filesystem::path uiRoot = Trigger::UiCatalog::ResolveUiRoot(toolRoot);
    std::error_code error;
    if (!std::filesystem::is_directory(uiRoot, error)) {
        return Result<std::vector<PackageInfo>>::Failure("UiLibrary", "UI_ROOT_MISSING", "UI 数据根不存在：" + IO::GenericPathUtf8(toolRoot));
    }

    std::set<std::string> seen;
    auto listed = Trigger::UiCatalog::ReadConfigLayers(toolRoot);
    result.diagnostics.insert(result.diagnostics.end(), listed.diagnostics.begin(), listed.diagnostics.end());
    if (listed.value) {
        for (std::size_t order = 0; order < listed.value->size(); ++order) {
            const std::string& id = (*listed.value)[order];
            if (!seen.insert(Text::LowerCType(id)).second) continue;
            if (!std::filesystem::is_directory(uiRoot / IO::PathFromUtf8(id), error)) {
                result.diagnostics.push_back({DiagnosticSeverity::Warning, "UiLibrary", "UI_PACKAGE_LISTED_MISSING",
                    "config 列出的包在数据根中不存在：" + id, std::nullopt});
                continue;
            }
            auto package = Fingerprint(uiRoot, id, true, order);
            result.diagnostics.insert(result.diagnostics.end(), package.diagnostics.begin(), package.diagnostics.end());
            if (package.value) packages.push_back(std::move(*package.value));
        }
    }

    // 未列出的包：最多向下两级（覆盖 "作者/包" 形式），按路径排序以保证结果稳定。
    std::vector<std::string> unlisted;
    for (auto it = std::filesystem::recursive_directory_iterator(uiRoot, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_directory(error)) continue;
        if (it.depth() >= 1) it.disable_recursion_pending();
        const std::string id = IO::GenericPathUtf8(it->path().lexically_relative(uiRoot));
        if (!seen.contains(Text::LowerCType(id)) && IsPackageDirectory(it->path())) unlisted.push_back(id);
    }
    std::sort(unlisted.begin(), unlisted.end());
    for (const std::string& id : unlisted) {
        auto package = Fingerprint(uiRoot, id, false, 0);
        result.diagnostics.insert(result.diagnostics.end(), package.diagnostics.begin(), package.diagnostics.end());
        if (package.value) packages.push_back(std::move(*package.value));
    }
    result.value.emplace(std::move(packages));
    return result;
}

GlobalSettings ReconcileGlobal(const GlobalSettings& saved, std::span<const PackageInfo> discovered) {
    GlobalSettings merged;
    std::set<std::string> kept;
    for (const GlobalEntry& entry : saved.packages) {
        const PackageInfo* package = FindPackage(discovered, entry.id);
        if (package == nullptr || !kept.insert(Text::LowerCType(package->id)).second) continue;
        merged.packages.push_back({package->id, entry.enabled, entry.fingerprint});
    }
    for (const PackageInfo& package : discovered) {
        if (!kept.insert(Text::LowerCType(package.id)).second) continue;
        merged.packages.push_back({package.id, package.listedInConfig, package.fingerprint});
    }
    return merged;
}

MapUsage ComputeMapUsage(const Trigger::TriggerDocument& document, std::span<const PackageInfo> discovered) {
    std::map<std::string, std::set<std::string>> functionsByLayer;
    std::vector<std::string> order;
    for (const auto& node : document.ecaNodes) {
        if (node.uiLayer.empty()) continue;
        const std::string key = Text::LowerCType(node.uiLayer);
        if (!functionsByLayer.contains(key)) order.push_back(node.uiLayer);
        functionsByLayer[key].insert(node.name.text);
    }
    MapUsage usage;
    for (const std::string& layer : order) {
        const PackageInfo* package = FindPackage(discovered, layer);
        usage.packages.push_back({package != nullptr ? package->id : layer,
            package != nullptr ? package->fingerprint : std::string{},
            functionsByLayer[Text::LowerCType(layer)].size()});
    }
    // 按本机发现顺序排列（config 顺序在前）；本机不存在的包保持出现顺序排在最后。
    std::stable_sort(usage.packages.begin(), usage.packages.end(),
        [&discovered](const MapUsageEntry& left, const MapUsageEntry& right) {
            const auto rank = [&discovered](const MapUsageEntry& entry) {
                const PackageInfo* package = FindPackage(discovered, entry.id);
                return package == nullptr ? discovered.size() : static_cast<std::size_t>(package - discovered.data());
            };
            return rank(left) < rank(right);
        });
    return usage;
}

std::vector<PackageCheck> CheckMapUsage(
    const MapUsage& usage, const GlobalSettings& global, std::span<const PackageInfo> discovered) {
    std::vector<PackageCheck> checks;
    for (const MapUsageEntry& entry : usage.packages) {
        PackageCheck check{entry.id, PackageStatus::Same, entry.fingerprint, {}};
        const PackageInfo* package = FindPackage(discovered, entry.id);
        if (package == nullptr) {
            check.status = PackageStatus::Missing;
        } else {
            check.localFingerprint = package->fingerprint;
            const auto setting = std::find_if(global.packages.begin(), global.packages.end(),
                [&package](const GlobalEntry& value) { return Text::LowerCType(value.id) == Text::LowerCType(package->id); });
            if (setting == global.packages.end() || !setting->enabled) {
                check.status = PackageStatus::DisabledLocally;
            } else if (!entry.fingerprint.empty() && entry.fingerprint != package->fingerprint) {
                check.status = PackageStatus::Changed;
            }
        }
        checks.push_back(std::move(check));
    }
    return checks;
}

std::vector<std::string> EffectiveLayers(
    const GlobalSettings& global, std::span<const std::string> mapOnly, std::span<const PackageInfo> discovered) {
    std::set<std::string> extra;
    for (const std::string& id : mapOnly) extra.insert(Text::LowerCType(id));
    std::vector<std::string> layers;
    for (const GlobalEntry& entry : global.packages) {
        if ((entry.enabled || extra.contains(Text::LowerCType(entry.id))) && FindPackage(discovered, entry.id) != nullptr) {
            layers.push_back(entry.id);
        }
    }
    return layers;
}

std::vector<DefinitionConflict> FindConflicts(const Trigger::UiCatalog& catalog) {
    std::vector<DefinitionConflict> conflicts;
    for (const UiDefinitionKind kind : {UiDefinitionKind::Define, UiDefinitionKind::Event,
             UiDefinitionKind::Condition, UiDefinitionKind::Action, UiDefinitionKind::Call}) {
        for (const auto* definition : catalog.Definitions(kind)) {
            std::vector<std::string> layers;
            for (const auto& revision : definition->revisions) {
                if (std::find(layers.begin(), layers.end(), revision.layer) == layers.end()) {
                    layers.push_back(revision.layer);
                }
            }
            if (layers.size() > 1) {
                conflicts.push_back({kind, definition->group, definition->name, std::move(layers),
                    definition->Active().layer});
            }
        }
    }
    return conflicts;
}

std::string_view ToString(PackageStatus status) noexcept {
    switch (status) {
    case PackageStatus::Same: return "same";
    case PackageStatus::Changed: return "changed";
    case PackageStatus::Missing: return "missing";
    case PackageStatus::DisabledLocally: return "disabled-locally";
    }
    return "unknown";
}

} // namespace War3Map::UiLibrary

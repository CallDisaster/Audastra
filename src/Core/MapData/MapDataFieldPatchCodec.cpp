#include "War3MapCore/MapData/MapDataFieldPatchCodec.h"
#include "MapDataPatchSource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace War3Map::MapData {
namespace {
bool HasSettingsStringDependency(const MapDocument& original, const MapDocument& current) {
    if (!original.Settings() || !current.Settings() || !original.Strings() || !current.Strings()
        || original.Member(MapDataKind::Strings).state != MapDataState::Ready
        || current.Member(MapDataKind::Strings).state != MapDataState::Ready) return false;
    constexpr std::array<std::string MapSettings::*, 3> fields{
        &MapSettings::author, &MapSettings::description, &MapSettings::recommendedPlayers};
    for (const auto field : fields) {
        const auto& before = original.Settings().value().*field;
        const auto& after = current.Settings().value().*field;
        if (before != after || !after.starts_with("TRIGSTR_")) continue;
        const auto oldText = original.Strings()->Resolve(before), newText = current.Strings()->Resolve(after);
        if (oldText.Succeeded() && oldText.value && newText.Succeeded() && newText.value)
            return true;
    }
    return false;
}
} // namespace

Result<MapDataPatch> MapDataFieldPatchCodec::Seal(const MapDocument& original,
    const MapDocument& current, const WorkspaceSnapshot& source,
    std::span<const MapDataKind> requestedKinds) {
    const auto frozen = FreezeOriginalMapDataFields(original, current);
    if (!frozen.Succeeded()) return {{}, frozen.diagnostics};
    if (requestedKinds.empty() || requestedKinds.size() > 6)
        return Result<MapDataPatch>::Failure("MapData.Fields", "MAP_FIELD_PATCH_SCOPE",
            "字段目录密封要求一到六个明确的既有成员。");
    std::set<MapDataKind> seen;
    for (const auto kind : requestedKinds) {
        if ((kind != MapDataKind::Settings && kind != MapDataKind::Strings
                && kind != MapDataKind::Units && kind != MapDataKind::Regions && kind != MapDataKind::Cameras && kind != MapDataKind::Sounds)
            || !seen.insert(kind).second)
            return Result<MapDataPatch>::Failure("MapData.Fields", "MAP_FIELD_PATCH_SCOPE",
                "字段目录密封只允许唯一的地图设置、字符串、单位、区域、镜头或声音成员。");
        if (kind == MapDataKind::Sounds && !ValidateSoundFieldSource(current).Succeeded())
            return Result<MapDataPatch>::Failure("MapData.Fields", "MAP_FIELD_PATCH_SCOPE", "声音密封范围必须是具有唯一名字与完整往返资格的已有 classic W3S v1。");
    }
    std::vector<MapDataKind> boundKinds(requestedKinds.begin(), requestedKinds.end());
    const bool stringDependency = seen.contains(MapDataKind::Settings) && !seen.contains(MapDataKind::Strings)
        && HasSettingsStringDependency(original, current);
    if (stringDependency) boundKinds.push_back(MapDataKind::Strings);
    // Settings 是请求身份；其既有引用的正文修改必须绑定原 WTS 的独立 SHA，不能只密封 W3I。
    auto files = detail::BindMapDataPatchSources(original, current, source, boundKinds,
        stringDependency ? "MCP 共用原生字段事务（保留 W3I 引用并绑定共享 WTS 来源）" : "MCP 共用原生字段事务");
    if (!files.Succeeded()) return {{}, files.diagnostics};
    return Result<MapDataPatch>::Success(MapDataPatch(std::move(*files.value)));
}

} // namespace War3Map::MapData

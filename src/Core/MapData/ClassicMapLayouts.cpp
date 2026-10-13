#include "War3MapCore/MapData/ClassicMapLayouts.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"

#include <algorithm>

namespace War3Map::MapData {

Status ValidateClassicMapLayouts(const ClassicMapLayoutInputs& inputs) {
    Status result;
    const auto reject = [&](std::string code, std::string member) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "MapData.Layout", std::move(code),
            std::move(member) + " 包含经典后端未支持或未确定的记录布局；原始数据继续保留，不能省略扩展字段生成脚本或重建成员。", {}});
    };
    if (inputs.settings && !inputs.settings->HasClassicLayout() && !inputs.settings->HasRocLayout()) {
        reject("CLASSIC_SETTINGS_LAYOUT_UNSUPPORTED", "war3map.w3i");
    }
    if (inputs.units) {
        const auto& document = *inputs.units;
        if (!IsClassicPlacementVersion(document.version, document.subversion)
            || document.recordLayout != PlacementRecordLayout::Classic
            || std::any_of(document.units.begin(), document.units.end(),
                [](const UnitPlacement& unit) { return unit.skinId.has_value(); })) {
            reject("CLASSIC_UNITS_LAYOUT_UNSUPPORTED", "war3mapUnits.doo");
        }
    }
    if (inputs.doodads) {
        const auto& document = *inputs.doodads;
        if (!IsClassicPlacementVersion(document.version, document.subversion) || document.specialVersion != 0
            || document.recordLayout != PlacementRecordLayout::Classic
            || std::any_of(document.doodads.begin(), document.doodads.end(),
                [](const DoodadPlacement& doodad) { return doodad.skinId.has_value(); })) {
            reject("CLASSIC_DOODADS_LAYOUT_UNSUPPORTED", "war3map.doo");
        }
    }
    if (inputs.cameras) {
        const auto& document = *inputs.cameras;
        if (document.version != 0 || document.layout != CameraLayout::Classic
            || std::any_of(document.entries.begin(), document.entries.end(),
                [](const CameraRecord& camera) { return camera.localRotation.has_value(); })) {
            reject("CLASSIC_CAMERAS_LAYOUT_UNSUPPORTED", "war3map.w3c");
        }
    }
    if (inputs.sounds) {
        const auto& document = *inputs.sounds;
        if (document.version != 1
            || std::any_of(document.entries.begin(), document.entries.end(),
                [](const SoundRecord& sound) { return sound.extended.has_value(); })) {
            reject("CLASSIC_SOUNDS_LAYOUT_UNSUPPORTED", "war3map.w3s");
        }
    }
    return result;
}

} // namespace War3Map::MapData

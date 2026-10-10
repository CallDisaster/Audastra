#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Services/BuildingSplatService.h"
#include "War3MapCore/Services/PlacementPresentationService.h"

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QStringList>

#include <optional>
#include <variant>
#include <vector>

namespace War3Map { class MapDocument; }
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {

// GUI仅捕获有效字段与实际父链来源；TGA和原创辅助资产由同一资源worker准备。
struct WorldPlacementPresentationCandidate {
    std::uint32_t requestIndex = 0;
    ObjectData::ObjectKey object;
    std::optional<MapData::PlacementId> owner;
    std::shared_ptr<const Services::PlacementPresentationFields> fields;
};

// 未解析行仍保留原摆放身份/坐标；不是模型请求，不能按rawcode猜一个替代实例。
struct WorldUnresolvedPlacement {
    MapData::PlacementId owner;
    std::array<float, 3> position{};
    std::optional<ObjectData::ObjectType> objectType;
    std::string modelPath, reasonCode, reason;
    std::vector<Services::PlacementFieldOrigin> origins;
};

// 地图摆放（war3map.doo / war3mapUnits.doo）到模型请求的只读适配。GUI 线程调用：
// 读取物编父链与原版目录后只输出自有数据，后台线程再按路径读取、解析模型。
struct WorldPlacementCapture {
    std::vector<Render::ModelInstanceRequest> requests;
    std::vector<MapData::PlacementId> ids; // 与 requests 平行：来源摆放的稳定标识
    std::vector<WorldPlacementPresentationCandidate> presentations; // 原requestIndex，不新建点选身份。
    std::size_t doodads = 0, destructibles = 0, units = 0, items = 0;
    std::size_t unresolved = 0;   // 未定义对象、模型字段为空或随机单位/物品
    std::size_t flightHeightPreviewUnits = 0; // 显示近似；原始doo坐标不变。
    QStringList unresolvedCodes;  // 去重后的前若干个 rawcode
    std::vector<WorldUnresolvedPlacement> unresolvedRows; // 完整逐项，不随摘要截断。
};
// document 为当前地图文档（含未保存的摆放编辑）；物编字段取自 project。
// file非空时只捕获该成员，计数也只覆盖该成员。复用另一个成员的宿主仍须通过
// 同来源/物编修订与WorldInstanceRegistry::Prepare的完整成员SHA、谱系检查。
[[nodiscard]] WorldPlacementCapture CaptureWorldPlacements(const QtProject::EditorProject& project, const MapDocument& document,
    std::optional<MapData::PlacementFile> file = std::nullopt);
// 独立于模型可用性的有效建筑贴画输入。继承规则仍由ObjectNames解析；目录字段
// 来源是实际sourceFiles集合，不伪造逐字段唯一文件/偏移。身份绑定owned有效值与来源。
struct WorldBuildingSplatCapture {
    Services::WorldSelectionVersion version;
    std::vector<Services::BuildingSplatUnitInput> units;
    std::string inputIdentity;
    struct FieldDiagnostic {
        std::string rawcode;
        Services::PlacementTextFact uubs;
        std::vector<Services::PlacementFieldOrigin> origins;
        std::size_t placements = 0;
        Services::BuildingSplatConfiguration configuration = Services::BuildingSplatConfiguration::EffectiveField;
        std::string applicabilityReason;
        std::vector<Services::PlacementFieldOrigin> applicabilityOrigins;
    };
    std::vector<FieldDiagnostic> fieldDiagnostics; // 一rawcode一行，原始presence和空值均保留。
};
[[nodiscard]] Result<WorldBuildingSplatCapture> CaptureWorldBuildingSplats(
    const QtProject::EditorProject& project, const MapDocument& document,
    const Services::WorldSelectionVersion& version);
// 有界指定行查询，复用全场景相同字段规则，不扫描其它摆放。retainUnresolved仅为
// 正式创建占位保留真实落点，空modelPath明确表示未知模型/占地，不进入主模型提交。
[[nodiscard]] Result<WorldPlacementCapture> CapturePlacementRows(const QtProject::EditorProject& project,
    const MapDocument& document, std::span<const MapData::PlacementId> rows, bool retainUnresolved = false);
// 与正式摆放读取同一物编父链；仅生成模型请求，不创建地图行。
[[nodiscard]] std::optional<Render::ModelInstanceRequest> CapturePlacementPreview(
    const QtProject::EditorProject& project, ObjectData::ObjectType type,
    const MapData::PlacementTransform& placement, std::int32_t owner,
    WorldPlacementPresentationCandidate* presentation = nullptr);
// 已有/新放置/拖动预览共享：rawPosition属于地图事务，返回值只用于渲染。
// 不重复加地形，不把selZ当模型高度；仅显式fly的有效moveHeight作为预览偏移。
[[nodiscard]] std::array<float, 3> WorldPlacementDisplayPosition(
    const Render::ModelInstanceRequest& request, const std::array<float, 3>& rawPosition);

// 新摆放的默认字段，与原版编辑器写出的一致（实测经典地图）：flags 2、朝向 270°、缩放 1、
// 生命/魔法/掉落表 -1、金矿储量 12500、警戒范围默认、英雄等级 1、非随机（级别 1）；
// 物品固定属于中立被动（玩家 16），装饰物生命 100%。坐标由调用方给出（已贴地）。
[[nodiscard]] std::variant<MapData::UnitPlacement, MapData::DoodadPlacement> MakeNewPlacement(
    ObjectData::ObjectType type, const std::string& rawcode, std::int32_t owner, const std::array<float, 3>& position);

} // namespace War3Map::Modules

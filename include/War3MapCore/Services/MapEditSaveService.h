#pragma once

#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"
#include "War3MapCore/Trigger/TriggerEditBatch.h"
#include "War3MapCore/Trigger/TriggerTransactionSnapshot.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace War3Map::Services {

class ObjectTransactionSnapshot;
class AuxiliaryTransactionSnapshot;

enum class MapEditPointSpace {
    World,
    TerrainVertex,
};

// 同一保存事务内按顺序施加。TerrainVertex 使用源 W3E 顶点索引；World 使用
// Warcraft 世界坐标。普通笔刷与浅/深水最终都通过同一Core准备/提交事务。
struct MapEditTerrainStroke {
    MapData::TerrainBrush brush;
    std::vector<MapData::TerrainStrokePoint> points;
    MapEditPointSpace pointSpace = MapEditPointSpace::World;
    bool followDoodads = false;
    // 有值时brush必须逐字段等于TerrainBrush{}；不接受两种笔刷同时指定。
    // 不接受客户端anchor，服务从当前有序地形的第一点捕获并固定整笔。
    std::optional<MapData::TerrainWaterStrokeOptions> waterBrush;
    // 独立同尺寸可玩区命令：整数地格半开矩形；无points/普通笔刷/水选项。
    std::optional<MapData::TerrainBoundsEditIntent> bounds;
};

// 地图数据字段修改：类别 / 行 / 字段寻址（字段目录见 MapData/MapFields.h），绑定成员 SHA、
// 行身份与精确前值。同一类别的所有修改必须绑定同一成员 SHA；调用方不能注入任意成员字节。
struct MapEditFieldOperation {
    std::string expectedMemberSha256;
    MapData::MapFieldEdit edit;
};

// 行身份和成员 SHA 必须来自同一份查询快照；保存服务在修改前重新核对。
struct MapEditPlacementSource {
    MapData::PlacementFile file = MapData::PlacementFile::Units;
    std::uint32_t row = 0;
    std::string expectedRowKey;
    std::string expectedMemberSha256;
};

struct MapEditPlacementCopy {
    std::vector<MapEditPlacementSource> sources;
    std::array<float, 3> offset{};
};

struct MapEditPlacementRotation {
    MapEditPlacementSource source;
    float facingRadians = 0;
};

struct MapEditPlacementCreate {
    std::string expectedMemberSha256;
    MapData::PlacementCreationRecords records;
};
struct MapEditPlacementDelete {
    std::vector<MapEditPlacementSource> sources;
};
using MapEditPlacementOperation = std::variant<std::monostate, MapEditPlacementCopy, MapEditPlacementRotation,
    MapEditPlacementCreate, MapEditPlacementDelete>;

struct MapEditSaveRequest {
    std::filesystem::path sourceMap;
    std::filesystem::path targetCandidate;
    std::filesystem::path toolRoot;
    std::filesystem::path uiRoot; // 可由宿主明确分离；为空时兼容既有 toolRoot 同时提供 UI 的调用
    std::filesystem::path gameRoot;
    std::vector<Script::ScriptProfileSourceEncoding> sourceEncodings;
    std::vector<Script::ScriptProfileIncludeAccess> includeAccess;
    std::optional<LuaMapTargetRequest> luaTarget;
    std::string expectedSourceSha256;
    std::string expectedTerrainSha256;
    // 显式层序供 Agent/工程宿主传入；省略只允许证明 config 默认层序完全一致。
    std::optional<std::vector<std::string>> effectiveLayers;
    std::vector<MapEditTerrainStroke> terrainStrokes;
    // 普通保存根据本次实际地图变化自动重建小地图；不接客户端图像或SHA。
    TerrainMinimapSaveChoice minimapChoice = TerrainMinimapSaveChoice::Automatic;
    // 原版独立“计算阴影”操作；普通保存沿用已有SHD。
    bool rebuildTerrainShadow = false;
    std::vector<MapEditFieldOperation> fieldEdits;
    MapEditPlacementOperation placementEdit;
    // 一个领域批次与地图 SHA 同时绑定，不能提供任意 WTG/WCT/脚本成员字节。
    std::optional<Trigger::TriggerEditBatch> triggerEdits;
    // 公共编辑事务冻结的完整 GUI 图；与 triggerEdits 互斥，仍走同一脚本/归档保存链。
    std::shared_ptr<const Trigger::TriggerTransactionSnapshot> triggerTransaction;
    // 七类物编的类型化冻结候选；拥有来源和最终成员，不能从协议传入任意替换字节。
    std::vector<std::shared_ptr<const ObjectTransactionSnapshot>> objectTransactions;
    // 共用区域/镜头命令序列封印；保存按实际归档来源和当前触发器重新重放，不能提供 bytes。
    std::shared_ptr<const AuxiliaryTransactionSnapshot> auxiliaryTransaction;
    // 宿主从真实预放/分类来源捕获；协议端不能自报名单。保存须与最终地图内容复核。
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjectReferences;
    // 仅可信宿主提供，不接受客户端函数或脚本。保存链在实际发布前再次复核。
    std::function<Status()> beforePublish;
};

struct MapEditMemberReceipt {
    std::string name;
    std::string originalSha256;
    std::string savedSha256;
};

struct MapEditMinimapReceipt {
    std::string encodedBlpSha256, decodedMip0RgbaSha256, manifestSha256;
    std::size_t encodedBlpBytes = 0;
    std::uint32_t width = 0, height = 0, encodedMipCount = 0;
    // 来自同次 helper 的实际 Manifest，不能由请求选项代填。
    Render::TerrainMinimapRangePolicy rangePolicy = Render::TerrainMinimapRangePolicy::FullW3e;
};

struct MapEditSaveReceipt {
    std::filesystem::path candidate;
    std::string sourceSha256;
    std::string candidateSha256;
    std::string originalTerrainSha256;
    std::string savedTerrainSha256;
    std::string savedPathingSha256;
    std::size_t memberCount = 0;
    double scriptSeconds = 0;
    std::vector<std::string> effectiveLayers;
    std::size_t appliedStrokeCount = 0;
    std::size_t appliedFieldEditCount = 0;
    std::size_t appliedPlacementCount = 0;
    std::vector<MapEditMemberReceipt> editedMembers;
    std::optional<Trigger::TriggerEditBatchReceipt> triggerEdits;
    std::optional<Trigger::TriggerTransactionReceipt> triggerTransaction;
    std::size_t appliedObjectTransactionCount = 0;
    std::size_t appliedAuxiliaryCommandCount = 0;
    // 改动不影响游戏脚本，候选沿用了源地图脚本原字节。
    bool scriptPreserved = false;
    // 仅完成同一保存任务的生成和独立回读后出现；不表示原版编辑器或游戏已验证。
    std::optional<MapEditMinimapReceipt> generatedMissingMinimap;
};

// 接受一种有界领域操作，或同源的完整触发器与物编封印事务；旧触发器批次不能混用。
// 地形笔划也可带一种伴随修改：四个地图说明字段，或冻结物编事务。两类输入分别验证，
// 同时绑定源图与W3E SHA，并经最终通行来源检查；不混入摆放、触发器或区域/镜头事务。
// 源归档及目标成员 SHA 绑定到请求；WPM 语义由编辑会话验证；
// 最终成员、脚本和物编均经同一 MapSaveService 审核，候选回读成功才返回回执。
[[nodiscard]] Result<MapEditSaveReceipt> SaveMapEditCandidate(
    const MapEditSaveRequest& request, const MapSaveService::Progress& progress = {});

struct MapTriggerTextQueryRequest {
    std::filesystem::path sourceMap, toolRoot, uiRoot;
    std::string expectedSourceSha256;
    std::optional<std::string> expectedCatalogSha256;
    std::optional<std::vector<std::string>> effectiveLayers;
    Trigger::TriggerTextQuery query;
};
struct MapTriggerTextQueryReceipt {
    std::string sourceSha256;
    Trigger::TriggerTextPage page;
    std::vector<std::string> effectiveLayers;
    // 普通 GUI 槽且根数量在编辑预算内时，提供完整原树绑定；不替代 batch 的 pair SHA。
    std::optional<Trigger::TriggerEditTriggerTarget> guiTarget;
};
// 从只读归档获取有界 WCT 文本页，使用与保存相同的有效 Catalog 来源。
// 不执行编译器、不创建工作目录、不消费旧目录事务中的 overlay。
[[nodiscard]] Result<MapTriggerTextQueryReceipt> QueryMapTriggerText(const MapTriggerTextQueryRequest& request);

} // namespace War3Map::Services

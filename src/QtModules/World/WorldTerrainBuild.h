#pragma once

#include "WorldModule.h"
#include "WorldEnvironment.h"
#include "WorldTerrainAssets.h"
#include "WorldCliffLayer.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Render/SceneEnvironment.h"

#include <QString>
#include <QStringList>

#include <map>
#include <memory>
#include <optional>
#include <span>

// 世界视图的地形构建（后台线程）：完整读取与地形编辑后的快速重建共用一处实现。
namespace War3Map::Modules {

enum class WorldShadowReadState { NotObserved, ReadFailed, Present };
// 复用现公开净变化比较器，包含W3E或WPM变化，不推断源阴影bake身份。
enum class WorldShadowTerrainRelation { Unknown, TerrainAndPathingUnchanged, TerrainOrPathingEdited };

struct WorldShadowContext {
    WorldShadowTerrainRelation terrainRelation = WorldShadowTerrainRelation::Unknown;
    std::uint64_t sessionIdentity = 0, revision = 0, terrainSourceRevision = 0;
    bool pendingSourceChanges = false;
};

struct WorldShadowInput {
    // nullopt为未观测；失败仍带原诊断，成功空正文仍是Present。没有ExactMissing推断。
    std::optional<Result<std::vector<std::byte>>> read;
    WorldShadowContext context;
};

struct WorldShadowObservation {
    WorldShadowReadState readState = WorldShadowReadState::NotObserved;
    std::string sourceArchiveSha;
    std::size_t sourceBytes = 0;
    std::vector<Diagnostic> readDiagnostics;
    WorldShadowContext context;
};

// 只格式化已有事实，不读来源、不重试、不写全局诊断；便于UI重绘复用同一次观测。
[[nodiscard]] QString WorldShadowSummary(const WorldShadowObservation& shadow, const Render::SceneEnvironment* environment);
[[nodiscard]] QString WorldShadowDetails(const WorldShadowObservation& shadow, const Render::SceneEnvironment* environment);

struct WorldTerrainLoaded {
    WorldTerrainMesh mesh;
    std::shared_ptr<const MapData::TerrainGridSnapshot> rawTerrain; // 解析后的原始 W3E，显式水帧重建不二次抬高坡道
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain; // 显示与拾取用（含坡道抬高）
    std::shared_ptr<const Render::TerrainGroundMesh> ground;
    std::shared_ptr<const Render::TerrainCliffs> cliffs; // 只与贴图地面一起使用（地面已隐藏被覆盖的格）
    std::shared_ptr<const Render::SceneEnvironment> environment; // 模型光照、雾、静态阴影
    std::optional<Render::TerrainWaterStyle> selectedWater; // worker 产物；UI 成功提交完成帧后才写入缓存
    std::optional<WorldCliffLayerFrame> cliffLayer; // 请求时为新烘焙的悬崖准备的独立层
    std::vector<Diagnostic> cliffLayerDiagnostics;
    QString materialNote;
    WorldShadowObservation shadowObservation;
};

// 完整读取时建立、编辑重建时复用的资源：已打开的资源读取器（不重新校验整张地图）、地表贴图、
// 水面样式、地形光照、悬崖类型与已读取的悬崖模型文件。同一时刻只能由一个线程使用。
struct WorldTerrainCache {
    std::unique_ptr<WorldResourceReader> reader;
    MapData::TerrainHeader header; // atlases 对应的贴图表
    std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>> atlases;
    std::shared_ptr<const Render::TerrainAtlasSnapshot> blightAtlas;
    std::optional<Render::TerrainWaterStyle> water;
    std::optional<Render::WaterMaterialDefinition> waterDefinition;
    QStringList atlasNotes;
    Render::TerrainLighting lighting;
    std::vector<std::optional<Render::CliffTypeSource>> cliffTypes;
    WorldTerrainNames names; // 完整读取时的贴图表显示名（与 header 同序）
    std::map<std::string, std::optional<std::vector<std::byte>>, std::less<>> modelFiles;
    QStringList notes; // 光照、环境等与地形内容无关的提示
    std::shared_ptr<const Render::SceneEnvironment> environment;
    WorldShadowObservation shadowObservation; // 无raw，不因编辑重建重新读取或重复发布失败诊断。
};

// 完整读取：解析地形并读取全部地表资源。地表资源可用时 *cache 为新建的缓存，否则为空。
// blightPolicy由UI在worker开始前实际查询provider能力后传入；默认Legacy保持旧provider/minimap语义。
[[nodiscard]] Result<WorldTerrainLoaded> LoadWorldTerrain(std::span<const std::byte> bytes,
    const Result<WorldTerrainSources>& sources, const std::optional<WorldEnvironmentInputs>& environment,
    const WorldShadowInput& shadow, std::shared_ptr<WorldTerrainCache>* cache,
    Render::TerrainGroundBlightPolicy blightPolicy = Render::TerrainGroundBlightPolicy::Legacy,
    Render::TerrainGroundRepresentation representation = Render::TerrainGroundRepresentation::Expanded);

// 编辑后重建：复用缓存（贴图表变化时只重读贴图）。keepCliffs 非空时不重算悬崖、沿用其覆盖格
// （拖动中的快速预览）；为空时重新烘焙悬崖。selectedWaterFrame 仅选择 Water.slk 帧，
// 不推导 texRate 时间；返回 selectedWater 待宿主成功提交渲染后再切换缓存帧，
// 调用方须为成功渲染分配新场景修订。
[[nodiscard]] Result<WorldTerrainLoaded> RebuildWorldTerrain(const MapData::TerrainGridSnapshot& raw,
    WorldTerrainCache& cache, std::shared_ptr<const Render::TerrainCliffs> keepCliffs,
    std::optional<std::uint32_t> selectedWaterFrame = std::nullopt,
    Render::TerrainGroundBlightPolicy blightPolicy = Render::TerrainGroundBlightPolicy::Legacy,
    Render::TerrainGroundRepresentation representation = Render::TerrainGroundRepresentation::Expanded,
    const std::optional<WorldShadowContext>& shadowContext = std::nullopt);

} // namespace War3Map::Modules

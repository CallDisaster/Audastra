#pragma once

#include "War3MapCore/Render/DayNightLighting.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/TerrainTextureAtlas.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Resources/ResourceResolver.h"
#include <filesystem>
#include <memory>
#include <limits>
#include <optional>

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {

struct WorldAssetEntry {
    std::string path, archivePath;
    std::filesystem::path diskPath;
};
// GUI 线程只复制路径/身份。后台读取用独立文件租约，不借用 Project/MPQ/QObject。
struct WorldTerrainSources {
    std::filesystem::path archivePath, gameRoot;
    std::string archiveSha;
    std::vector<WorldAssetEntry> entries;
    std::optional<std::uint8_t> mainTileset;
};
[[nodiscard]] bool SameWorldTerrainSources(const WorldTerrainSources& a, const WorldTerrainSources& b);
[[nodiscard]] Result<WorldTerrainSources> CaptureWorldTerrainSources(const QtProject::EditorProject& project,
    std::size_t maxEntries = std::numeric_limits<std::size_t>::max());
// worker 内打开只读解析器（校验地图 SHA；游戏包按需挂载）。不可复制或并发读取。
// 初始场景reader后续仅作来源检查；专用实例reader可在前一资源任务结束后独占移交，
// 继续实际读取，逐项来源及任务前后租约复核不因挂载复用而省略。
class WorldResourceReader {
public:
    struct OpenTimings {
        std::uint64_t totalNs = 0, hashNs = 0, archiveNs = 0, hashedBytes = 0;
        bool verifiedArchiveReused = false;
    };
    // verifiedSource只授予其已SHA验证且仍持有拒写租约的同一归档身份；新reader仍有
    // 独立租约/MPQ/解析器。路径替换、SHA或文件身份不同直接拒绝，不静默信任缓存。
    [[nodiscard]] static Result<std::unique_ptr<WorldResourceReader>> Open(const WorldTerrainSources& sources,
        const WorldResourceReader* verifiedSource = nullptr, OpenTimings* timings = nullptr);
    ~WorldResourceReader();
    WorldResourceReader(const WorldResourceReader&) = delete;
    WorldResourceReader& operator=(const WorldResourceReader&) = delete;
    [[nodiscard]] Resources::ResourceResolver& Resolver() noexcept;
    // 缓存复用前确认来源路径仍指向打开时的同一文件；避免原地保存后的旧句柄被当成新地图资源。
    [[nodiscard]] Status VerifyCurrentSource() const;
    // 只比较捕获的完整查找配置；实际文件租约仍须在每次任务前后Verify。
    [[nodiscard]] bool MatchesSources(const WorldTerrainSources& sources) const;
    // 动画缓存必须记录真实命中的游戏 MPQ 层/地图覆盖，而非笼统的 Game 来源。
    [[nodiscard]] Result<Render::AnimationResource> ReadAnimationResource(std::string_view path, std::uint64_t maxBytes);
private:
    WorldResourceReader();
    struct State;
    std::unique_ptr<State> state_;
};
// water 非空时取 Water.slk 的水面样式；blight 非空时按 UI/WorldEditData.txt 的
// [TileSets] 映射取主 tileset 的荒芜 atlas（可被地图导入覆写）；先核验局部遮罩 alpha。
// 缺失或遮罩无效时输出空值及警告，由网格显式近似。
[[nodiscard]] Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    const WorldTerrainSources& sources, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water = nullptr,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight = nullptr,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition = nullptr);
// 同上，使用调用方已打开的解析器（同一次加载里与光照、阴影共用一个资源租约）。
[[nodiscard]] Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water = nullptr,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight = nullptr,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition = nullptr);
// 悬崖类型：与 header.cliffTilesets 同序，CliffTypes.slk 未定义的留空（由烘焙计数说明）。
[[nodiscard]] Result<std::vector<std::optional<Render::CliffTypeSource>>> LoadWorldCliffTypes(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain);
// 笔刷面板显示名：取 Terrain.slk / CliffTypes.slk 的 name（缺省取 comment）原值，与贴图表同序；
// 未定义的为空。WESTRING 键由界面按当前字符串表解析。
struct WorldTerrainNames {
    std::vector<std::string> ground, cliffs;
};
[[nodiscard]] Result<WorldTerrainNames> LoadWorldTerrainNames(Resources::ResourceResolver& resolver,
    const MapData::TerrainHeader& terrain);

// 原版昼夜光照：按 WorldEditData.txt 选地形/单位 DNC 模型并取 hours 时刻的光。读取失败的一项为空，
// 原因记入 notes（调用方对该项使用默认中性光）。
struct WorldLighting {
    std::optional<Render::SceneLight> terrain, unit;
    std::vector<Diagnostic> notes;
};
[[nodiscard]] WorldLighting LoadWorldLighting(Resources::ResourceResolver& resolver, char mainTileset,
    std::uint8_t lightEnvironment, float hours = Render::kDefaultDayNightHours);

} // namespace War3Map::Modules

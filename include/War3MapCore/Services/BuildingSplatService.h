#pragma once

#include "War3MapCore/Resources/ResourceResolver.h"
#include "War3MapCore/Services/WorldSelection.h"

#include <map>
#include <memory>
#include <span>
#include <array>
#include <optional>

namespace War3Map::Services {

struct BuildingSplatCatalogLimits {
    std::size_t maxTableBytes = 2 * 1024 * 1024, maxRows = 4096;
    std::size_t maxCells = 131072, maxCellBytes = 4096;
};
struct BuildingSplatDefinition {
    std::string key, texturePath;
    std::uint32_t tableRow = 0;
    float scale = 0, birth = 0, pause = 0, decay = 0;
    std::int32_t tableBlendMode = 0; // 只保留表值；不映射为MDX filterMode。
    // SLK生命周期颜色原始无符号整数；实际游戏表含256。不是已验证的8-bit RGBA，
    // 不截断/归一，不运行尚未闭合的生命周期规则。
    std::array<std::uint32_t, 4> start{}, middle{}, end{};
    std::map<std::string, std::string> rawFields; // 包括未支持的Sound/version等，按原列标题小写索引。
};
// 只由解析函数创建shared_ptr<const>；内容及实际来源同时纳入身份。没有外部文件长租约。
struct BuildingSplatCatalog {
    static constexpr std::uint32_t schemaVersion = 2;
    Resources::ResourceLocation source;
    std::string tableSha256, identitySha256;
    std::vector<std::byte> originalBytes;
    std::map<std::string, BuildingSplatDefinition> definitions;
};
// 调用方须先做有界资源读取。损坏/冲突的高层资源不得静默换低层来源。
[[nodiscard]] Result<std::shared_ptr<const BuildingSplatCatalog>> ParseBuildingSplatCatalog(
    Resources::ResolvedResource resource, const BuildingSplatCatalogLimits& limits = {});

struct BuildingSplatFieldOrigin {
    std::string field, objectRawcode, sourceId, sourcePath;
    std::optional<std::uint64_t> byteOffset;
};
// 宿主公共有效字段reader的owned输出；必须来自同一冻结物编/父链快照。
// complete只表示来源解析完成，不表示原版渲染已验。服务不复制父链，也不填默认uubs/tint。
// origins用规范字段ID uubs/uclr/uclg/uclb；标准表映射仍记录实际列/父对象的来源。
enum class BuildingSplatConfiguration : std::uint8_t { EffectiveField, NotApplicableToNonBuilding };
struct BuildingSplatEffectiveFields {
    bool complete = false;
    std::string uubs, reason;
    // 输入保留足够宽度供共用服务拒绝>255；合法后才生成8-bit footprint.color。
    std::array<std::uint32_t, 3> tint{};
    std::vector<BuildingSplatFieldOrigin> origins;
    // 只有完整父链的uubs为Absent、有效ubdg明确0、hero/creep分类已知，且公共schema
    // 对该实际typed对象明确不适用时可赋值；不按rawcode猜类别，不把缺标志当false。
    // uubs仍不存在；origins另保留ubdg/hero/creep和uubs-applicability-schema证据。
    BuildingSplatConfiguration configuration = BuildingSplatConfiguration::EffectiveField;
};
struct BuildingSplatUnitInput {
    MapData::PlacementId owner;
    WorldSelectionVersion version;
    std::array<float, 3> position{};
    BuildingSplatEffectiveFields fields;
};
// 借用宿主持有的冻结输入；查询期间不能修改。inputIdentity必须覆盖该序列及顺序，
// 不是任意用户筛选字符串。正式owner须由宿主当前文档/注册表核验且整序列唯一；
// 服务仅检查当前页重复，不扫描全部地图来验证下一页。本服务不猜对象身份。
struct BuildingSplatQueryContext {
    WorldSelectionVersion version;
    std::string inputIdentity;
    std::span<const BuildingSplatUnitInput> units;
};
enum class BuildingSplatAvailability : std::uint8_t { Available, Absent, Unavailable };
struct BuildingSplatFootprint {
    std::array<float, 2> minimum{}, maximum{};
    float fullWidth = 0, fullHeight = 0;
    std::array<std::uint8_t, 4> color{};
    // 1.27a普通建筑runtime路径覆盖：Birth/Pause=0，三个阶段均使用此tint/alpha255。
    float buildingBirth = 0, buildingPause = 0;
    bool tableSizeIndependentOfModel = true;
    // 只是有证的XY范围/颜色；没有宣称原版terrain投影、blend、深度或完整生命周期。
    bool projectionVerified = false, blendVerified = false, lifecycleVerified = false;
};
struct BuildingSplatRow {
    MapData::PlacementId owner;
    WorldSelectionVersion version;
    std::uint64_t inputIndex = 0;
    BuildingSplatAvailability availability = BuildingSplatAvailability::Unavailable;
    std::string splatKey, reason;
    std::optional<BuildingSplatFootprint> footprint;
    std::vector<BuildingSplatFieldOrigin> origins;
    std::string reasonCode; // availability机器分类，不按message猜测。
};
struct BuildingSplatCursor {
    WorldSelectionVersion version;
    std::string catalogIdentity, inputIdentity;
    std::uint64_t nextInput = 0;
};
struct BuildingSplatQueryOptions {
    std::size_t limit = 64; // 1..128，逐页继续；不是整张地图实例数上限。
    std::size_t maxLogicalBytes = 256 * 1024; // 1..1MiB，owned行载荷；不重复计算共享目录。
};
struct BuildingSplatPage {
    WorldSelectionVersion version;
    std::shared_ptr<const BuildingSplatCatalog> catalog; // 保活key对应的完整raw定义和来源。
    std::vector<BuildingSplatRow> rows;
    std::optional<BuildingSplatCursor> next;
    std::size_t logicalBytes = 0;
};
[[nodiscard]] Result<BuildingSplatPage> QueryBuildingSplatFootprints(
    std::shared_ptr<const BuildingSplatCatalog> catalog, const BuildingSplatQueryContext& context,
    const WorldSelectionVersion& expected, std::string_view expectedCatalogIdentity,
    const BuildingSplatQueryOptions& options = {}, const std::optional<BuildingSplatCursor>& cursor = {});

} // namespace War3Map::Services

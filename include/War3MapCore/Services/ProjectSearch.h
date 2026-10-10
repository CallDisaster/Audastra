#pragma once

#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Jass/JassDeclarationIndex.h"

#include <array>
#include <span>

namespace War3Map::Trigger { class TriggerDocument; class TriggerTypeSystem; }
namespace War3Map::ObjectData { class StandardObjectCatalog; }
namespace War3Map::Resources { class ImportTable; }

namespace War3Map::Services {

enum class ProjectSearchCategory : std::uint8_t {
    Trigger, Variable, MapObject, StandardObject, Sound, ImportedFile,
    UnitPlacement, DoodadPlacement, Region, Camera, CodeSymbol, Count,
};
enum class ProjectSearchState : std::uint8_t {
    Ready, Partial, Missing, Unavailable, Invalid, Ambiguous, QueryTooShort,
};
enum class ProjectSearchObjectOrigin : std::uint8_t { Lni, Native };

// 当前可见 IR 的借用视图；同一种物编只传实际生效的 native/LNI 来源。
struct ProjectSearchObjectSource {
    ObjectData::ObjectType type = ObjectData::ObjectType::Unit;
    std::span<const ObjectData::ObjectRecord> records;
    ProjectSearchState state = ProjectSearchState::Unavailable;
    ProjectSearchObjectOrigin origin = ProjectSearchObjectOrigin::Lni;
};

// 宿主从当前资源编辑态组装（含待导入、改名、删除），UTF8 字符串的所有权由宿主持有。
struct ProjectSearchImportedFile {
    std::string_view stableId;
    std::string_view path;
};

// 宿主在文档所属线程组装并串行查询；调用期间不许修改借用数据。
// revision 必须覆盖工程身份、文档世代与各当前 overlay/目录修订。切图和编辑后更新，
// 激活结果前再次比较，不以名字相同或 sourceIndex 相同代替身份验证。
struct ProjectSearchContext {
    std::string revision;
    const Trigger::TriggerDocument* triggers = nullptr;
    const Trigger::TriggerTypeSystem* types = nullptr;
    ProjectSearchState triggerState = ProjectSearchState::Unavailable;
    std::span<const ProjectSearchObjectSource> objects;
    const ObjectData::StandardObjectCatalog* standardObjects = nullptr;
    ProjectSearchState standardState = ProjectSearchState::Unavailable;
    const MapDataQuerySnapshot* mapData = nullptr;
    MapDataQueryBinding mapDataBinding;
    const MapData::StringTableDocument* strings = nullptr;
    const Resources::ImportTable* imports = nullptr;
    ProjectSearchState importState = ProjectSearchState::Unavailable;
    std::span<const ProjectSearchImportedFile> currentFiles;
    bool usesImportedFiles = false; // 为真时只查 currentFiles，空 span 也不回退旧 ImportTable。
    const Jass::DeclarationIndex* codeDeclarations = nullptr;
    ProjectSearchState codeState = ProjectSearchState::Unavailable;
    std::string_view codeBinding; // 宿主已核当前来源；激活仍需同一 binding。
};

struct ProjectSearchRow {
    ProjectSearchCategory category = ProjectSearchCategory::Trigger;
    std::string stableId, title, detail, contextRevision;
    std::int64_t score = 0;
    std::optional<StableId> sourceId;
    std::optional<ObjectData::ObjectKey> object;
    std::optional<MapDataKind> mapKind;
    std::optional<std::uint64_t> sourceIndex;
    std::string rowKey;
    bool textTruncated = false;
    std::optional<std::string> codeSource;
    std::uint32_t codeLine = 0, codeColumn = 1;
    std::string codeBinding;
    // 参数结果沿当前 IR 导航；sourceId 为所属触发器，不用文本或数组位置代替身份。
    std::optional<StableId> ecaId, argumentId;
    std::optional<std::string> rawValue; // 有界 UTF8 原值；title 可为名称解析后的显示值。
};
struct ProjectSearchCoverage {
    ProjectSearchCategory category = ProjectSearchCategory::Trigger;
    ProjectSearchState state = ProjectSearchState::Unavailable;
    std::size_t examined = 0, matches = 0;
    std::optional<std::size_t> totalRows;
    bool scanTruncated = false, resultsTruncated = false;
    std::size_t graphNodesExamined = 0, textBytesExamined = 0;
};
struct ProjectSearchResult {
    std::string contextRevision;
    std::vector<ProjectSearchRow> rows;
    std::array<ProjectSearchCoverage, static_cast<std::size_t>(ProjectSearchCategory::Count)> coverage;
    bool complete = false, hasMore = false;
    // 全局名称解析计费（含完整域核对及共用解析器的保守工作上界），不是命中数。
    std::size_t nameNodesCharged = 0, nameTextBytesCharged = 0;
};
struct ProjectSearchOptions {
    std::size_t limit = 40; // 1..128 条，按同源 FuzzyCommandScore 排序，同分保持来源顺序。
    std::size_t maxExaminedPerCategory = 4096; // 1..32768；不无界复制标准目录。
    std::size_t maxResultBytes = 128 * 1024; // 1..256 KiB；单条文本最多各 2048 字节。
    std::size_t maxTriggerNodes = 8192; // 1..65536，含重复边；也是独立全局名称解析节点账上限。
    std::size_t maxTriggerTextBytes = 1024 * 1024; // 1..8 MiB；也是独立全局名称解析字节账上限。
};

// 游标是拥有字符串的值对象；SHA 绑定 exact query、当前修订、所有预算、页长和排序窗口。
struct ProjectSearchCursor {
    std::string contextRevision, queryUtf8, windowSha256;
    std::size_t nextIndex = 0;
};
struct ProjectSearchPage {
    std::string contextRevision, queryUtf8, windowSha256;
    std::vector<ProjectSearchRow> rows;
    std::array<ProjectSearchCoverage, static_cast<std::size_t>(ProjectSearchCategory::Count)> coverage;
    std::optional<ProjectSearchCursor> next;
    std::size_t totalWindowRows = 0;
    bool complete = false; // 来源扫描覆盖；不表示地图所有内容都已解析。
    bool windowHasMore = false; // 排序窗口外还有命中；须缩小查询，不伪造无限全量分页。
    std::size_t nameNodesCharged = 0, nameTextBytesCharged = 0;
};

[[nodiscard]] std::string_view ProjectSearchCategoryName(ProjectSearchCategory category) noexcept;
[[nodiscard]] Result<ProjectSearchResult> SearchProject(const ProjectSearchContext& context,
    std::string_view expectedContextRevision, std::string_view queryUtf8,
    const ProjectSearchOptions& options = {});
// 同步查询当前借用 IR，并返回拥有内容的页；借用 context 不能跨异步保存。
// 每页重跑同一有界搜索并核 SHA，窗口最多128条；next 仅表示窗口内尚有页。
[[nodiscard]] Result<ProjectSearchPage> SearchProjectPage(const ProjectSearchContext& context,
    std::string_view expectedContextRevision, std::string_view queryUtf8,
    const ProjectSearchOptions& options = {128, 4096, 128 * 1024},
    std::size_t pageLimit = 40, const ProjectSearchCursor* cursor = nullptr);

} // namespace War3Map::Services

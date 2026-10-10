#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

enum class MapSizeCategory : std::uint8_t { Model, Texture, Sound, Script, Other, Count };
enum class MapSizeState : std::uint8_t { Ready, Partial, Unknown };
enum class MapSizeSourceKind : std::uint8_t { ArchiveMembers, DirectorySnapshot };
enum class MapSizeByteTrust : std::uint8_t { OwnedMetadata, ReportedUnverified, Unknown };
enum class MapSizeRowOrigin : std::uint8_t { Original, Renamed, PendingImport, PendingReplacement };

// 全部是宿主冻结的拥有值；ArchiveMembers 只取已有 Members().name/data.size()，
// DirectorySnapshot 只取已有 Snapshot().files.relativePath/size。不得为查询重新打开、
// 解压、散列或 rescan。统计范围是会话捕获的命名逻辑成员，不是整张压缩地图的磁盘大小。
struct MapSizeMemberMetadata {
    std::string path;
    std::optional<std::uint64_t> logicalBytes;
};
struct MapSizeAssetEditMetadata {
    std::string from, to; // from 为原始成员路径；to 为空表示删除。
};
struct MapSizePendingMetadata {
    std::string path;
    std::optional<std::uint64_t> logicalBytes;
    // 仅拥有正文的当前 size 可用 OwnedMetadata；外盘 PendingImport.size 只能 ReportedUnverified。
    MapSizeByteTrust trust = MapSizeByteTrust::ReportedUnverified;
};
struct MapSizeContext {
    std::string contextRevision; // 覆盖工程身份、打开世代、文档/AssetState/脚本修订；激活前宿主再核对。
    MapSizeSourceKind sourceKind = MapSizeSourceKind::ArchiveMembers;
    std::vector<MapSizeMemberMetadata> members;
    std::vector<MapSizeAssetEditMetadata> assetEdits;
    std::vector<MapSizePendingMetadata> pending;
    bool membersComplete = false; // 只说明上述捕获范围；目录权限跳过/宿主截断必须 false。
    bool overlayComplete = true; // 假时不把未完整应用的旧成员大小称为当前 effective 大小。
    std::uint64_t opaqueMemberCount = 0;
    bool opaqueMemberCountUnknown = false;
    // 对应 war3map.j/lua（根、scripts 或目录 map 前缀）的当前生成大小未知，保留 baseline。
    // 其他导入脚本的已知大小不受影响；没有对应捕获行时记录一项未列出的未知脚本。
    bool generatedScriptSizeUnknown = false;
};
struct MapSizeAnalysisOptions {
    std::size_t maxMetadataRows = 4096; // 全局成员+edit+pending预算，1..32768；不是各自4096。
    std::size_t maxPathBytes = 2048; // 单条UTF8路径，1..2048；不截断路径后猜身份。
    std::size_t maxTotalPathBytes = 1024 * 1024; // 全部处理路径，1..8MiB。
    std::size_t maxResultBytes = 64 * 1024; // 1..256KiB，返回字符串/字段的保守输出账，非磁盘大小。
};
struct MapSizeCategoryTotals {
    MapSizeCategory category = MapSizeCategory::Other;
    std::optional<std::uint64_t> knownBytes = 0;
    std::optional<std::uint64_t> reportedUnverifiedBytes = 0;
    std::size_t knownRows = 0, reportedUnverifiedRows = 0, unknownRows = 0;
};
struct MapSizeTotals {
    MapSizeState state = MapSizeState::Unknown;
    std::array<MapSizeCategoryTotals, static_cast<std::size_t>(MapSizeCategory::Count)> categories;
    // 溢出为null，不返回饱和值或把前缀和冒充总和；未验证上报值始终与known分开。
    std::optional<std::uint64_t> knownLogicalSubtotal = 0;
    std::optional<std::uint64_t> reportedUnverifiedSubtotal = 0;
    std::size_t knownRows = 0, reportedUnverifiedRows = 0, unknownRows = 0;
    bool sumOverflow = false;
};
struct MapSizeRow {
    std::string identity, originalPath, currentPath;
    MapSizeCategory category = MapSizeCategory::Other;
    std::optional<std::uint64_t> originalLogicalBytes, currentLogicalBytes;
    MapSizeByteTrust trust = MapSizeByteTrust::Unknown;
    MapSizeRowOrigin origin = MapSizeRowOrigin::Original;
    bool nonAsciiIdentity = false, managedMember = false, navigationAllowed = false;
};
struct MapSizeReport {
    std::string contextRevision;
    MapSizeSourceKind sourceKind = MapSizeSourceKind::ArchiveMembers;
    MapSizeState state = MapSizeState::Unknown;
    MapSizeTotals baseline, effective;
    // 仅在已处理effective metadata窗口内按当前字节降序、identity升序取最多20项。
    // Unknown大小不参与排序；ReportedUnverified有明确trust。Partial不能称全图最大的20项。
    std::vector<MapSizeRow> top20;
    bool topWindowComplete = false, inputTruncated = false, outputTruncated = false;
    bool effectiveUnknown = false, generatedScriptSizeUnknown = false;
    std::size_t examinedMetadataRows = 0, processedPathBytes = 0;
    std::size_t invalidPathRows = 0, ambiguousRows = 0, nonAsciiRows = 0, deletedRows = 0;
    std::uint64_t opaqueMemberCount = 0;
    bool opaqueMemberCountUnknown = false;
    std::size_t outputBudgetBytes = 0;
    // 固定原因代码去重、中文说明；不输出未知/匿名路径或正文，不按坏输入制造无界诊断。
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] std::string_view MapSizeCategoryName(MapSizeCategory category) noexcept;
[[nodiscard]] Result<MapSizeReport> AnalyzeMapSize(const MapSizeContext& context,
    std::string_view expectedContextRevision, const MapSizeAnalysisOptions& options = {});

} // namespace War3Map::Services

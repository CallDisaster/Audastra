#pragma once

#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Services {

// 宿主从同一打开会话取得四项身份；Core 无法从 MapDocument 单独证明归档 SHA。
// Capture 接管文档值并冻结解析结果。generation 改变表示重新打开或换源；revision
// 改变表示当前会话的编辑事务已提交。二者均由宿主管理，所有后续查询须精确回传。
struct MapDataQueryBinding {
    std::string documentId;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::string sourceSha256;

    [[nodiscard]] bool operator==(const MapDataQueryBinding&) const = default;
};

enum class MapDataQueryState {
    Missing, Ready, Partial, Invalid, Ambiguous, Unreadable, OpaqueUnknown,
};

enum class MapDataQueryTextState {
    Literal, Resolved, ReferenceInvalid, ReferenceMissing, ReferenceAmbiguous,
    StringTableUnavailable, CatalogUnavailable,
};

struct MapDataQueryText {
    std::string rawBytes;
    std::optional<std::string> displayBytes;
    MapDataQueryTextState state = MapDataQueryTextState::Literal;
};

struct MapDataQueryMember {
    MapDataKind kind = MapDataKind::Settings;
    std::string memberName;
    MapDataQueryState state = MapDataQueryState::Missing;
    std::optional<std::size_t> recordCount;
    std::optional<std::string> memberSha256;
    std::size_t diagnosticCount = 0;
};

enum class MapDataQueryRowForm { Record, NormalDoodad, SpecialDoodad };

struct MapDataQueryField {
    const MapData::MapFieldDescriptor* descriptor = nullptr;
    MapData::MapFieldValue value;
    // 目录是全局能力；这一位还需满足当前成员已完整可编辑。
    bool writable = false;
};

// sourceIndex 是当前修订内的定位符，不是跨修订稳定 ID，也不授予写入能力。
// 所有文本及 rawcode 均为原始字节；JSON 等适配层负责无损编码和 64 位数转十进制字符串。
struct MapDataQueryRow {
    std::uint64_t sourceIndex = 0;
    MapDataQueryRowForm form = MapDataQueryRowForm::Record;
    std::string rawcodeBytes;
    std::optional<std::int32_t> creationNumber;
    std::optional<std::uint32_t> stringId;
    MapDataQueryText name;
    std::optional<std::int32_t> owner;
    std::optional<std::array<float, 3>> positionWorld;
    std::optional<std::array<std::int32_t, 2>> gridPosition;
    std::optional<std::string> resourcePathBytes;
    // 行身份令牌与字段目录中的全部字段（含只读字段）；写入请求回传 rowKey 与字段原值。
    std::string rowKey;
    std::vector<MapDataQueryField> fields;
};

struct MapDataQueryCursor {
    MapDataQueryBinding binding;
    MapDataKind kind = MapDataKind::Settings;
    std::uint64_t nextIndex = 0;
};

struct MapDataQueryDescription {
    MapDataQueryBinding binding;
    bool hasOpaqueArchiveMembers = false;
    std::array<MapDataQueryMember, 7> members;
};

struct MapDataQueryPage {
    MapDataQueryBinding binding;
    MapDataQueryMember member;
    std::vector<MapDataQueryRow> rows;
    std::optional<MapDataQueryCursor> next;
    std::size_t logicalBytes = 0;
};

struct MapDataUnitFilter {
    std::optional<std::string> typeIdBytes; // 恰好四个原始字节，区分大小写，不查对象目录。
    std::optional<std::int32_t> player; // 精确匹配 native owner，不套用字段写入范围。

    [[nodiscard]] bool operator==(const MapDataUnitFilter&) const = default;
};

struct MapDataUnitQueryCursor {
    MapDataQueryCursor source;
    MapDataUnitFilter filter;
    std::optional<std::string> memberSha256; // null 只绑定实际 Missing；已有成员绑定真实 SHA。
};

struct MapDataUnitQueryPage {
    MapDataQueryBinding binding;
    MapDataQueryMember member;
    std::vector<MapDataQueryRow> rows;
    std::optional<MapDataUnitQueryCursor> next;
    std::size_t examined = 0, logicalBytes = 0;
    std::optional<std::uint64_t> matchedTotal;
    bool scanBudgetExhausted = false;
};

class MapDataQuerySnapshot {
public:
    MapDataQuerySnapshot(const MapDataQuerySnapshot&) = default;
    MapDataQuerySnapshot(MapDataQuerySnapshot&&) noexcept = default;
    MapDataQuerySnapshot& operator=(const MapDataQuerySnapshot&) = default;
    MapDataQuerySnapshot& operator=(MapDataQuerySnapshot&&) noexcept = default;

    [[nodiscard]] const MapDataQueryBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] std::shared_ptr<const MapDocument> OwnedDocument() const noexcept { return document_; }

private:
    MapDataQuerySnapshot(std::shared_ptr<const MapDocument> document,
        MapDataQueryBinding binding, bool hasOpaqueArchiveMembers,
        std::array<MapDataQueryMember, 7> members);

    std::shared_ptr<const MapDocument> document_;
    MapDataQueryBinding binding_;
    bool hasOpaqueArchiveMembers_ = false;
    std::array<MapDataQueryMember, 7> members_;

    friend Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
        MapDocument, MapDataQueryBinding, bool);
    friend Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
        std::shared_ptr<const MapDocument>, MapDataQueryBinding, bool);
    friend Result<MapDataQueryDescription> DescribeMapData(
        const MapDataQuerySnapshot&, const MapDataQueryBinding&);
    friend Result<MapDataQueryPage> QueryMapDataPage(
        const MapDataQuerySnapshot&, const MapDataQueryBinding&, MapDataKind,
        const std::optional<MapDataQueryCursor>&, std::size_t, std::size_t);
    friend Result<MapDataUnitQueryPage> QueryMapDataUnitsPage(
        const MapDataQuerySnapshot&, const MapDataQueryBinding&, const MapDataUnitFilter&,
        const std::optional<MapDataUnitQueryCursor>&, std::size_t, std::size_t, std::size_t);
};

[[nodiscard]] Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
    MapDocument document, MapDataQueryBinding binding, bool hasOpaqueArchiveMembers);
// 调用者移交已冻结的自有文档；保留所有成员摘要/结构检查，不借可变编辑会话。
[[nodiscard]] Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
    std::shared_ptr<const MapDocument> document, MapDataQueryBinding binding, bool hasOpaqueArchiveMembers);
[[nodiscard]] Result<MapDataQueryDescription> DescribeMapData(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected);
// 固定输入顺序，无隐式筛选。单页最多 128 条、256 KiB；超限行显式报错。
// Missing 返回显式状态和零行；Invalid/Ambiguous/Unreadable/OpaqueUnknown 拒绝伪造空表。
[[nodiscard]] Result<MapDataQueryPage> QueryMapDataPage(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected,
    MapDataKind kind, const std::optional<MapDataQueryCursor>& cursor = std::nullopt,
    std::size_t limit = 64, std::size_t maxLogicalBytes = 256 * 1024);

// 仅筛选 units.doo 原行，最多 128 条、4096 次 native 谓词检查和 252 KiB 逻辑正文。
// 全表行数不超过扫描预算时，每页从原行 0 计可信全局总数；更大表始终不报告总数。
// 零行且有 next 不表示全表零匹配；游标绑定原索引、过滤条件、完整身份及成员 SHA/presence。
// 来源状态规则与无过滤查询一致；不构造未命中行，不改变快照或任何编辑会话。
[[nodiscard]] Result<MapDataUnitQueryPage> QueryMapDataUnitsPage(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected,
    const MapDataUnitFilter& filter,
    const std::optional<MapDataUnitQueryCursor>& cursor = std::nullopt,
    std::size_t limit = 64, std::size_t maxExamined = 4096,
    std::size_t maxLogicalBytes = 252 * 1024);

} // namespace War3Map::Services

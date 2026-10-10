#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace War3Map::Services {
namespace {

constexpr std::size_t kKindCount = static_cast<std::size_t>(MapDataKind::Count);
constexpr std::size_t kMaxRows = 128;
constexpr std::size_t kMaxLogicalBytes = 256 * 1024;
constexpr std::size_t kMaxUnitLogicalBytes = 252 * 1024;
constexpr std::size_t kMaxUnitExamined = 4096;
static_assert(kKindCount == 7);
static_assert(sizeof(float) == sizeof(std::uint32_t));

template<class T>
Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("MapData.Query", std::move(code), std::move(message));
}

bool ValidBinding(const MapDataQueryBinding& binding) {
    if (binding.documentId.empty() || binding.documentId.size() > 128 || binding.generation == 0
        || binding.sourceSha256.size() != 64) return false;
    for (const char c : binding.documentId) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    for (const char c : binding.sourceSha256) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

MapDataQueryState QueryState(MapDataState state, bool opaque) {
    switch (state) {
    case MapDataState::Missing: return opaque ? MapDataQueryState::OpaqueUnknown : MapDataQueryState::Missing;
    case MapDataState::Ready: return MapDataQueryState::Ready;
    case MapDataState::Partial: return MapDataQueryState::Partial;
    case MapDataState::Invalid: return MapDataQueryState::Invalid;
    case MapDataState::Ambiguous: return MapDataQueryState::Ambiguous;
    case MapDataState::Unreadable: return MapDataQueryState::Unreadable;
    }
    return MapDataQueryState::Invalid;
}

Result<MapDataQueryText> Text(const MapDocument& document, std::string_view raw) {
    MapDataQueryText text;
    text.rawBytes = raw;
    NameResolutionContext names;
    names.mapDocument = &document;
    names.preserveStringFormatting = true;
    const auto resolved = ResolveName(names, raw, "string");
    if (resolved.state == NameResolutionState::Literal) {
        text.displayBytes = resolved.displayName;
        return Result<MapDataQueryText>::Success(std::move(text));
    }
    if (resolved.state == NameResolutionState::Resolved) {
        text.displayBytes = resolved.displayName;
        text.state = MapDataQueryTextState::Resolved;
        return Result<MapDataQueryText>::Success(std::move(text));
    }
    if (resolved.state == NameResolutionState::Invalid) text.state = MapDataQueryTextState::ReferenceInvalid;
    else if (resolved.state == NameResolutionState::MissingString) text.state = document.Strings()
        ? MapDataQueryTextState::ReferenceMissing : MapDataQueryTextState::StringTableUnavailable;
    else if (resolved.state == NameResolutionState::Ambiguous) text.state = MapDataQueryTextState::ReferenceAmbiguous;
    else return Failure<MapDataQueryText>("MAP_DATA_QUERY_TEXT_ERROR", resolved.reason);
    return Result<MapDataQueryText>::Success(std::move(text));
}

Result<MapDataQueryRow> MakeRow(const MapDocument& document, MapDataKind kind, std::size_t index) {
    MapDataQueryRow row;
    row.sourceIndex = index;
    std::string_view name;
    switch (kind) {
    case MapDataKind::Settings:
        if (!document.Settings() || index != 0) break;
        name = document.Settings()->mapName;
        break;
    case MapDataKind::Units:
        if (!document.Units() || index >= document.Units()->units.size()) break;
        {
            const auto& unit = document.Units()->units[index];
            row.rawcodeBytes = unit.rawcode;
            row.creationNumber = unit.creationId;
            row.owner = unit.owner;
            row.positionWorld = unit.position;
            row.name.state = MapDataQueryTextState::CatalogUnavailable;
            return Result<MapDataQueryRow>::Success(std::move(row));
        }
    case MapDataKind::Doodads:
        if (!document.Doodads()) break;
        if (index < document.Doodads()->doodads.size()) {
            const auto& doodad = document.Doodads()->doodads[index];
            row.form = MapDataQueryRowForm::NormalDoodad;
            row.rawcodeBytes = doodad.rawcode;
            row.creationNumber = doodad.creationId;
            row.positionWorld = doodad.position;
        } else {
            const auto specialIndex = index - document.Doodads()->doodads.size();
            if (specialIndex >= document.Doodads()->specialDoodads.size()) break;
            const auto& doodad = document.Doodads()->specialDoodads[specialIndex];
            row.form = MapDataQueryRowForm::SpecialDoodad;
            row.rawcodeBytes = doodad.rawcode;
            row.gridPosition = doodad.position;
        }
        row.name.state = MapDataQueryTextState::CatalogUnavailable;
        return Result<MapDataQueryRow>::Success(std::move(row));
    case MapDataKind::Regions:
        if (!document.Regions() || index >= document.Regions()->entries.size()) break;
        {
            const auto& region = document.Regions()->entries[index];
            name = region.name;
            row.creationNumber = region.creationNumber;
        }
        break;
    case MapDataKind::Cameras:
        if (!document.Cameras() || index >= document.Cameras()->entries.size()) break;
        {
            const auto& camera = document.Cameras()->entries[index];
            name = camera.name;
        }
        break;
    case MapDataKind::Sounds:
        if (!document.Sounds() || index >= document.Sounds()->entries.size()) break;
        name = document.Sounds()->entries[index].name;
        row.resourcePathBytes = document.Sounds()->entries[index].filePath;
        break;
    case MapDataKind::Strings:
        if (!document.Strings() || index >= document.Strings()->entries.size()) break;
        {
            const auto& entry = document.Strings()->entries[index];
            row.stringId = entry.id;
            row.name.rawBytes = entry.value;
            row.name.displayBytes = Trigger::DecodeSourceText(std::as_bytes(std::span(entry.value.data(), entry.value.size()))).text;
            return Result<MapDataQueryRow>::Success(std::move(row));
        }
    case MapDataKind::Count: break;
    }
    if (!name.data()) return Failure<MapDataQueryRow>(
        "MAP_DATA_QUERY_INCONSISTENT", "成员计数与已解析记录不一致。");
    auto parsed = Text(document, name);
    if (!parsed.Succeeded()) {
        Result<MapDataQueryRow> failure;
        failure.diagnostics = std::move(parsed.diagnostics);
        return failure;
    }
    row.name = std::move(*parsed.value);
    return Result<MapDataQueryRow>::Success(std::move(row));
}

// 行身份与字段目录读自同一文档；字段按目录顺序附加，只读字段同样列出。
Result<MapDataQueryRow> AttachFields(const MapDocument& document, MapDataKind kind,
    std::size_t index, MapDataQueryRow row) {
    auto key = MapData::MapFieldRowKey(document, kind, index);
    if (!key.Succeeded() || !key.value) {
        Result<MapDataQueryRow> failure;
        failure.diagnostics = std::move(key.diagnostics);
        return failure;
    }
    row.rowKey = std::move(*key.value);
    for (const auto& field : MapData::MapFieldCatalog(kind)) {
        auto value = MapData::ReadMapField(document, kind, index, field);
        if (!value.Succeeded() || !value.value) {
            // 普通与特殊装饰物共用成员类别，但字段布局不同；只跳过明确不适用的列。
            if (!value.value && value.diagnostics.size() == 1 &&
                value.diagnostics.front().code == "MAP_FIELD_NOT_APPLICABLE") continue;
            Result<MapDataQueryRow> failure;
            failure.diagnostics = std::move(value.diagnostics);
            return failure;
        }
        row.fields.push_back({&field, std::move(*value.value),
            document.Member(kind).state == MapDataState::Ready && MapData::MapFieldRowAllowsWrite(document, index, field)});
    }
    return Result<MapDataQueryRow>::Success(std::move(row));
}

std::size_t RowLogicalBytes(const MapDataQueryRow& row) {
    std::size_t bytes = 128 + row.rawcodeBytes.size() + row.name.rawBytes.size() + row.rowKey.size();
    for (const auto& field : row.fields) bytes += 64 + field.descriptor->id.size() + field.value.bytes.size() * 3;
    if (row.name.displayBytes) bytes += row.name.displayBytes->size();
    if (row.resourcePathBytes) bytes += row.resourcePathBytes->size();
    return bytes;
}

} // namespace

MapDataQuerySnapshot::MapDataQuerySnapshot(std::shared_ptr<const MapDocument> document,
    MapDataQueryBinding binding, bool hasOpaqueArchiveMembers,
    std::array<MapDataQueryMember, 7> members)
    : document_(std::move(document)), binding_(std::move(binding)),
      hasOpaqueArchiveMembers_(hasOpaqueArchiveMembers), members_(std::move(members)) {}

Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
    MapDocument document, MapDataQueryBinding binding, bool hasOpaqueArchiveMembers) {
    if (!ValidBinding(binding)) return Failure<MapDataQuerySnapshot>(
        "MAP_DATA_QUERY_BINDING_INVALID", "文档身份、世代或来源 SHA-256 无效。");
    try {
        return CaptureMapDataSnapshot(std::make_shared<const MapDocument>(std::move(document)),
            std::move(binding), hasOpaqueArchiveMembers);
    } catch (const std::bad_alloc&) {
        return Failure<MapDataQuerySnapshot>("MAP_DATA_QUERY_ALLOCATION", "基础数据快照内存分配失败。");
    } catch (const std::length_error&) {
        return Failure<MapDataQuerySnapshot>("MAP_DATA_QUERY_LENGTH", "基础数据快照长度超出容器限制。");
    }
}

Result<MapDataQuerySnapshot> CaptureMapDataSnapshot(
    std::shared_ptr<const MapDocument> document, MapDataQueryBinding binding, bool hasOpaqueArchiveMembers) {
    if (!ValidBinding(binding)) return Failure<MapDataQuerySnapshot>(
        "MAP_DATA_QUERY_BINDING_INVALID", "文档身份、世代或来源 SHA-256 无效。");
    if (!document) return Failure<MapDataQuerySnapshot>(
        "MAP_DATA_QUERY_DOCUMENT_MISSING", "基础数据快照缺少自有文档。");
    try {
        std::array<MapDataQueryMember, 7> summaries;
        for (std::size_t i = 0; i < summaries.size(); ++i) {
            const auto& source = document->Member(static_cast<MapDataKind>(i));
            auto& summary = summaries[i];
            summary.kind = source.kind;
            summary.memberName = source.memberName;
            summary.state = QueryState(source.state, hasOpaqueArchiveMembers);
            summary.recordCount = source.recordCount;
            summary.diagnosticCount = source.diagnostics.size();
            if (source.sources.size() != 1) continue;
            auto hashed = Sha256(source.sources.front().data);
            if (!hashed.Succeeded()) {
                Result<MapDataQuerySnapshot> failure;
                failure.diagnostics = std::move(hashed.diagnostics);
                return failure;
            }
            summary.memberSha256 = std::move(*hashed.value);
        }
        return Result<MapDataQuerySnapshot>::Success(MapDataQuerySnapshot(
            std::move(document),
            std::move(binding), hasOpaqueArchiveMembers, std::move(summaries)));
    } catch (const std::bad_alloc&) {
        return Failure<MapDataQuerySnapshot>("MAP_DATA_QUERY_ALLOCATION", "基础数据快照内存分配失败。");
    } catch (const std::length_error&) {
        return Failure<MapDataQuerySnapshot>("MAP_DATA_QUERY_LENGTH", "基础数据快照长度超出容器限制。");
    }
}

Result<MapDataQueryDescription> DescribeMapData(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected) {
    if (expected != snapshot.binding_) return Failure<MapDataQueryDescription>(
        "MAP_DATA_QUERY_STALE", "文档身份、世代、修订或来源 SHA-256 已变化。");
    try {
        return Result<MapDataQueryDescription>::Success({snapshot.binding_,
            snapshot.hasOpaqueArchiveMembers_, snapshot.members_});
    } catch (const std::bad_alloc&) {
        return Failure<MapDataQueryDescription>("MAP_DATA_QUERY_ALLOCATION", "查询描述内存分配失败。");
    }
}

Result<MapDataQueryPage> QueryMapDataPage(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected,
    MapDataKind kind, const std::optional<MapDataQueryCursor>& cursor,
    std::size_t limit, std::size_t maxLogicalBytes) {
    if (expected != snapshot.binding_ || (cursor && cursor->binding != snapshot.binding_))
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_STALE", "文档身份、世代、修订或来源 SHA-256 已变化。");
    const auto kindIndex = static_cast<std::size_t>(kind);
    if (kindIndex >= kKindCount || (cursor && cursor->kind != kind))
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_CURSOR_INVALID", "成员类别或续页游标不匹配。");
    if (limit == 0 || limit > kMaxRows || maxLogicalBytes == 0 || maxLogicalBytes > kMaxLogicalBytes)
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_LIMIT", "单页须为 1–128 条且不超过 256 KiB。");
    const auto& member = snapshot.members_[kindIndex];
    if (member.state != MapDataQueryState::Ready && member.state != MapDataQueryState::Partial
        && member.state != MapDataQueryState::Missing)
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_MEMBER_UNAVAILABLE", "成员状态未知或不可可靠解析，不能视为空表。");
    if ((member.state == MapDataQueryState::Missing && member.recordCount)
        || (member.state != MapDataQueryState::Missing && !member.recordCount))
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_INCONSISTENT", "成员状态与记录计数不一致。");
    const std::size_t count = member.recordCount.value_or(0);
    const std::uint64_t first = cursor ? cursor->nextIndex : 0;
    if (first > count) return Failure<MapDataQueryPage>(
        "MAP_DATA_QUERY_CURSOR_INVALID", "续页游标越过成员记录数量。");
    try {
        MapDataQueryPage page{snapshot.binding_, member};
        page.logicalBytes = 256 + page.binding.documentId.size() + page.binding.sourceSha256.size()
            + page.member.memberName.size() + page.member.memberSha256.value_or("").size();
        if (page.logicalBytes > maxLogicalBytes) return Failure<MapDataQueryPage>(
            "MAP_DATA_QUERY_BUDGET", "单页预算不足以容纳基础描述。");
        page.rows.reserve(std::min(limit, count - static_cast<std::size_t>(first)));
        for (std::size_t i = static_cast<std::size_t>(first);
            i < count && page.rows.size() < limit; ++i) {
            auto made = MakeRow(*snapshot.document_, kind, i);
            if (made.Succeeded() && made.value) made = AttachFields(*snapshot.document_, kind, i, std::move(*made.value));
            if (!made.Succeeded()) {
                Result<MapDataQueryPage> failure;
                failure.diagnostics = std::move(made.diagnostics);
                return failure;
            }
            const auto rowBytes = RowLogicalBytes(*made.value);
            if (rowBytes > maxLogicalBytes - page.logicalBytes) {
                if (page.rows.empty()) return Failure<MapDataQueryPage>(
                    "MAP_DATA_QUERY_BUDGET", "单条记录超出允许的页预算；请提高预算或检查异常字段。");
                break;
            }
            page.logicalBytes += rowBytes;
            page.rows.push_back(std::move(*made.value));
        }
        const auto after = first + page.rows.size();
        if (after < count) page.next = MapDataQueryCursor{page.binding, kind, after};
        return Result<MapDataQueryPage>::Success(std::move(page));
    } catch (const std::bad_alloc&) {
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_ALLOCATION", "查询页内存分配失败。");
    } catch (const std::length_error&) {
        return Failure<MapDataQueryPage>("MAP_DATA_QUERY_LENGTH", "查询页长度超出容器限制。");
    }
}

Result<MapDataUnitQueryPage> QueryMapDataUnitsPage(
    const MapDataQuerySnapshot& snapshot, const MapDataQueryBinding& expected,
    const MapDataUnitFilter& filter, const std::optional<MapDataUnitQueryCursor>& cursor,
    std::size_t limit, std::size_t maxExamined, std::size_t maxLogicalBytes) {
    if (expected != snapshot.binding_ || (cursor && cursor->source.binding != snapshot.binding_))
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_STALE", "文档身份、世代、修订或来源 SHA-256 已变化。");
    if (cursor && (cursor->source.kind != MapDataKind::Units || cursor->filter != filter))
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_CURSOR_INVALID", "单位续页游标的成员类别或过滤条件不匹配。");
    if (filter.typeIdBytes && filter.typeIdBytes->size() != 4)
        return Failure<MapDataUnitQueryPage>("MAP_DATA_UNIT_FILTER_INVALID", "单位类型筛选须为恰好四个原始字节。");
    if (limit == 0 || limit > kMaxRows || maxExamined == 0 || maxExamined > kMaxUnitExamined
        || maxLogicalBytes == 0 || maxLogicalBytes > kMaxUnitLogicalBytes)
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_LIMIT", "单位页须为 1–128 条、1–4096 次扫描且逻辑正文不超过 252 KiB。");
    const auto& member = snapshot.members_[static_cast<std::size_t>(MapDataKind::Units)];
    if (member.state != MapDataQueryState::Ready && member.state != MapDataQueryState::Partial
        && member.state != MapDataQueryState::Missing)
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_MEMBER_UNAVAILABLE", "成员状态未知或不可可靠解析，不能视为空表。");
    if ((member.state == MapDataQueryState::Missing && (member.recordCount || member.memberSha256))
        || (member.state != MapDataQueryState::Missing && (!member.recordCount || !member.memberSha256)))
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_INCONSISTENT", "单位成员状态、记录计数与 SHA/presence 不一致。");
    if (cursor && cursor->memberSha256 != member.memberSha256)
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_MEMBER_STALE", "单位成员 SHA 或 presence 已变化，请从当前快照重新查询。");
    const std::size_t count = member.recordCount.value_or(0);
    if (member.state != MapDataQueryState::Missing
        && (!snapshot.document_->Units() || snapshot.document_->Units()->units.size() != count))
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_INCONSISTENT", "单位成员摘要与冻结 typed 表的记录计数不一致。");
    const std::uint64_t first = cursor ? cursor->source.nextIndex : 0;
    if (first > count) return Failure<MapDataUnitQueryPage>(
        "MAP_DATA_QUERY_CURSOR_INVALID", "续页游标越过单位成员记录数量。");
    try {
        MapDataUnitQueryPage page{snapshot.binding_, member};
        page.logicalBytes = 320 + page.binding.documentId.size() + page.binding.sourceSha256.size()
            + page.member.memberName.size() + page.member.memberSha256.value_or("").size()
            + filter.typeIdBytes.value_or("").size();
        if (page.logicalBytes > maxLogicalBytes) return Failure<MapDataUnitQueryPage>(
            "MAP_DATA_QUERY_BUDGET", "单位页预算不足以容纳身份、过滤条件及成员描述。");
        page.rows.reserve(std::min(limit, count - static_cast<std::size_t>(first)));
        // 小表从零扫描一遍取得可信总数；不接受客户端传入累计计数。
        const bool exactTotal = count <= maxExamined;
        if (exactTotal) page.matchedTotal = 0;
        const auto begin = exactTotal ? std::size_t{0} : static_cast<std::size_t>(first);
        const auto end = exactTotal ? count : begin + std::min(maxExamined, count - begin);
        auto nextIndex = static_cast<std::size_t>(first);
        bool accepting = true, pendingMatch = false;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& unit = snapshot.document_->Units()->units[i];
            ++page.examined;
            const bool matches = (!filter.typeIdBytes || unit.rawcode == *filter.typeIdBytes)
                && (!filter.player || unit.owner == *filter.player);
            if (matches && exactTotal) ++*page.matchedTotal;
            if (i < first) continue;
            if (!accepting) {
                pendingMatch = pendingMatch || matches;
                continue;
            }
            if (!matches) { nextIndex = i + 1; continue; }
            // 未命中原行只读取四字节类型与 owner，不展开字段、字符串或 JSON DTO。
            auto made = MakeRow(*snapshot.document_, MapDataKind::Units, i);
            if (made.Succeeded() && made.value)
                made = AttachFields(*snapshot.document_, MapDataKind::Units, i, std::move(*made.value));
            if (!made.Succeeded()) {
                Result<MapDataUnitQueryPage> failure;
                failure.diagnostics = std::move(made.diagnostics);
                return failure;
            }
            const auto rowBytes = RowLogicalBytes(*made.value);
            if (rowBytes > maxLogicalBytes - page.logicalBytes) {
                if (page.rows.empty()) return Failure<MapDataUnitQueryPage>(
                    "MAP_DATA_QUERY_BUDGET", "单条单位记录超出允许页预算。");
                accepting = false;
                pendingMatch = true;
                nextIndex = i; // 当前命中尚未返回，下一页必须重查该原行。
                if (!exactTotal) break;
                continue;
            }
            page.logicalBytes += rowBytes;
            page.rows.push_back(std::move(*made.value));
            nextIndex = i + 1;
            if (page.rows.size() == limit) {
                accepting = false;
                if (!exactTotal) break;
            }
        }
        if ((exactTotal && pendingMatch) || (!exactTotal && nextIndex < count))
            page.next = MapDataUnitQueryCursor{{page.binding, MapDataKind::Units, nextIndex}, filter, member.memberSha256};
        page.scanBudgetExhausted = !exactTotal && page.examined == maxExamined && nextIndex < count;
        return Result<MapDataUnitQueryPage>::Success(std::move(page));
    } catch (const std::bad_alloc&) {
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_ALLOCATION", "单位筛选页内存分配失败。");
    } catch (const std::length_error&) {
        return Failure<MapDataUnitQueryPage>("MAP_DATA_QUERY_LENGTH", "单位筛选页长度超出容器限制。");
    }
}

} // namespace War3Map::Services

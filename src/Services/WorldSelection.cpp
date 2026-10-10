#include "War3MapCore/Services/WorldSelection.h"
#include "War3MapCore/Services/CommandRegistry.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <algorithm>
#include <limits>
#include <map>
#include <type_traits>

namespace War3Map::Services {
namespace {
constexpr std::size_t kTextBytes = 2048, kObjectRows = 32768;
template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("WorldSelection", std::move(code), std::move(message));
}
Status Invalid(std::string code, std::string message) {
    return Status::Failure("WorldSelection", std::move(code), std::move(message));
}
bool Supported(MapDataKind kind) {
    return kind == MapDataKind::Units || kind == MapDataKind::Doodads
        || kind == MapDataKind::Regions || kind == MapDataKind::Cameras;
}
bool Complete(ProjectSearchState state) {
    return state == ProjectSearchState::Ready || state == ProjectSearchState::Missing;
}
Status ContextStatus(const WorldSelectionContext& context) {
    if (!context.document || !context.mapData || context.binding.documentId.empty()
        || context.binding.documentId.size() > 128 || !context.binding.generation
        || context.binding.sourceSha256.size() != 64
        || !std::all_of(context.binding.sourceSha256.begin(), context.binding.sourceSha256.end(),
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
        || context.mapData->Binding() != context.binding || context.objects.size() > 7)
        return Invalid("WORLD_SELECTION_CONTEXT", "选择上下文缺失、查询快照身份不一致或物编来源超限。");
    std::array<bool, 7> seen{};
    for (const auto& source : context.objects) {
        const auto type = static_cast<std::size_t>(source.type);
        if (type >= seen.size() || seen[type]) return Invalid("WORLD_SELECTION_SOURCES", "物编类型来源重复或无效。");
        seen[type] = true;
    }
    return Status::Ok();
}
bool ValidId(const MapDocument& document, const WorldObjectId& id) {
    return std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MapData::PlacementId>) {
            if (value.rawcode.size() != 4) return false;
            if (value.file == MapData::PlacementFile::Units) return document.Units()
                && value.index < document.Units()->units.size()
                && MapData::MakePlacementId(value.file, value.index, document.Units()->units[value.index]) == value;
            if (value.file == MapData::PlacementFile::Doodads) return document.Doodads()
                && value.index < document.Doodads()->doodads.size()
                && MapData::MakePlacementId(value.file, value.index, document.Doodads()->doodads[value.index]) == value;
            return false;
        } else if constexpr (std::is_same_v<T, MapData::RegionId>) {
            return value.name.size() <= kTextBytes && document.Regions() && value.index < document.Regions()->entries.size()
                && MapData::MakeRegionId(value.index, document.Regions()->entries[value.index]) == value;
        } else {
            return value.name.size() <= kTextBytes && document.Cameras() && value.index < document.Cameras()->entries.size()
                && MapData::MakeCameraId(value.index, document.Cameras()->entries[value.index]) == value;
        }
    }, id);
}
std::optional<WorldObjectId> RowId(const MapDocument& doc, const MapDataQueryRow& row, MapDataKind kind) {
    if (row.sourceIndex > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    const auto i = static_cast<std::uint32_t>(row.sourceIndex);
    std::optional<WorldObjectId> id;
    if (kind == MapDataKind::Units && doc.Units() && i < doc.Units()->units.size()) {
        const auto& record = doc.Units()->units[i];
        if (record.rawcode == row.rawcodeBytes && row.creationNumber == record.creationId)
            id = MapData::MakePlacementId(MapData::PlacementFile::Units, i, record);
    } else if (kind == MapDataKind::Doodads && row.form == MapDataQueryRowForm::NormalDoodad
        && doc.Doodads() && i < doc.Doodads()->doodads.size()) {
        const auto& record = doc.Doodads()->doodads[i];
        if (record.rawcode == row.rawcodeBytes && row.creationNumber == record.creationId)
            id = MapData::MakePlacementId(MapData::PlacementFile::Doodads, i, record);
    } else if (kind == MapDataKind::Regions && doc.Regions() && i < doc.Regions()->entries.size()) {
        const auto& record = doc.Regions()->entries[i];
        if (record.name == row.name.rawBytes && row.creationNumber == record.creationNumber)
            id = MapData::MakeRegionId(i, record);
    } else if (kind == MapDataKind::Cameras && doc.Cameras() && i < doc.Cameras()->entries.size()) {
        const auto& record = doc.Cameras()->entries[i];
        if (record.name == row.name.rawBytes) id = MapData::MakeCameraId(i, record);
    }
    return id && ValidId(doc, *id) ? id : std::nullopt;
}
// 同一世代修订后只能保留唯一创建身份；重复创建编号/名称不能靠仍占原下标假认同一对象。
struct RebindIdentities {
    std::map<std::pair<MapData::PlacementFile, std::int32_t>, std::size_t> placements;
    std::map<std::int32_t, std::size_t> regions;
    std::map<std::string_view, std::size_t> cameras;
    bool unitsComplete = true, doodadsComplete = true, regionsComplete = true, camerasComplete = true;
    explicit RebindIdentities(const MapDocument& doc) {
        if (doc.Units()) {
            unitsComplete = doc.Units()->units.size() <= kObjectRows;
            if (unitsComplete) for (const auto& r : doc.Units()->units) ++placements[{MapData::PlacementFile::Units, r.creationId}];
        }
        if (doc.Doodads()) {
            doodadsComplete = doc.Doodads()->doodads.size() <= kObjectRows;
            if (doodadsComplete) for (const auto& r : doc.Doodads()->doodads) ++placements[{MapData::PlacementFile::Doodads, r.creationId}];
        }
        if (doc.Regions()) {
            regionsComplete = doc.Regions()->entries.size() <= kObjectRows;
            if (regionsComplete) for (const auto& r : doc.Regions()->entries) ++regions[r.creationNumber];
        }
        if (doc.Cameras()) {
            camerasComplete = doc.Cameras()->entries.size() <= kObjectRows;
            if (camerasComplete) for (const auto& r : doc.Cameras()->entries) if (r.name.size() <= kTextBytes) ++cameras[r.name];
        }
    }
    bool Unique(const WorldObjectId& id) const {
        return std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MapData::PlacementId>) {
                const auto found = placements.find({value.file, value.creationId});
                return (value.file == MapData::PlacementFile::Units ? unitsComplete : doodadsComplete)
                    && found != placements.end() && found->second == 1;
            } else if constexpr (std::is_same_v<T, MapData::RegionId>) {
                const auto found = regions.find(value.creationNumber);
                return regionsComplete && found != regions.end() && found->second == 1;
            } else {
                const auto found = cameras.find(value.name);
                return camerasComplete && found != cameras.end() && found->second == 1;
            }
        }, id);
    }
};
struct ObjectLookup {
    const WorldSelectionContext& context;
    std::map<std::pair<ObjectData::ObjectType, std::string_view>, unsigned> hits;
    std::array<ProjectSearchState, 7> states;
    explicit ObjectLookup(const WorldSelectionContext& c, MapDataKind kind) : context(c) {
        states.fill(ProjectSearchState::Unavailable);
        std::size_t examined = 0;
        for (const auto& source : c.objects) {
            const auto type = source.type;
            if ((kind == MapDataKind::Units && type != ObjectData::ObjectType::Unit && type != ObjectData::ObjectType::Item)
                || (kind == MapDataKind::Doodads && type != ObjectData::ObjectType::Doodad && type != ObjectData::ObjectType::Destructable)) continue;
            auto& state = states[static_cast<std::size_t>(type)]; state = source.state;
            if (state != ProjectSearchState::Ready && state != ProjectSearchState::Partial) continue;
            for (const auto& record : source.records) {
                if (examined == kObjectRows) { state = ProjectSearchState::Partial; break; }
                ++examined;
                if (record.key.type != type || record.key.rawcode.size() != 4) { state = ProjectSearchState::Partial; continue; }
                ++hits[{type, record.key.rawcode}];
            }
        }
    }
    std::pair<bool, bool> Known(ObjectData::ObjectType type, std::string_view rawcode) const {
        const auto hit = hits.find({type, rawcode});
        const bool ambiguous = hit != hits.end() && hit->second > 1;
        const bool standard = context.standardState == ProjectSearchState::Ready || context.standardState == ProjectSearchState::Partial;
        const bool found = hit != hits.end() || (standard && context.standardObjects && context.standardObjects->Find(type, rawcode));
        const bool complete = Complete(states[static_cast<std::size_t>(type)]) && Complete(context.standardState)
            && (context.standardState != ProjectSearchState::Ready || context.standardObjects);
        return {found, complete && !ambiguous};
    }
    void Classify(WorldObjectRow& row, std::string_view rawcode) const {
        if (row.member == MapDataKind::Regions) { row.category = WorldCategory::Region; row.typeState = WorldTypeState::Known; return; }
        if (row.member == MapDataKind::Cameras) { row.category = WorldCategory::Camera; row.typeState = WorldTypeState::Known; return; }
        if (row.member == MapDataKind::Units && rawcode == "sloc") {
            row.category = WorldCategory::StartLocation; row.typeState = WorldTypeState::Known; return;
        }
        const bool unit = row.member == MapDataKind::Units;
        const auto first = unit ? ObjectData::ObjectType::Unit : ObjectData::ObjectType::Destructable;
        const auto second = unit ? ObjectData::ObjectType::Item : ObjectData::ObjectType::Doodad;
        const auto a = Known(first, rawcode), b = Known(second, rawcode);
        if (a.first && b.first) { row.typeState = WorldTypeState::Ambiguous; row.reason = "原始ID跨物体类型歧义。"; }
        else if (!a.second || !b.second) { row.typeState = WorldTypeState::Partial; row.reason = "当前物编或原版目录未完整可用，类别未确定。"; }
        else if (!a.first && !b.first) { row.typeState = WorldTypeState::Missing; row.reason = "当前物编和原版目录没有此对象。"; }
        else {
            row.category = a.first ? (unit ? WorldCategory::Unit : WorldCategory::Destructable)
                : (unit ? WorldCategory::Item : WorldCategory::Doodad);
            row.typeState = WorldTypeState::Known;
        }
    }
};
void BoundText(std::string& text, bool& partial) {
    if (text.size() > kTextBytes) { text.resize(kTextBytes); partial = true; }
}
std::size_t RowBytes(const WorldObjectRow& row) {
    const auto identity = row.id ? std::visit([](const auto& id) -> std::size_t {
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, MapData::PlacementId>) return id.rawcode.size();
        else return id.name.size();
    }, *row.id) : 0;
    return 256 + identity + row.rowKey.size() + row.name.rawBytes.size() + row.name.displayBytes.value_or("").size() + row.reason.size();
}
} // namespace

std::string_view WorldCategoryName(WorldCategory category) noexcept {
    switch (category) {
    case WorldCategory::Unit: return "单位";
    case WorldCategory::Item: return "物品";
    case WorldCategory::Destructable: return "可破坏物";
    case WorldCategory::Doodad: return "装饰物";
    case WorldCategory::Region: return "区域";
    case WorldCategory::Camera: return "镜头";
    case WorldCategory::StartLocation: return "起始位置";
    case WorldCategory::Unknown: return "未确定对象";
    }
    return "未确定对象";
}

void WorldSelection::Clear() noexcept {
    if (version_) retiredRevision_ = version_->editorRevision;
    version_.reset(); selected_.clear(); primary_.reset(); flags_.clear();
}
WorldEditorFlags WorldSelection::Flags(const WorldObjectId& id) const noexcept {
    const auto found = std::find_if(flags_.begin(), flags_.end(), [&](const auto& flag) { return flag.id == id; });
    return found == flags_.end() ? WorldEditorFlags{} : found->value;
}
Status WorldSelection::Bind(const WorldSelectionContext& context) {
    const auto checked = ContextStatus(context); if (!checked.Succeeded()) return checked;
    const bool same = version_ && version_->document.documentId == context.binding.documentId
        && version_->document.generation == context.binding.generation && version_->document.sourceSha256 == context.binding.sourceSha256;
    if (version_ && version_->document == context.binding && version_->catalogRevision == context.catalogRevision) return Status::Ok();
    const auto revision = version_ ? version_->editorRevision : retiredRevision_;
    if (revision == std::numeric_limits<std::uint64_t>::max()) return Invalid("WORLD_SELECTION_REVISION", "编辑器选择修订已耗尽。");
    if (!same) { selected_.clear(); primary_.reset(); flags_.clear(); }
    else if (!selected_.empty() || !flags_.empty() || primary_) {
        const RebindIdentities identities(*context.document);
        const auto stale = [&](const auto& id) { return !ValidId(*context.document, id) || !identities.Unique(id); };
        std::erase_if(selected_, stale);
        std::erase_if(flags_, [&](const auto& flag) { return stale(flag.id); });
        if (primary_ && std::find(selected_.begin(), selected_.end(), *primary_) == selected_.end()) primary_.reset();
        if (!primary_ && !selected_.empty()) primary_ = selected_.back();
    }
    version_ = WorldSelectionVersion{context.binding, context.catalogRevision, revision + 1};
    return Status::Ok();
}
Status WorldSelection::Check(const WorldSelectionContext& context, const WorldSelectionVersion& expected) const {
    const auto status = ContextStatus(context); if (!status.Succeeded()) return status;
    if (!version_ || *version_ != expected || context.binding != expected.document
        || context.catalogRevision != expected.catalogRevision)
        return Invalid("WORLD_SELECTION_STALE", "工程、地图、目录或编辑器选择修订已改变，请重新查询。");
    return Status::Ok();
}
Result<WorldSelectionSnapshot> WorldSelection::Snapshot(const WorldSelectionContext& context,
    const WorldSelectionVersion& expected) const {
    const auto status = Check(context, expected); if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    for (const auto& id : selected_) if (!ValidId(*context.document, id))
        return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_ID_STALE", "当前选择对象身份已改变。");
    return Result<WorldSelectionSnapshot>::Success({*version_, selected_, primary_});
}
Status WorldSelection::EditStatus(const WorldSelectionContext& context,
    const WorldSelectionVersion& expected) const {
    const auto snapshot = Snapshot(context, expected);
    if (!snapshot.Succeeded()) return {snapshot.diagnostics};
    if (snapshot.value->selected.empty()) return Invalid("WORLD_SELECTION_EMPTY", "没有可编辑的场景选择。");
    if (snapshot.value->selected.size() > 1
        && std::any_of(snapshot.value->selected.begin(), snapshot.value->selected.end(),
            [](const auto& id) { return !std::holds_alternative<MapData::PlacementId>(id); }))
        return Invalid("WORLD_SELECTION_MIXED", "辅助对象混选没有共同编辑事务，请单独选择区域或镜头。");
    std::array<bool, static_cast<std::size_t>(MapDataKind::Count)> checked{};
    for (const auto& id : snapshot.value->selected) {
        const auto flags = Flags(id);
        if (!flags.visible || flags.locked)
            return Invalid("WORLD_SELECTION_DISABLED", "隐藏或锁定对象不能编辑。");
        const auto kind = std::visit([](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, MapData::PlacementId>)
                return value.file == MapData::PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
            else if constexpr (std::is_same_v<T, MapData::RegionId>) return MapDataKind::Regions;
            else return MapDataKind::Cameras;
        }, id);
        if (checked[static_cast<std::size_t>(kind)]) continue;
        checked[static_cast<std::size_t>(kind)] = true;
        const auto& member = context.document->Member(kind);
        if (member.state != MapDataState::Ready) {
            auto status = Invalid("MAP_SOURCE_UNSAFE", member.memberName + " 来源不完整、损坏或身份歧义；可查看，不能编辑。");
            status.diagnostics.insert(status.diagnostics.end(), member.diagnostics.begin(), member.diagnostics.end());
            return status;
        }
    }
    return Status::Ok();
}
Result<WorldSelectionSnapshot> WorldSelection::Select(const WorldSelectionContext& context,
    const WorldSelectionVersion& expected, SelectionMode mode, std::span<const WorldObjectId> ids) {
    const auto status = Check(context, expected); if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    const auto before = Snapshot(context, expected); if (!before.Succeeded()) return before;
    if (ids.size() > MaxSelection || (mode != SelectionMode::Replace && mode != SelectionMode::Add && mode != SelectionMode::Subtract))
        return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_LIMIT", "选择操作无效或对象数超过4096。");
    for (const auto& id : ids) {
        if (!ValidId(*context.document, id)) return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_ID_STALE", "选择对象的完整身份与当前地图不符。");
        const auto flags = Flags(id);
        if (mode != SelectionMode::Subtract && (!flags.visible || flags.locked))
            return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_DISABLED", "隐藏或锁定对象不能加入编辑选择。");
    }
    auto next = mode == SelectionMode::Replace ? std::vector<WorldObjectId>{} : selected_;
    for (const auto& id : ids) {
        const auto found = std::find(next.begin(), next.end(), id);
        if (mode == SelectionMode::Subtract) { if (found != next.end()) next.erase(found); }
        else if (found == next.end()) next.push_back(id);
    }
    if (next.size() > MaxSelection) return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_LIMIT", "合并选择超过4096个对象。");
    std::optional<WorldObjectId> primary = primary_;
    if (mode != SelectionMode::Subtract && !ids.empty()) primary = ids.back();
    if (!primary || std::find(next.begin(), next.end(), *primary) == next.end()) primary = next.empty() ? std::nullopt : std::optional(next.back());
    if (next != selected_ || primary != primary_) {
        if (version_->editorRevision == std::numeric_limits<std::uint64_t>::max()) return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_REVISION", "编辑器选择修订已耗尽。");
        selected_ = std::move(next); primary_ = std::move(primary); ++version_->editorRevision;
    }
    return Snapshot(context, *version_);
}
Result<WorldSelectionSnapshot> WorldSelection::SetEditorFlags(const WorldSelectionContext& context,
    const WorldSelectionVersion& expected, std::span<const WorldEditorFlagEdit> edits) {
    const auto status = Check(context, expected); if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    const auto before = Snapshot(context, expected); if (!before.Succeeded()) return before;
    if (edits.size() > MaxSelection) return Failure<WorldSelectionSnapshot>("WORLD_FLAGS_LIMIT", "单批编辑器状态超过4096个对象。");
    for (const auto& edit : edits) if (!ValidId(*context.document, edit.id))
        return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_ID_STALE", "显隐/锁定对象的完整身份与当前地图不符。");
    auto next = flags_;
    for (const auto& edit : edits) {
        const auto found = std::find_if(next.begin(), next.end(), [&](const auto& f) { return f.id == edit.id; });
        if (edit.value == WorldEditorFlags{}) { if (found != next.end()) next.erase(found); }
        else if (found != next.end()) found->value = edit.value;
        else next.push_back(edit);
    }
    if (next.size() > MaxFlagOverrides) return Failure<WorldSelectionSnapshot>("WORLD_FLAGS_LIMIT", "编辑器显隐/锁定状态超过16384个对象。");
    const bool changed = next.size() != flags_.size() || !std::equal(next.begin(), next.end(), flags_.begin(),
        [](const auto& a, const auto& b) { return a.id == b.id && a.value == b.value; });
    if (changed) {
        if (version_->editorRevision == std::numeric_limits<std::uint64_t>::max()) return Failure<WorldSelectionSnapshot>("WORLD_SELECTION_REVISION", "编辑器选择修订已耗尽。");
        flags_ = std::move(next);
        std::erase_if(selected_, [&](const auto& id) { const auto f = Flags(id); return !f.visible || f.locked; });
        if (primary_ && std::find(selected_.begin(), selected_.end(), *primary_) == selected_.end())
            primary_ = selected_.empty() ? std::nullopt : std::optional(selected_.back());
        ++version_->editorRevision;
    }
    return Snapshot(context, *version_);
}
Result<WorldObjectPage> WorldSelection::QueryObjects(const WorldSelectionContext& context,
    const WorldSelectionVersion& expected, MapDataKind kind, const WorldObjectFilter& filter,
    const std::optional<WorldObjectCursor>& cursor, const WorldObjectQueryOptions& options) const {
    const auto status = Check(context, expected); if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    if (!Supported(kind) || !options.limit || options.limit > 128 || !options.maxExamined || options.maxExamined > 4096
        || !options.maxLogicalBytes || options.maxLogicalBytes > 256 * 1024 || filter.searchUtf8.size() > 512
        || !FuzzyCommandScore({}, filter.searchUtf8)
        || (filter.category && static_cast<unsigned>(*filter.category) > static_cast<unsigned>(WorldCategory::Unknown)))
        return Failure<WorldObjectPage>("WORLD_QUERY_OPTIONS", "类别、过滤文本或有界查询预算无效。");
    if (cursor && (cursor->version != expected || cursor->source.binding != expected.document
        || cursor->source.kind != kind || cursor->filter != filter))
        return Failure<WorldObjectPage>("WORLD_QUERY_CURSOR", "查询游标的文档、编辑器修订或筛选条件已改变。");
    const auto described = DescribeMapData(*context.mapData, context.binding);
    if (!described.Succeeded()) return {std::nullopt, described.diagnostics};
    WorldObjectPage page; page.version = expected; page.member = described.value->members[static_cast<std::size_t>(kind)];
    page.logicalBytes = 256 + expected.document.documentId.size() + expected.document.sourceSha256.size()
        + page.member.memberName.size() + page.member.memberSha256.value_or("").size() + filter.searchUtf8.size();
    if (page.logicalBytes > options.maxLogicalBytes) return Failure<WorldObjectPage>("WORLD_QUERY_BUDGET", "页身份及成员状态超出允许预算。");
    if (page.member.state != MapDataQueryState::Ready && page.member.state != MapDataQueryState::Partial
        && page.member.state != MapDataQueryState::Missing) {
        page.partial = true; page.reason = "地图成员不可可靠查询，不能将其显示为没有记录。";
        return Result<WorldObjectPage>::Success(std::move(page));
    }
    page.partial = page.member.state == MapDataQueryState::Partial;
    const ObjectLookup lookup(context, kind);
    std::optional<MapDataQueryCursor> source = cursor ? std::optional(cursor->source) : std::nullopt;
    while (page.examined < options.maxExamined && page.rows.size() < options.limit) {
        const auto count = (std::min)({std::size_t{64}, options.maxExamined - page.examined, options.limit - page.rows.size()});
        const auto input = QueryMapDataPage(*context.mapData, context.binding, kind, source, count);
        if (!input.Succeeded()) return {std::nullopt, input.diagnostics};
        if (input.value->rows.empty()) break;
        for (const auto& record : input.value->rows) {
            WorldObjectRow row; row.member = kind; row.sourceIndex = record.sourceIndex;
            row.rowKey = record.rowKey; row.name = record.name; row.owner = record.owner; row.positionWorld = record.positionWorld;
            row.id = RowId(*context.document, record, kind);
            lookup.Classify(row, record.rawcodeBytes);
            if (kind == MapDataKind::Units || kind == MapDataKind::Doodads) {
                row.name.rawBytes = record.rawcodeBytes;
                if (row.category == WorldCategory::StartLocation) {
                    row.name.displayBytes = "起始位置"; row.name.state = MapDataQueryTextState::Literal;
                } else if (row.typeState == WorldTypeState::Known) {
                    const auto type = row.category == WorldCategory::Unit ? ObjectData::ObjectType::Unit
                        : row.category == WorldCategory::Item ? ObjectData::ObjectType::Item
                        : row.category == WorldCategory::Doodad ? ObjectData::ObjectType::Doodad : ObjectData::ObjectType::Destructable;
                    if (context.objectNameReader) {
                        auto name = context.objectNameReader({type, record.rawcodeBytes});
                        if (name.Succeeded()) row.name = std::move(*name.value);
                        else { page.partial = true; row.reason += " 对象名称读取失败，保留原始ID。"; }
                    } else if (context.standardObjects) {
                        if (const auto* object = context.standardObjects->Find(type, record.rawcodeBytes)) {
                            row.name.displayBytes = object->displayName.substr(0, kTextBytes + 1);
                            row.name.state = object->nameResolved ? MapDataQueryTextState::Resolved : MapDataQueryTextState::CatalogUnavailable;
                        }
                    }
                }
            }
            if (!row.id) { row.reason = "记录尚无可可靠核对的编辑身份（特殊地物、超长名称或文档身份不一致）。"; page.partial = true; }
            if (row.typeState != WorldTypeState::Known) page.partial = true;
            bool truncated = false;
            BoundText(row.rowKey, truncated); BoundText(row.name.rawBytes, truncated);
            if (row.name.displayBytes) BoundText(*row.name.displayBytes, truncated);
            if (truncated) { page.partial = true; row.textTruncated = true; row.reason += " 文本超过2048字节，已截断展示。"; }
            if (row.id) {
                row.flags = Flags(*row.id);
                row.selected = std::find(selected_.begin(), selected_.end(), *row.id) != selected_.end();
                row.primary = primary_ == row.id;
            }
            const auto matches = (!filter.category || row.category == *filter.category)
                && (!filter.owner || row.owner == filter.owner)
                && (filter.searchUtf8.empty() || FuzzyCommandScore(filter.searchUtf8,
                    row.name.displayBytes.value_or(row.name.rawBytes) + " " + record.rawcodeBytes + " " + std::string(WorldCategoryName(row.category))));
            if (matches) {
                const auto bytes = RowBytes(row);
                if (bytes > options.maxLogicalBytes - page.logicalBytes) {
                    if (page.rows.empty()) return Failure<WorldObjectPage>("WORLD_QUERY_BUDGET", "单条世界记录超出允许页预算。");
                    page.next = WorldObjectCursor{expected, {context.binding, kind, record.sourceIndex}, filter};
                    return Result<WorldObjectPage>::Success(std::move(page));
                }
                page.logicalBytes += bytes; page.rows.push_back(std::move(row));
            }
            ++page.examined;
        }
        source = input.value->next;
        if (!source) break;
    }
    if (source) page.next = WorldObjectCursor{expected, *source, filter};
    if (page.examined == options.maxExamined && source) { page.partial = true; page.reason = "达到本次扫描预算，后续记录请继续分页。"; }
    return Result<WorldObjectPage>::Success(std::move(page));
}

} // namespace War3Map::Services

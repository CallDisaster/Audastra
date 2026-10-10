#include "War3MapCore/Services/ProjectChanges.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Services {
namespace {
using K = MapDataKind;
using V = MapData::MapFieldValue;
constexpr std::size_t kMaxMemberBytes = 16 * 1024 * 1024;

template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("Project.Changes", std::move(code), std::move(message));
}
template<class T> Result<T> Forward(const std::vector<Diagnostic>& diagnostics) {
    Result<T> result; result.diagnostics = diagnostics; return result;
}
bool Valid(const ProjectChangesContext& context, const MapDataQueryBinding& expected) {
    if (!context.baseline || !context.current || context.binding != expected
        || expected.documentId.empty() || expected.documentId.size() > 128 || !expected.generation
        || expected.sourceSha256.size() != 64) return false;
    return std::all_of(expected.sourceSha256.begin(), expected.sourceSha256.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Valid(const ProjectChangesOptions& options) {
    return options.limit && options.limit <= 128 && options.maxRecordsPerMember
        && options.maxRecordsPerMember <= 32768 && options.maxLogicalBytes
        && options.maxLogicalBytes <= 256 * 1024 && options.maxTextBytes && options.maxTextBytes <= 4096
        && options.maxContentNodes && options.maxContentNodes <= 1048576;
}
const std::vector<std::byte>* Raw(const MapMemberState& member) {
    return member.sources.size() == 1 ? &member.sources.front().data : nullptr;
}
std::size_t ValueBytes(const std::optional<V>& value) { return value ? 32 + value->bytes.size() : 0; }
std::size_t ObjectValueBytes(const ObjectData::Value& value) {
    return std::visit([](const auto& v) -> std::size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) return v.size();
        else if constexpr (std::is_same_v<T, ObjectData::RawcodeListValue>) {
            std::size_t n = 0; for (const auto& text : v.values) n += text.size() + sizeof(std::string); return n;
        } else if constexpr (std::is_same_v<T, ObjectData::UnknownBytesValue>) return v.bytes.size();
        else if constexpr (requires { v.value; }) return v.value.size();
        else return sizeof(v);
    }, value);
}
std::size_t RowBytes(const ProjectChangeRow& row) {
    return 256 + row.id.size() + row.rowKey.size() + row.fieldId.size() + row.label.size()
        + row.detail.size() + row.revertReason.size() + row.binding.documentId.size()
        + row.binding.sourceSha256.size() + (row.beforeMemberSha256 ? row.beforeMemberSha256->size() : 0)
        + (row.afterMemberSha256 ? row.afterMemberSha256->size() : 0)
        + (row.object ? row.object->rawcode.size() : 0) + (row.modification ? row.modification->fieldId.size() : 0)
        + ValueBytes(row.before) + ValueBytes(row.after) + row.changedArguments.size() * sizeof(StableId)
        + row.beforeText.size() + row.afterText.size()
        + (row.objectBefore ? ObjectValueBytes(*row.objectBefore) : 0)
        + (row.objectAfter ? ObjectValueBytes(*row.objectAfter) : 0);
}
void Partial(ProjectChangedMember& member, std::string_view reason) {
    member.partial = true;
    if (member.reason.find(reason) != std::string::npos) return;
    if (!member.reason.empty()) member.reason += "；";
    member.reason += reason;
}
struct Collector {
    ProjectChangesPage page;
    const ProjectChangesOptions& options;
    std::size_t offset = 0, seen = 0;
    std::string_view wanted;
    bool more = false, oversized = false;

    void Offer(ProjectChangeRow row) {
        row.binding = page.binding;
        row.beforeMemberSha256 = page.member.beforeSha256;
        row.afterMemberSha256 = page.member.afterSha256;
        row.editorRevision = page.editorRevision;
        const auto bound = [&](std::string& text) {
            if (text.size() > options.maxTextBytes) {
                text.resize(options.maxTextBytes); row.textTruncated = true;
            }
        };
        bound(row.label); bound(row.detail); bound(row.revertReason); bound(row.beforeText); bound(row.afterText);
        if (row.rowKey.size() > options.maxTextBytes || row.id.size() > options.maxTextBytes) row.textTruncated = true;
        for (auto* value : {&row.before, &row.after}) if (*value && (*value)->bytes.size() > options.maxTextBytes) {
            value->reset(); row.textTruncated = true;
        }
        if (row.textTruncated) {
            row.revertible = false; row.revertReason = "原始文本超过展示上限，不能用截断值还原";
            Partial(page.member, "有原始文本超过展示上限");
        }
        const auto index = seen++;
        if (!wanted.empty() && row.id != wanted) return;
        if (wanted.empty() && index < offset) return;
        const auto bytes = RowBytes(row);
        if (bytes > options.maxLogicalBytes - std::min(page.logicalBytes, options.maxLogicalBytes)) {
            more = true; oversized = page.rows.empty(); return;
        }
        if (page.rows.size() >= options.limit || more) { more = true; return; }
        page.logicalBytes += bytes; page.rows.push_back(std::move(row));
    }
};
ProjectChangeRow Row(K kind, ProjectChangeKind change, std::string key = {}) {
    ProjectChangeRow row; row.member = kind; row.kind = change; row.rowKey = std::move(key);
    row.id = std::string(MapData::MapDataKindId(kind)) + "/" + std::to_string(static_cast<unsigned>(change)) + "/" + row.rowKey;
    return row;
}
void MemberRow(Collector& output) {
    auto row = Row(output.page.member.kind, ProjectChangeKind::Member);
    row.label = "成员差异"; row.detail = output.page.member.reason;
    row.revertReason = "本项仅报告成员原字节变化，不提供整成员覆盖";
    output.Offer(std::move(row));
}
struct Keys {
    std::vector<std::string> ordered;
    std::unordered_map<std::string, std::size_t> byKey;
    std::unordered_map<std::int32_t, std::string> byCreation;
};
Result<Keys> ReadKeys(const MapDocument& document, K kind, std::size_t count, std::size_t textLimit) {
    Keys keys; keys.ordered.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto key = MapData::MapFieldRowKey(document, kind, i);
        if (!key.Succeeded()) return Forward<Keys>(key.diagnostics);
        if (key.value->size() > textLimit) return Fail<Keys>("CHANGE_ID_TEXT_LIMIT", "行身份原始文本超过上限");
        if (!keys.byKey.emplace(*key.value, i).second) return Fail<Keys>("CHANGE_ID_DUPLICATE", "行身份键重复，不能对齐或还原");
        std::optional<std::int32_t> creation;
        if (kind == K::Units) creation = document.Units()->units[i].creationId;
        if (kind == K::Doodads && i < document.Doodads()->doodads.size()) creation = document.Doodads()->doodads[i].creationId;
        if (creation && !keys.byCreation.emplace(*creation, *key.value).second)
            return Fail<Keys>("CHANGE_CREATION_DUPLICATE", "创建编号重复，不能确认记录身份");
        keys.ordered.push_back(std::move(*key.value));
    }
    return Result<Keys>::Success(std::move(keys));
}
bool Classic(const MapDocument& document, K kind) {
    if (kind == K::Units) return document.Units() && document.Units()->recordLayout == MapData::PlacementRecordLayout::Classic;
    if (kind == K::Doodads) return document.Doodads() && document.Doodads()->recordLayout == MapData::PlacementRecordLayout::Classic;
    if (kind == K::Settings) return document.Settings() && document.Settings()->HasClassicLayout();
    if (kind == K::Sounds) return document.Sounds() && document.Sounds()->version == 1;
    if (kind == K::Cameras) return document.Cameras() && document.Cameras()->layout == MapData::CameraLayout::Classic;
    return true;
}
Result<std::vector<std::byte>> Serialize(const MapDocument& document, K kind) {
    if (kind == K::Doodads && document.Doodads()) return document.Doodads()->Serialize();
    if (kind == K::Strings && document.Strings()) return document.Strings()->Serialize();
    return MapData::SerializeMapFieldTable(document, kind);
}
bool ExactSource(const MapDocument& document, K kind) {
    const auto* raw = Raw(document.Member(kind));
    auto serialized = Serialize(document, kind);
    return raw && serialized.Succeeded() && *serialized.value == *raw;
}
bool HasNestedBudget(const MapDocument& document, K kind) {
    std::size_t total = 0;
    const auto add = [&total](std::size_t count) { total += std::min(count, std::size_t(32769)); return total <= 32768; };
    const auto choices = [&add](const auto& row) {
        if (!add(row.itemSets.size())) return false;
        for (const auto& set : row.itemSets) if (!add(set.items.size())) return false;
        return true;
    };
    if (kind == K::Units && document.Units()) for (const auto& row : document.Units()->units)
        if (!choices(row) || !add(row.inventory.size()) || !add(row.abilities.size())
            || (std::holds_alternative<MapData::RandomCustomTable>(row.random)
                && !add(std::get<MapData::RandomCustomTable>(row.random).choices.size()))) return false;
    if (kind == K::Doodads && document.Doodads()) for (const auto& row : document.Doodads()->doodads)
        if (!choices(row)) return false;
    return true;
}
std::optional<std::size_t> RestoreIndex(const Keys& baseline, const Keys& current, std::size_t removed) {
    for (std::size_t i = removed + 1; i < baseline.ordered.size(); ++i)
        if (const auto found = current.byKey.find(baseline.ordered[i]); found != current.byKey.end()) return found->second;
    for (std::size_t i = removed; i > 0; --i)
        if (const auto found = current.byKey.find(baseline.ordered[i - 1]); found != current.byKey.end()) return found->second + 1;
    if (current.ordered.empty()) return 0;
    return std::nullopt;
}
bool IdentityChanged(const Keys& before, const Keys& after, std::string_view key) {
    for (const auto& [id, source] : before.byCreation) if (source == key) {
        const auto found = after.byCreation.find(id);
        return found != after.byCreation.end() && found->second != key;
    }
    return false;
}
void RecordRow(Collector& output, const ProjectChangesContext& context, const Keys& before, const Keys& after,
    bool added, std::size_t index, bool exact) {
    const auto kind = output.page.member.kind;
    const auto& keys = added ? after : before;
    auto row = Row(kind, added ? ProjectChangeKind::Added : ProjectChangeKind::Removed, keys.ordered[index]);
    (added ? row.currentIndex : row.baselineIndex) = index;
    row.label = added ? "新增记录" : "删除记录"; row.detail = row.rowKey;
    const auto& document = added ? *context.current : *context.baseline;
    bool normal = kind == K::Units || (kind == K::Doodads && index < document.Doodads()->doodads.size());
    const bool changedIdentity = IdentityChanged(added ? after : before, added ? before : after, row.rowKey);
    const bool start = kind == K::Units && document.Units()->units[index].rawcode == "sloc";
    row.revertible = exact && normal && !changedIdentity && !start && (added || RestoreIndex(before, after, index).has_value());
    if (!exact) row.revertReason = "序列化器不能证明完整原字节往返，不能恢复完整记录";
    else if (!normal) row.revertReason = "当前阶段只还原单位与普通地物的完整记录";
    else if (changedIdentity) row.revertReason = "同一创建编号的类型身份改变，不能猜测是新增或删除";
    else if (start) row.revertReason = "起始位置与玩家设置联动，不能单独还原这条记录";
    else if (!row.revertible) row.revertReason = "缺少唯一邻接记录，不能确定原插入位置";
    if (!row.revertible) Partial(output.page.member, "有记录不能可靠反向恢复");
    output.Offer(std::move(row));
}
bool RevertFieldAllowed(const MapDocument& current, K kind, std::size_t row,
    const MapData::MapFieldDescriptor& field, const V& target) {
    if (!MapData::MapFieldRowAllowsWrite(current, row, field)
        || !MapData::ValidateMapFieldValue(kind, field.id, target).Succeeded()) return false;
    if (kind == K::Regions) {
        auto bounds = current.Regions()->entries[row];
        const auto value = MapData::MapFieldNumber(target); if (!value) return false;
        if (field.id == "left") bounds.left = static_cast<float>(*value);
        else if (field.id == "bottom") bounds.bottom = static_cast<float>(*value);
        else if (field.id == "right") bounds.right = static_cast<float>(*value);
        else if (field.id == "top") bounds.top = static_cast<float>(*value);
        return MapData::ValidateMapFieldRegionBounds(bounds.left, bounds.bottom, bounds.right, bounds.top).Succeeded();
    }
    return true;
}
// 将所有已展示的可写字段与增删还原到基线，比较完整原字节。不能解释的余量留成员提示。
bool Explained(const ProjectChangesContext& context, K kind, const Keys& before, const Keys& after,
    const std::vector<MapData::MapFieldEdit>& inverse, bool fieldProof, bool recordsChanged) {
    if (!fieldProof || inverse.size() > MapData::kMaxMapFieldEdits) return false;
    const auto* baselineBytes = Raw(context.baseline->Member(kind));
    auto currentBytes = Serialize(*context.current, kind); if (!baselineBytes || !currentBytes.Succeeded()) return false;
    if (kind == K::Units || kind == K::Doodads) {
        std::vector<std::string> a, b;
        for (const auto& key : before.ordered) if (after.byKey.contains(key)) a.push_back(key);
        for (const auto& key : after.ordered) if (before.byKey.contains(key)) b.push_back(key);
        if (a != b) return false; // 独立的记录重排不能被按基线排列的归一化掩盖。
        if (kind == K::Units) {
            auto table = *context.current->Units(); table.units.clear();
            for (std::size_t i = 0; i < before.ordered.size(); ++i) {
                const auto found = after.byKey.find(before.ordered[i]);
                table.units.push_back(found == after.byKey.end() ? context.baseline->Units()->units[i]
                    : context.current->Units()->units[found->second]);
            }
            currentBytes = table.Serialize();
        } else {
            // specialDoodads 不参与普通记录归一化，完整保留当前值用于余量检测。
            if (context.baseline->Doodads()->specialDoodads.size() || context.current->Doodads()->specialDoodads.size()) return false;
            auto table = *context.current->Doodads(); table.doodads.clear();
            for (std::size_t i = 0; i < before.ordered.size(); ++i) {
                const auto found = after.byKey.find(before.ordered[i]);
                table.doodads.push_back(found == after.byKey.end() ? context.baseline->Doodads()->doodads[i]
                    : context.current->Doodads()->doodads[found->second]);
            }
            currentBytes = table.Serialize();
        }
    } else if (recordsChanged) return false;
    if (!currentBytes.Succeeded()) return false;
    if (inverse.empty()) return *currentBytes.value == *baselineBytes;
    std::vector<MapArchiveMember> members{{context.current->Member(kind).memberName, std::move(*currentBytes.value)}};
    auto normalized = MapDocument::FromMembers(members);
    auto edits = inverse;
    if (kind == K::Units) for (auto& edit : edits) edit.row = before.byKey.at(edit.expectedRowKey);
    auto applied = MapData::ApplyMapFieldEdits(normalized, edits); if (!applied.Succeeded()) return false;
    auto result = MapData::SerializeMapFieldTable(*applied.value, kind);
    return result.Succeeded() && *result.value == *baselineBytes;
}

Result<ProjectChangesPage> Analyze(const ProjectChangesContext& context, const MapDataQueryBinding& expected,
    K kind, const std::optional<ProjectChangesCursor>& cursor, const ProjectChangesOptions& options,
    std::string_view wanted = {}) {
    if (!Valid(context, expected)) return Fail<ProjectChangesPage>("CHANGES_REVISION_STALE", "工程或当前地图修订已经变化，请刷新修改列表");
    if (static_cast<std::size_t>(kind) >= static_cast<std::size_t>(K::Count) || !Valid(options))
        return Fail<ProjectChangesPage>("CHANGES_QUERY_LIMIT", "成员类别或分页/扫描上限无效");
    Collector output{{}, options}; output.page.binding = expected; output.wanted = wanted;
    auto& member = output.page.member; member.kind = kind;
    const auto& baseline = context.baseline->Member(kind); const auto& current = context.current->Member(kind);
    member.memberName = current.memberName.empty() ? baseline.memberName : current.memberName;
    member.beforeState = baseline.state; member.afterState = current.state;
    member.beforeCount = baseline.recordCount; member.afterCount = current.recordCount;
    const auto hash = [&](const MapMemberState& state, std::optional<std::string>& target) -> Status {
        if (const auto* bytes = Raw(state)) {
            auto result = Sha256(*bytes); if (!result.Succeeded()) return Status{result.diagnostics};
            target = std::move(*result.value);
        }
        return Status::Ok();
    };
    if (auto status = hash(baseline, member.beforeSha256); !status.Succeeded()) return Forward<ProjectChangesPage>(status.diagnostics);
    if (auto status = hash(current, member.afterSha256); !status.Succeeded()) return Forward<ProjectChangesPage>(status.diagnostics);
    member.changed = member.beforeSha256 != member.afterSha256 || baseline.state != current.state;
    if (cursor) {
        if (cursor->binding != expected || cursor->member != kind || cursor->beforeSha256 != member.beforeSha256
            || cursor->afterSha256 != member.afterSha256 || cursor->nextOffset > 4 * 1024 * 1024)
            return Fail<ProjectChangesPage>("CHANGES_CURSOR_STALE", "分页游标与当前成员或修订不符");
        output.offset = cursor->nextOffset;
    }
    output.page.logicalBytes = 512 + expected.documentId.size() + member.memberName.size();
    if (output.page.logicalBytes > options.maxLogicalBytes)
        return Fail<ProjectChangesPage>("CHANGES_PAGE_BYTES", "单页字节上限不足以容纳成员身份元数据");
    if (baseline.state != MapDataState::Ready || current.state != MapDataState::Ready) {
        if (!(baseline.state == MapDataState::Missing && current.state == MapDataState::Missing)) {
            Partial(member, "成员缺失、部分、损坏或来源不唯一，不能确认完整差异");
            member.changed = member.changed || !member.beforeSha256 || !member.afterSha256;
        }
    } else if (!Classic(*context.baseline, kind) || !Classic(*context.current, kind)) {
        Partial(member, "记录布局只读或未确定，不提供反向写入");
    } else if (!Raw(baseline) || !Raw(current) || Raw(baseline)->size() > kMaxMemberBytes || Raw(current)->size() > kMaxMemberBytes) {
        Partial(member, "成员来源不唯一或超过16 MiB展开上限");
    } else if (member.changed) {
        auto countBefore = MapData::MapFieldRowCount(*context.baseline, kind);
        auto countAfter = MapData::MapFieldRowCount(*context.current, kind);
        if (!countBefore.Succeeded() || !countAfter.Succeeded()) Partial(member, "字段目录不能完整读取此成员");
        else if (*countBefore.value > options.maxRecordsPerMember || *countAfter.value > options.maxRecordsPerMember)
            Partial(member, "记录超过扫描上限，未扫描其余记录，不能假定身份唯一");
        else if (!HasNestedBudget(*context.baseline, kind) || !HasNestedBudget(*context.current, kind))
            Partial(member, "嵌套记录超过32768项展开上限");
        else {
            auto keysBefore = ReadKeys(*context.baseline, kind, *countBefore.value, options.maxTextBytes);
            auto keysAfter = ReadKeys(*context.current, kind, *countAfter.value, options.maxTextBytes);
            output.page.examinedBefore = *countBefore.value; output.page.examinedAfter = *countAfter.value;
            if (!keysBefore.Succeeded() || !keysAfter.Succeeded()) Partial(member, "身份重复、无效或过长，记录不能可靠对齐");
            else {
                const auto& a = *keysBefore.value; const auto& b = *keysAfter.value;
                bool proof = ExactSource(*context.baseline, kind) && ExactSource(*context.current, kind);
                const bool exact = proof;
                bool recordsChanged = false; std::vector<MapData::MapFieldEdit> inverse;
                for (std::size_t i = 0; i < a.ordered.size(); ++i) {
                    const auto found = b.byKey.find(a.ordered[i]);
                    if (found == b.byKey.end()) {
                        recordsChanged = true; RecordRow(output, context, a, b, false, i, exact); continue;
                    }
                    const auto j = found->second;
                    for (const auto& field : MapData::MapFieldCatalog(kind)) {
                        auto old = MapData::ReadMapField(*context.baseline, kind, i, field);
                        auto now = MapData::ReadMapField(*context.current, kind, j, field);
                        if (!old.Succeeded() || !now.Succeeded()) { proof = false; continue; }
                        if (*old.value == *now.value) continue;
                        auto row = Row(kind, ProjectChangeKind::Field, a.ordered[i]); row.id += "/" + std::string(field.id);
                        row.baselineIndex = i; row.currentIndex = j; row.fieldId = field.id; row.label = field.label;
                        row.before = std::move(*old.value); row.after = std::move(*now.value);
                        row.revertible = exact && RevertFieldAllowed(*context.current, kind, j, field, *row.before);
                        if (!row.revertible) row.revertReason = "字段只读、原值不满足现有约束或需关联字段一起还原";
                        if (field.writable && inverse.size() <= MapData::kMaxMapFieldEdits)
                            inverse.push_back({kind, j, a.ordered[i], std::string(field.id), *row.after, *row.before});
                        else proof = false;
                        output.Offer(std::move(row));
                    }
                }
                for (std::size_t j = 0; j < b.ordered.size(); ++j) if (!a.byKey.contains(b.ordered[j])) {
                    recordsChanged = true; RecordRow(output, context, a, b, true, j, exact);
                }
                if (!Explained(context, kind, a, b, inverse, proof, recordsChanged))
                    Partial(member, "原字节还包含目录未解释的内容、身份/顺序变化或不可写字段");
                if (!exact) Partial(member, "现有序列化器不能证明完整原字节往返");
            }
        }
    }
    if (member.changed && member.partial) MemberRow(output);
    output.page.totalRows = output.seen;
    output.page.logicalBytes += member.reason.size();
    if (output.page.logicalBytes > options.maxLogicalBytes || output.oversized)
        return Fail<ProjectChangesPage>("CHANGES_PAGE_BYTES", "单项差异或成员说明超过单页字节上限，请提高上限");
    if (output.more && wanted.empty()) output.page.next = ProjectChangesCursor{expected, kind,
        member.beforeSha256, member.afterSha256, output.offset + output.page.rows.size()};
    return Result<ProjectChangesPage>::Success(std::move(output.page));
}
} // namespace

Result<ProjectChangesPage> QueryProjectChanges(const ProjectChangesContext& context,
    const MapDataQueryBinding& expected, MapDataKind member, const std::optional<ProjectChangesCursor>& cursor,
    const ProjectChangesOptions& options) {
    try { return Analyze(context, expected, member, cursor, options); }
    catch (const std::exception&) { return Fail<ProjectChangesPage>("CHANGES_QUERY_FAILED", "计算差异时发生异常；没有修改地图"); }
}

Result<ProjectChangeReversePlan> BuildProjectChangeReversePlan(const ProjectChangesContext& context,
    const ProjectChangeRow& selected, const ProjectChangesOptions& options) {
    try {
        if (selected.domain != ProjectChangeDomain::MapData)
            return Fail<ProjectChangeReversePlan>("CHANGE_DOMAIN_MISMATCH", "此入口只还原地图数据成员");
        if (selected.id.empty() || selected.id.size() > 8192)
            return Fail<ProjectChangeReversePlan>("CHANGE_SELECTION_INVALID", "所选差异身份无效");
        auto queried = Analyze(context, selected.binding, selected.member, std::nullopt, options, selected.id);
        if (!queried.Succeeded()) return Forward<ProjectChangeReversePlan>(queried.diagnostics);
        if (selected.beforeMemberSha256 != queried.value->member.beforeSha256
            || selected.afterMemberSha256 != queried.value->member.afterSha256)
            return Fail<ProjectChangeReversePlan>("CHANGE_MEMBER_STALE", "基线或当前成员已变化，请刷新后再还原");
        if (queried.value->rows.size() != 1 || !queried.value->rows.front().revertible)
            return Fail<ProjectChangeReversePlan>("CHANGE_REVERT_UNAVAILABLE", "所选差异不能可靠反向恢复；没有修改地图");
        const auto& row = queried.value->rows.front();
        ProjectChangeReversePlan plan; plan.binding = context.binding; plan.member = selected.member; plan.changeId = row.id;
        plan.beforeMemberSha256 = row.beforeMemberSha256; plan.afterMemberSha256 = row.afterMemberSha256;
        if (row.kind == ProjectChangeKind::Field) {
            MapData::MapFieldEdit edit{row.member, *row.currentIndex, row.rowKey, row.fieldId, *row.after, *row.before};
            auto applied = MapData::ApplyMapFieldEdits(*context.current, {&edit, 1});
            if (!applied.Succeeded()) return Forward<ProjectChangeReversePlan>(applied.diagnostics);
            auto serialized = MapData::SerializeMapFieldTable(*applied.value, row.member);
            if (!serialized.Succeeded()) return Forward<ProjectChangeReversePlan>(serialized.diagnostics);
            plan.fieldTables = std::move(*applied.value);
        } else {
            auto countA = MapData::MapFieldRowCount(*context.baseline, row.member);
            auto countB = MapData::MapFieldRowCount(*context.current, row.member);
            auto a = ReadKeys(*context.baseline, row.member, *countA.value, options.maxTextBytes);
            auto b = ReadKeys(*context.current, row.member, *countB.value, options.maxTextBytes);
            if (!a.Succeeded() || !b.Succeeded()) return Fail<ProjectChangeReversePlan>("CHANGE_ID_STALE", "记录身份不能确认");
            const auto restore = [&](auto& records, const auto& baselineRecords) {
                if (row.kind == ProjectChangeKind::Added) records.erase(records.begin() + *row.currentIndex);
                else records.insert(records.begin() + *RestoreIndex(*a.value, *b.value, *row.baselineIndex),
                    baselineRecords[*row.baselineIndex]);
            };
            if (row.member == K::Units) {
                plan.units = *context.current->Units(); restore(plan.units->units, context.baseline->Units()->units);
                auto serialized = plan.units->Serialize();
                if (!serialized.Succeeded()) return Forward<ProjectChangeReversePlan>(serialized.diagnostics);
            } else if (row.member == K::Doodads) {
                plan.doodads = *context.current->Doodads(); restore(plan.doodads->doodads, context.baseline->Doodads()->doodads);
                auto serialized = plan.doodads->Serialize();
                if (!serialized.Succeeded()) return Forward<ProjectChangeReversePlan>(serialized.diagnostics);
            } else return Fail<ProjectChangeReversePlan>("CHANGE_RECORD_UNSUPPORTED", "此类别没有记录级反向编辑能力");
        }
        return Result<ProjectChangeReversePlan>::Success(std::move(plan));
    } catch (const std::exception&) {
        return Fail<ProjectChangeReversePlan>("CHANGE_REVERSE_FAILED", "构造反向计划时发生异常；没有修改地图");
    }
}
namespace {
namespace OD = ObjectData;
namespace TD = Trigger;
using Domain = ProjectChangeDomain;
std::string DomainId(Domain domain) {
    return domain == Domain::Objects ? "objects" : domain == Domain::Triggers ? "triggers" : "variables";
}
bool Valid(const ProjectContentChangesContext& context, const MapDataQueryBinding& expected) {
    return context.binding == expected && !expected.documentId.empty() && expected.documentId.size() <= 128
        && expected.generation && expected.sourceSha256.size() == 64
        && std::all_of(expected.sourceSha256.begin(), expected.sourceSha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
ProjectChangeRow ContentRow(const Collector& output, Domain domain, ProjectChangeKind kind, std::string key) {
    auto row = Row(K::Settings, kind, std::move(key)); row.domain = domain;
    row.id = DomainId(domain) + "/" + output.page.member.memberName + "/"
        + std::to_string(output.page.editorRevision) + "/" + std::to_string(static_cast<unsigned>(kind))
        + "/" + row.rowKey;
    return row;
}
void ContentMemberRow(Collector& output, Domain domain) {
    auto row = ContentRow(output, domain, ProjectChangeKind::Member, {});
    row.label = "未展开的成员差异"; row.detail = output.page.member.reason;
    row.revertReason = "不能以完整成员覆盖代替单项撤销；请在所属模块或普通撤销中处理";
    output.Offer(std::move(row));
}
std::string ModId(const OD::ModificationKey& key) {
    const auto index = [](std::optional<std::uint32_t> v) { return v ? std::to_string(*v) : "-"; };
    return std::to_string(key.fieldId.size()) + ":" + key.fieldId + "/" + index(key.level)
        + "/" + index(key.dataIndex) + "/" + index(key.variation);
}
bool EqualValue(const OD::Value& a, const OD::Value& b) {
    if (a.index() != b.index()) return false;
    if (const auto* number = std::get_if<float>(&a))
        return std::bit_cast<std::uint32_t>(*number) == std::bit_cast<std::uint32_t>(std::get<float>(b));
    return a == b;
}
using ObjectIndex = std::unordered_map<std::string, const OD::ObjectRecord*>;
bool ObjectBudget(const OD::LniObjectDocument* doc, std::size_t cap) {
    if (!doc) return true;
    std::size_t total = doc->Objects().size(); if (total > cap) return false;
    for (const auto& object : doc->Objects()) {
        if (object.modifications.size() > cap - total) return false;
        total += object.modifications.size();
    }
    return true;
}
bool IndexObjects(const OD::LniObjectDocument* doc, OD::ObjectType type, ObjectIndex& out, std::size_t textLimit) {
    if (!doc) return true;
    for (const auto& object : doc->Objects()) {
        if (object.key.type != type || object.key.rawcode.size() != 4 || !out.emplace(object.key.rawcode, &object).second)
            return false;
        std::unordered_set<std::string> fields;
        for (const auto& mod : object.modifications)
            if (mod.key.fieldId.size() > textLimit || !fields.emplace(ModId(mod.key)).second) return false;
    }
    return true;
}
void ObjectFieldRow(Collector& out, const ProjectContentChangesContext& context, OD::ObjectType type,
    const OD::ObjectRecord& object, const OD::ObjectModification* a, const OD::ObjectModification* b,
    bool editable) {
    const auto& key = b ? b->key : a->key;
    auto row = ContentRow(out, Domain::Objects, ProjectChangeKind::Field, object.key.rawcode);
    row.id += "/" + ModId(key); row.object = object.key; row.modification = key; row.fieldId = key.fieldId;
    const OD::ObjectFieldDefinition* definition = nullptr;
    if (context.schema) {
        OD::ObjectFieldContext fieldContext; fieldContext.rawcode = object.parentRawcode.value_or(object.key.rawcode);
        auto match = context.schema->MatchSourceField(type, key.fieldId, fieldContext);
        definition = match.definition ? match.definition : context.schema->FindNative(type, key.fieldId);
    }
    row.label = definition && !definition->displayName.empty() ? definition->displayName : key.fieldId;
    if (key.level) row.label += " · 等级 " + std::to_string(*key.level);
    if (key.dataIndex) row.label += " · 数据 " + std::to_string(*key.dataIndex);
    if (key.variation) row.label += " · 变体 " + std::to_string(*key.variation);
    row.detail = "地图覆盖字段；无覆盖时继承原版或父对象，不虚构继承值";
    row.revertible = editable && !(a && a->valueWasOpaque) && !(b && b->valueWasOpaque);
    for (const auto& item : {std::pair{a, &row.objectBefore}, std::pair{b, &row.objectAfter}}) if (item.first) {
        if (ObjectValueBytes(item.first->value) > out.options.maxTextBytes) row.textTruncated = true;
        else *item.second = item.first->value;
    }
    if (!row.revertible) row.revertReason = "对象身份/父链已变化，文档只读或原值不透明，不能单独恢复";
    out.Offer(std::move(row));
}
void AnalyzeObjects(Collector& out, const ProjectContentChangesContext& context, OD::ObjectType type) {
    const auto& source = context.objects[static_cast<std::size_t>(type)]; auto& member = out.page.member;
    if (!source.baselineKnown || !source.currentKnown) {
        member.changed = true; Partial(member, "物编来源缺失、未解析或基线未确定，不能确认空表"); return;
    }
    if (!member.changed) return;
    if (!ObjectBudget(source.baseline, out.options.maxContentNodes)
        || !ObjectBudget(source.current, out.options.maxContentNodes)) {
        Partial(member, "对象与字段总数超过有界扫描预算，未扫描记录不能假定唯一"); return;
    }
    ObjectIndex before, after;
    if (!IndexObjects(source.baseline, type, before, out.options.maxTextBytes)
        || !IndexObjects(source.current, type, after, out.options.maxTextBytes)) {
        Partial(member, "对象或字段身份重复/无效，不能可靠对齐"); return;
    }
    out.page.examinedBefore = before.size(); out.page.examinedAfter = after.size();
    member.beforeCount = before.size(); member.afterCount = after.size();
    const auto changedObject = [&](const OD::ObjectRecord& object, const OD::ObjectRecord* old) {
        const bool editable = source.current && source.current->IsStructurallySafe()
            && (!source.baseline || source.baseline->IsStructurallySafe())
            && (!old || old->parentRawcode == object.parentRawcode);
        if ((!old && object.parentRawcode) || (old && old->parentRawcode != object.parentRawcode)) {
            auto row = ContentRow(out, Domain::Objects, old ? ProjectChangeKind::Member : ProjectChangeKind::Added, object.key.rawcode);
            row.object = object.key; row.label = old ? "对象继承身份改变" : "新增自定义对象";
            row.detail = object.parentRawcode.value_or("无父对象");
            row.revertReason = "创建/父链变更涉及对象引用，不提供未经审计的删除或整对象覆盖"; out.Offer(std::move(row));
        }
        std::unordered_map<std::string, const OD::ObjectModification*> a, b;
        if (old) for (const auto& mod : old->modifications) a.emplace(ModId(mod.key), &mod);
        for (const auto& mod : object.modifications) b.emplace(ModId(mod.key), &mod);
        if (old) for (const auto& mod : old->modifications) {
            const auto found = b.find(ModId(mod.key)); const auto* now = found == b.end() ? nullptr : found->second;
            if (!now || !EqualValue(mod.value, now->value)) ObjectFieldRow(out, context, type, object, &mod, now, editable);
        }
        for (const auto& mod : object.modifications) if (!a.contains(ModId(mod.key)))
            ObjectFieldRow(out, context, type, object, nullptr, &mod, editable);
    };
    if (source.baseline) for (const auto& old : source.baseline->Objects()) {
        const auto found = after.find(old.key.rawcode);
        if (found == after.end()) {
            auto row = ContentRow(out, Domain::Objects, ProjectChangeKind::Removed, old.key.rawcode);
            row.object = old.key; row.label = "删除对象记录";
            row.revertReason = "对象删除需完整引用和身份审计，本项只读"; out.Offer(std::move(row));
        } else changedObject(*found->second, &old);
    }
    if (source.current) for (const auto& object : source.current->Objects()) if (!before.contains(object.key.rawcode))
        changedObject(object, nullptr);
    Partial(member, "字段明细不包含LNI注释、排版及未公开原文；保留完整成员指纹提示");
}
bool TextEqual(const TD::RawText& a, const TD::RawText& b) {
    return a.text == b.text && a.encoding == b.encoding && a.originalBytes == b.originalBytes;
}
template<class T> bool UniqueIds(const std::vector<T>& list) {
    std::unordered_set<StableId> ids;
    for (const auto& row : list) if (!row.id || !ids.emplace(row.id).second) return false;
    return true;
}
bool TriggerBudget(const TD::TriggerDocument* doc, std::size_t cap) {
    if (!doc) return true;
    std::size_t total = 0;
    for (const auto count : {doc->categories.size(), doc->variables.size(), doc->triggers.size(),
        doc->ecaNodes.size(), doc->argumentNodes.size()}) {
        if (count > cap - total) return false; total += count;
    }
    // 边数也有界，避免少量节点引用无界重复列表。
    std::size_t edges = 0;
    const auto add = [&](std::size_t count) { if (count > cap - edges) return false; edges += count; return true; };
    for (const auto& trigger : doc->triggers) if (!add(trigger.ecas.size())) return false;
    for (const auto& eca : doc->ecaNodes) if (!add(eca.arguments.size()) || !add(eca.children.size())) return false;
    return UniqueIds(doc->categories) && UniqueIds(doc->variables) && UniqueIds(doc->triggers)
        && UniqueIds(doc->ecaNodes) && UniqueIds(doc->argumentNodes);
}
struct TriggerNodes {
    std::unordered_map<StableId, const TD::EcaNode*> ecas;
    std::unordered_map<StableId, const TD::ArgumentNode*> arguments;
    bool valid = true;
};
TriggerNodes ReadTriggerNodes(const TD::TriggerDocument& doc, const TD::TriggerRecord& trigger, std::size_t& budget) {
    TriggerNodes nodes; std::vector<std::pair<bool, std::uint32_t>> pending;
    for (const auto index : trigger.ecas) pending.emplace_back(false, index);
    while (!pending.empty() && nodes.valid) {
        if (!budget) { nodes.valid = false; break; } --budget;
        const auto [argument, index] = pending.back(); pending.pop_back();
        if (argument) {
            if (index >= doc.argumentNodes.size()) { nodes.valid = false; break; }
            const auto& row = doc.argumentNodes[index];
            if (!nodes.arguments.emplace(row.id, &row).second) { nodes.valid = false; break; }
            if (row.callEca) pending.emplace_back(false, *row.callEca);
            if (row.indexArgument) pending.emplace_back(true, *row.indexArgument);
        } else {
            if (index >= doc.ecaNodes.size()) { nodes.valid = false; break; }
            const auto& row = doc.ecaNodes[index];
            if (!nodes.ecas.emplace(row.id, &row).second) { nodes.valid = false; break; }
            for (const auto child : row.children) pending.emplace_back(false, child);
            for (const auto child : row.arguments) pending.emplace_back(true, child);
        }
    }
    return nodes;
}
bool TriggerOwnersUnique(const TD::TriggerDocument& doc, std::size_t cap) {
    std::unordered_set<StableId> ecas, arguments; std::size_t work = cap * 2;
    for (const auto& trigger : doc.triggers) {
        const auto nodes = ReadTriggerNodes(doc, trigger, work); if (!nodes.valid) return false;
        for (const auto& [id, node] : nodes.ecas) { (void)node; if (!ecas.emplace(id).second) return false; }
        for (const auto& [id, node] : nodes.arguments) { (void)node; if (!arguments.emplace(id).second) return false; }
    }
    return true;
}
bool TriggerPropertiesEqual(const TD::TriggerRecord& a, const TD::TriggerRecord& b) {
    return TextEqual(a.name, b.name) && TextEqual(a.description, b.description) && a.type == b.type && a.enabled == b.enabled
        && a.customTextFlag == b.customTextFlag && a.initiallyClosed == b.initiallyClosed && a.runOnInit == b.runOnInit
        && a.categoryId == b.categoryId && a.ecas == b.ecas && TextEqual(a.customText, b.customText);
}
bool EcaEqual(const TD::EcaNode& a, const TD::EcaNode& b) {
    return a.type == b.type && a.childId == b.childId && TextEqual(a.name, b.name) && a.enabled == b.enabled
        && a.arguments == b.arguments && a.children == b.children && a.uiSource == b.uiSource
        && a.uiLayer == b.uiLayer && a.uiSignature == b.uiSignature;
}
bool ArgumentShapeEqual(const TD::ArgumentNode& a, const TD::ArgumentNode& b) {
    return a.insertCall == b.insertCall && a.callEca == b.callEca
        && a.insertIndex == b.insertIndex && a.indexArgument == b.indexArgument;
}
void TriggerRow(Collector& out, const ProjectContentChangesContext& context,
    const TD::TriggerRecord* before, const TD::TriggerRecord* after, std::size_t& work) {
    const auto& record = after ? *after : *before;
    auto row = ContentRow(out, Domain::Triggers, before && after ? ProjectChangeKind::Field
        : after ? ProjectChangeKind::Added : ProjectChangeKind::Removed, std::to_string(record.id));
    row.entityId = record.id; row.label = record.name.text.substr(0, out.options.maxTextBytes);
    row.beforeText = before ? before->name.text.substr(0, out.options.maxTextBytes) : "不存在";
    row.afterText = after ? after->name.text.substr(0, out.options.maxTextBytes) : "已删除";
    bool scalar = before && after && TriggerPropertiesEqual(*before, *after);
    std::size_t count = scalar ? 0 : 1;
    if (before && after) {
        const auto a = ReadTriggerNodes(*context.baselineTriggers, *before, work);
        const auto b = ReadTriggerNodes(*context.currentTriggers, *after, work);
        if (!a.valid || !b.valid) {
            scalar = false; ++count; Partial(out.page.member, "触发器引用无效、重复/循环或超过遍历预算");
        } else {
            for (const auto& [id, old] : a.ecas) {
                const auto found = b.ecas.find(id);
                if (found == b.ecas.end() || !EcaEqual(*old, *found->second)) { ++count; scalar = false; }
            }
            for (const auto& [id, now] : b.ecas) if (!a.ecas.contains(id)) { (void)now; ++count; scalar = false; }
            for (const auto& [id, old] : a.arguments) {
                const auto found = b.arguments.find(id);
                if (found == b.arguments.end()) { ++count; scalar = false; continue; }
                const auto& now = *found->second;
                if (!ArgumentShapeEqual(*old, now)) { ++count; scalar = false; }
                else if (old->type != now.type || !TextEqual(old->value, now.value)) {
                    ++count; row.changedArguments.push_back(id);
                    if (old->insertCall || old->insertIndex || old->value.text.size() > out.options.maxTextBytes
                        || now.value.text.size() > out.options.maxTextBytes
                        || old->value.originalBytes.size() > out.options.maxTextBytes
                        || now.value.originalBytes.size() > out.options.maxTextBytes
                        || (old->type != 0 && old->type != 1 && old->type != 3)
                        || (now.type != 0 && now.type != 1 && now.type != 3)) scalar = false;
                }
            }
            for (const auto& [id, now] : b.arguments) if (!a.arguments.contains(id)) { (void)now; ++count; scalar = false; }
        }
    }
    if (!count) return;
    std::sort(row.changedArguments.begin(), row.changedArguments.end());
    row.changedCount = count; row.detail = "此触发器有 " + std::to_string(count) + " 处节点、参数或属性变化";
    if (!row.changedArguments.empty()) {
        row.beforeText.clear(); row.afterText.clear();
        for (const auto id : row.changedArguments) {
            const auto append = [&](std::string& text, const auto& arguments) {
                const auto found = std::find_if(arguments.begin(), arguments.end(), [id](const auto& node) { return node.id == id; });
                if (found == arguments.end()) return;
                if (!text.empty()) text += "\n";
                text += "参数 " + std::to_string(id) + "：" + found->value.text;
            };
            append(row.beforeText, context.baselineTriggers->argumentNodes);
            append(row.afterText, context.currentTriggers->argumentNodes);
            if (row.beforeText.size() > out.options.maxTextBytes || row.afterText.size() > out.options.maxTextBytes) {
                row.textTruncated = true; break;
            }
        }
    }
    row.revertible = scalar && !row.changedArguments.empty() && context.currentTriggers->HasTrustedSourceSeal()
        && context.baselineTriggers->HasTrustedSourceSeal()
        && context.currentTriggers->UiCatalogSha256() == context.baselineTriggers->UiCatalogSha256();
    if (!row.revertible) row.revertReason = "结构、代码、属性或来源变化只能在触发器模块/普通撤销中处理";
    if (row.changedArguments.size() > out.options.maxTextBytes / sizeof(StableId)) {
        row.changedArguments.clear(); row.textTruncated = true;
    }
    out.Offer(std::move(row));
}
bool VariableEqual(const TD::VariableRecord& a, const TD::VariableRecord& b) {
    return TextEqual(a.name, b.name) && TextEqual(a.typeName, b.typeName) && TextEqual(a.value, b.value)
        && a.unknown == b.unknown && a.isArray == b.isArray && a.size == b.size && a.hasDefault == b.hasDefault;
}
void AnalyzeTriggers(Collector& out, const ProjectContentChangesContext& context, Domain domain) {
    auto& member = out.page.member;
    if (!context.baselineTriggersKnown || !context.currentTriggersKnown) {
        member.changed = true; Partial(member, "触发器/变量基线或当前文档未解析，不能确认没有修改"); return;
    }
    if (!member.changed) return;
    if (!TriggerBudget(context.baselineTriggers, out.options.maxContentNodes)
        || !TriggerBudget(context.currentTriggers, out.options.maxContentNodes)) {
        Partial(member, "触发器节点/引用超预算或稳定身份重复，未完整扫描"); return;
    }
    const TD::TriggerDocument empty;
    const auto& before = context.baselineTriggers ? *context.baselineTriggers : empty;
    const auto& after = context.currentTriggers ? *context.currentTriggers : empty;
    if (!TriggerOwnersUnique(before, out.options.maxContentNodes) || !TriggerOwnersUnique(after, out.options.maxContentNodes)) {
        Partial(member, "触发器节点引用跨记录共享、无效或循环，不能证明单项撤销只影响本触发器"); return;
    }
    if (domain == Domain::Variables) {
        std::unordered_map<StableId, const TD::VariableRecord*> a, b;
        for (const auto& item : before.variables) a.emplace(item.id, &item);
        for (const auto& item : after.variables) b.emplace(item.id, &item);
        const auto offer = [&](const TD::VariableRecord* old, const TD::VariableRecord* now) {
            const auto& item = now ? *now : *old;
            auto row = ContentRow(out, domain, old && now ? ProjectChangeKind::Field : now ? ProjectChangeKind::Added
                : ProjectChangeKind::Removed, std::to_string(item.id));
            row.entityId = item.id; row.label = item.name.text.substr(0, out.options.maxTextBytes);
            row.beforeText = old ? old->value.text.substr(0, out.options.maxTextBytes) : "不存在";
            row.afterText = now ? now->value.text.substr(0, out.options.maxTextBytes) : "已删除";
            row.detail = "变量声明、名称、类型、数组或初始值变化";
            row.revertReason = "变量可能被触发器引用，需使用变量模块的现有编辑/撤销"; out.Offer(std::move(row));
        };
        for (const auto& old : before.variables) {
            const auto found = b.find(old.id); if (found == b.end()) offer(&old, nullptr);
            else if (!VariableEqual(old, *found->second)) offer(&old, found->second);
        }
        for (const auto& now : after.variables) if (!a.contains(now.id)) offer(nullptr, &now);
        out.page.examinedBefore = a.size(); out.page.examinedAfter = b.size();
        member.beforeCount = a.size(); member.afterCount = b.size();
    } else {
        std::unordered_map<StableId, const TD::TriggerRecord*> a, b;
        for (const auto& item : before.triggers) a.emplace(item.id, &item);
        for (const auto& item : after.triggers) b.emplace(item.id, &item);
        std::size_t work = out.options.maxContentNodes * 4;
        for (const auto& old : before.triggers) {
            const auto found = b.find(old.id); TriggerRow(out, context, &old, found == b.end() ? nullptr : found->second, work);
        }
        for (const auto& now : after.triggers) if (!a.contains(now.id)) TriggerRow(out, context, nullptr, &now, work);
        out.page.examinedBefore = a.size(); out.page.examinedAfter = b.size();
        member.beforeCount = a.size(); member.afterCount = b.size();
        bool extra = before.variablesHeader != after.variablesHeader || !TextEqual(before.wct.customCode, after.wct.customCode)
            || !TextEqual(before.wct.customComment, after.wct.customComment) || before.wct.version != after.wct.version
            || before.categories.size() != after.categories.size();
        for (std::size_t i = 0; !extra && i < before.categories.size(); ++i) {
            const auto& x = before.categories[i]; const auto& y = after.categories[i];
            extra = x.id != y.id || x.categoryId != y.categoryId || x.comment != y.comment || !TextEqual(x.name, y.name);
        }
        if (extra) { Partial(member, "触发器类别、全局代码或声明头变化未提供单项逆操作"); ContentMemberRow(out, domain); }
    }
}
Result<ProjectChangesPage> AnalyzeContent(const ProjectContentChangesContext& context, const MapDataQueryBinding& expected,
    Domain domain, OD::ObjectType type, const std::optional<ProjectChangesCursor>& cursor,
    const ProjectChangesOptions& options, std::string_view wanted = {}) {
    if (!Valid(context, expected) || !Valid(options) || domain == Domain::MapData
        || static_cast<unsigned>(domain) > static_cast<unsigned>(Domain::Variables) || static_cast<unsigned>(type) >= 7)
        return Fail<ProjectChangesPage>("CONTENT_CHANGES_CONTEXT_INVALID", "内容差异查询的身份、范围或预算无效");
    Collector out{{}, options, cursor ? cursor->nextOffset : 0, 0, wanted};
    out.page.binding = expected; out.page.editorRevision = context.editorRevision; auto& member = out.page.member;
    member.domain = domain; member.memberName = domain == Domain::Objects ? std::string(OD::NativeFileName(type))
        : domain == Domain::Triggers ? "war3map.wtg/wct" : "变量";
    const auto& object = context.objects[static_cast<std::size_t>(type)];
    const auto& oldHash = domain == Domain::Objects ? object.baselineFingerprint : context.baselineTriggerFingerprint;
    const auto& nowHash = domain == Domain::Objects ? object.currentFingerprint : context.currentTriggerFingerprint;
    const bool oldKnown = domain == Domain::Objects ? object.baselineKnown : context.baselineTriggersKnown;
    const bool nowKnown = domain == Domain::Objects ? object.currentKnown : context.currentTriggersKnown;
    if (!oldHash.empty()) member.beforeSha256 = oldHash;
    if (!nowHash.empty()) member.afterSha256 = nowHash;
    member.beforeState = !oldKnown ? MapDataState::Unreadable : oldHash.empty() ? MapDataState::Missing : MapDataState::Ready;
    member.afterState = !nowKnown ? MapDataState::Unreadable : nowHash.empty() ? MapDataState::Missing : MapDataState::Ready;
    member.changed = oldHash != nowHash || !oldKnown || !nowKnown;
    if ((domain == Domain::Objects && ((object.baseline && oldHash.empty()) || (object.current && nowHash.empty())))
        || (domain != Domain::Objects && ((context.baselineTriggers && oldHash.empty()) || (context.currentTriggers && nowHash.empty()))))
        return Fail<ProjectChangesPage>("CONTENT_CHANGES_FINGERPRINT_MISSING", "已载入文档没有对应的内容修订指纹");
    if (cursor && (cursor->binding != expected || cursor->domain != domain || cursor->objectType != type
        || cursor->editorRevision != context.editorRevision || cursor->beforeSha256 != member.beforeSha256
        || cursor->afterSha256 != member.afterSha256 || cursor->nextOffset > options.maxContentNodes * 4))
        return Fail<ProjectChangesPage>("CONTENT_CHANGES_CURSOR_STALE", "内容差异游标的域、修订或基线已经变化");
    out.page.logicalBytes = 256 + expected.documentId.size() + oldHash.size() + nowHash.size();
    if (domain == Domain::Objects) AnalyzeObjects(out, context, type); else AnalyzeTriggers(out, context, domain);
    if (member.changed && member.partial && !out.seen) ContentMemberRow(out, domain);
    out.page.totalRows = out.seen;
    out.page.logicalBytes += member.reason.size();
    if (out.oversized || out.page.logicalBytes > options.maxLogicalBytes)
        return Fail<ProjectChangesPage>("CHANGES_PAGE_BYTES", "单项内容差异超过分页字节上限");
    if (out.more && wanted.empty()) {
        ProjectChangesCursor next{expected, K::Settings, member.beforeSha256, member.afterSha256, out.offset + out.page.rows.size()};
        next.domain = domain; next.objectType = type; next.editorRevision = context.editorRevision; out.page.next = std::move(next);
    }
    return Result<ProjectChangesPage>::Success(std::move(out.page));
}
} // namespace

Result<ProjectChangesPage> QueryProjectContentChanges(const ProjectContentChangesContext& context,
    const MapDataQueryBinding& expected, ProjectChangeDomain domain, OD::ObjectType type,
    const std::optional<ProjectChangesCursor>& cursor, const ProjectChangesOptions& options) {
    try { return AnalyzeContent(context, expected, domain, type, cursor, options); }
    catch (const std::exception&) { return Fail<ProjectChangesPage>("CONTENT_CHANGES_QUERY_FAILED", "计算内容差异失败，没有修改文档"); }
}
Result<ProjectChangeReversePlan> BuildProjectContentReversePlan(const ProjectContentChangesContext& context,
    const ProjectChangeRow& selected, const ProjectChangesOptions& options) {
    try {
        if (selected.domain == Domain::MapData || selected.editorRevision != context.editorRevision
            || selected.id.empty() || selected.id.size() > 8192)
            return Fail<ProjectChangeReversePlan>("CONTENT_CHANGE_STALE", "内容差异的域或编辑修订已变化");
        const auto type = selected.object ? selected.object->type : OD::ObjectType::Unit;
        auto queried = AnalyzeContent(context, selected.binding, selected.domain, type, std::nullopt, options, selected.id);
        if (!queried.Succeeded()) return Forward<ProjectChangeReversePlan>(queried.diagnostics);
        if (selected.beforeMemberSha256 != queried.value->member.beforeSha256
            || selected.afterMemberSha256 != queried.value->member.afterSha256)
            return Fail<ProjectChangeReversePlan>("CONTENT_CHANGE_FINGERPRINT_STALE", "内容基线或当前覆盖已变化");
        if (queried.value->rows.size() != 1 || !queried.value->rows.front().revertible)
            return Fail<ProjectChangeReversePlan>("CONTENT_CHANGE_REVERSE_UNAVAILABLE", "这项变化不能可靠单独撤销");
        const auto& row = queried.value->rows.front();
        ProjectChangeReversePlan plan; plan.binding = context.binding; plan.domain = row.domain;
        plan.editorRevision = context.editorRevision; plan.changeId = row.id;
        plan.beforeMemberSha256 = row.beforeMemberSha256; plan.afterMemberSha256 = row.afterMemberSha256;
        if (row.domain == Domain::Objects) {
            plan.objectType = row.object->type; plan.objectDocument = *context.objects[static_cast<std::size_t>(*plan.objectType)].current;
            const auto status = row.objectBefore ? plan.objectDocument->SetModification(*row.object, *row.modification, *row.objectBefore)
                : plan.objectDocument->RemoveModification(*row.object, *row.modification);
            if (!status.Succeeded()) return Forward<ProjectChangeReversePlan>(status.diagnostics);
            const auto validated = OD::LniObjectCodec(*plan.objectType).Validate(*plan.objectDocument);
            if (!validated.Succeeded()) return Forward<ProjectChangeReversePlan>(validated.diagnostics);
        } else if (row.domain == Domain::Triggers) {
            for (const auto id : row.changedArguments) {
                const auto find = [id](const auto& list) { return std::find_if(list.begin(), list.end(), [id](const auto& item) { return item.id == id; }); };
                const auto a = find(context.currentTriggers->argumentNodes), b = find(context.baselineTriggers->argumentNodes);
                if (a == context.currentTriggers->argumentNodes.end() || b == context.baselineTriggers->argumentNodes.end())
                    return Fail<ProjectChangeReversePlan>("CONTENT_ARGUMENT_STALE", "触发器参数身份已变化");
                plan.arguments.push_back({id, *a, *b});
            }
        } else return Fail<ProjectChangeReversePlan>("CONTENT_CHANGE_REVERSE_UNAVAILABLE", "此范围没有单项反向编辑");
        return Result<ProjectChangeReversePlan>::Success(std::move(plan));
    } catch (const std::exception&) {
        return Fail<ProjectChangeReversePlan>("CONTENT_CHANGE_REVERSE_FAILED", "构造内容反向计划失败，没有修改文档");
    }
}
} // namespace War3Map::Services

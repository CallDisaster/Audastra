#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace War3Map::MapData {
namespace {
constexpr std::array<std::string_view, 7> kMembers{
    "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3r",
    "war3map.w3c", "war3map.w3s", "war3map.wts"};

template<class T> Result<T> Failure(const char* code, std::string message) {
    return Result<T>::Failure("MapData.Fields", code, std::move(message));
}
template<class T> Result<T> Forward(std::vector<Diagnostic> diagnostics) {
    Result<T> result; result.diagnostics = std::move(diagnostics); return result;
}
bool Allowed(MapDataKind kind) noexcept {
    return kind == MapDataKind::Settings || kind == MapDataKind::Strings
        || kind == MapDataKind::Units || kind == MapDataKind::Regions || kind == MapDataKind::Cameras || kind == MapDataKind::Sounds;
}
bool SameSource(const MapMemberState& first, const MapMemberState& second) {
    if (first.state != second.state || first.memberName != second.memberName || first.sources.size() != second.sources.size()) return false;
    for (std::size_t i = 0; i < first.sources.size(); ++i)
        if (first.sources[i].name != second.sources[i].name || first.sources[i].data != second.sources[i].data) return false;
    return true;
}
Status Editable(const MapDocument& document, MapDataKind kind) {
    if (!Allowed(kind)) return Status::Failure("MapData.Fields", "MAP_FIELD_KIND_UNSUPPORTED",
        "字段事务只开放已证明的地图设置、字符串、单位、区域、镜头和既有声音字段；其它类别保持只读。");
    if (kind == MapDataKind::Sounds) return ValidateSoundFieldSource(document);
    const auto& member = document.Member(kind);
    if (member.state != MapDataState::Ready || member.sources.size() != 1
        || member.memberName != kMembers[static_cast<std::size_t>(kind)])
        return Status::Failure("MapData.Fields", "MAP_FIELD_SOURCE_UNSAFE",
            "成员必须是唯一、已有且完整可解析的原生来源，不能把缺失或损坏来源重建为空表。");
    const bool classic = kind == MapDataKind::Settings ? (document.Settings() && document.Settings()->HasClassicLayout()
        && document.Settings()->FullyParsed())
        : kind == MapDataKind::Strings ? document.Strings().has_value()
        : kind == MapDataKind::Units ? (document.Units() && document.Units()->version == 8
        && document.Units()->subversion == 11 && document.Units()->recordLayout == PlacementRecordLayout::Classic)
        : kind == MapDataKind::Regions ? (document.Regions() && document.Regions()->version == 5)
        : (document.Cameras() && document.Cameras()->version == 0 && document.Cameras()->layout == CameraLayout::Classic);
    if (!classic) return Status::Failure("MapData.Fields", "MAP_FIELD_LAYOUT_READ_ONLY",
        "该记录布局没有经典原生字段写入证明，保留原成员。");
    const auto serialized = SerializeMapFieldTable(document, kind);
    if (!serialized.Succeeded() || !serialized.value || *serialized.value != member.sources.front().data)
        return Status::Failure("MapData.Fields", "MAP_FIELD_ROUNDTRIP_UNSAFE", "原生字段表未通过完整字节往返，不能修改。");
    return Status::Ok();
}
Status SameStringIdentity(const MapDocument& original, const MapDocument& current) {
    for (const auto* document : {&original, &current}) {
        const auto checked = Editable(*document, MapDataKind::Strings);
        if (!checked.Succeeded()) return checked;
    }
    const auto& before = original.Strings()->entries;
    const auto& after = current.Strings()->entries;
    if (before.size() != after.size()
        || original.Member(MapDataKind::Strings).sources.front().name != current.Member(MapDataKind::Strings).sources.front().name)
        return Status::Failure("MapData.Fields", "MAP_FIELD_STRING_SOURCE_STALE", "设置引用的 WTS 来源、行数或编号顺序已变化，不能沿用原来源身份。");
    for (std::size_t row = 0; row < before.size(); ++row)
        if (before[row].id != after[row].id)
            return Status::Failure("MapData.Fields", "MAP_FIELD_STRING_SOURCE_STALE", "设置引用的 WTS 编号或顺序已变化，不能沿用原来源身份。");
    return Status::Ok();
}
} // namespace

Status ValidateSoundFieldSource(const MapDocument& document) {
    const auto& member = document.Member(MapDataKind::Sounds);
    if (member.state != MapDataState::Ready || member.sources.size() != 1 || member.memberName != "war3map.w3s" || !document.Sounds())
        return Status::Failure("MapData.Fields", "MAP_FIELD_SOURCE_UNSAFE", "声音字段必须来自唯一、已有且完整可解析的 W3S，不能重建缺失或损坏来源。");
    const auto& sounds = *document.Sounds();
    if (sounds.version != 1)
        return Status::Failure("MapData.Fields", "MAP_FIELD_LAYOUT_READ_ONLY", "声音字段修改仅开放已证明的 classic W3S v1，扩展版本保持只读。");
    std::set<std::string> names, symbols;
    for (const auto& sound : sounds.entries) {
        const auto symbol = sound.name.starts_with("gg_snd_") ? sound.name : "gg_snd_" + Text::ObjectScriptName(sound.name);
        if (!names.insert(sound.name).second || !symbols.insert(symbol).second)
            return Status::Failure("MapData.Fields", "MAP_FIELD_SOUND_NAME_AMBIGUOUS", "声音原名字或生成的 gg_snd_ 脚本变量重复，不能选择唯一写入身份。");
    }
    const auto bytes = sounds.Serialize();
    if (!bytes.Succeeded() || !bytes.value || *bytes.value != member.sources.front().data)
        return Status::Failure("MapData.Fields", "MAP_FIELD_ROUNDTRIP_UNSAFE", "声音定义未通过完整原字节往返，保留原成员。");
    return Status::Ok();
}

namespace detail {
struct MapDataFieldEditingAccess {
    static void Assign(MapDocument& target, const MapDocument& parsed, MapDataKind kind) {
        target.members_[static_cast<std::size_t>(kind)] = parsed.Member(kind);
        switch (kind) {
        case MapDataKind::Settings: target.settings_ = parsed.settings_; target.lniSettings_ = parsed.lniSettings_; break;
        case MapDataKind::Units: target.units_ = parsed.units_; break;
        case MapDataKind::Doodads: target.doodads_ = parsed.doodads_; break;
        case MapDataKind::Regions: target.regions_ = parsed.regions_; break;
        case MapDataKind::Cameras: target.cameras_ = parsed.cameras_; break;
        case MapDataKind::Sounds: target.sounds_ = parsed.sounds_; break;
        case MapDataKind::Strings: target.strings_ = parsed.strings_; break;
        case MapDataKind::Count: break;
        }
    }
    static MapDocument Overlay(const MapDocument& source,
        const std::array<bool, 7>& changed, const std::array<std::optional<std::vector<std::byte>>, 7>& bytes) {
        std::vector<MapArchiveMember> members;
        for (std::size_t i = 0; i < changed.size(); ++i) {
            if (changed[i]) {
                if (bytes[i]) members.push_back({std::string(kMembers[i]), *bytes[i]});
            } else for (const auto& member : source.members_[i].sources)
                members.push_back({source.members_[i].memberName, member.data});
        }
        const auto parsed = MapDocument::FromMembers(members, source.SourcePath());
        MapDocument result(source);
        for (std::size_t i = 0; i < changed.size(); ++i) {
            const bool layoutDependency = changed[0] && (i == 1 || i == 2 || i == 4)
                && (source.members_[i].state == MapDataState::Ready || source.members_[i].state == MapDataState::Partial);
            if (changed[i] || layoutDependency) {
                Assign(result, parsed, static_cast<MapDataKind>(i));
                // FromMembers 按规范名解析；对外 typed 来源仍保留原目录别名和顺序。
                auto& projected = result.members_[i].sources;
                const auto& original = source.members_[i].sources;
                if (projected.size() == original.size())
                    for (std::size_t j = 0; j < projected.size(); ++j) projected[j].name = original[j].name;
            }
        }
        return result;
    }
    static Result<MapDocument> Project(const MapDocument& original, const MapDataEditSession& session) {
        std::array<bool, 7> changed{};
        for (std::size_t i = 0; i < changed.size(); ++i) {
            const auto& member = original.members_[i];
            const bool captured = (member.state == MapDataState::Ready || member.state == MapDataState::Partial)
                && member.sources.size() == 1 && member.memberName == kMembers[i];
            if (member.state != session.states_[i] || captured != session.original_[i].has_value()
                || (captured && member.sources.front().data != *session.original_[i]))
                return Failure<MapDocument>("MAP_FIELD_ORIGINAL_STALE", "原文档与编辑会话来源不同，不能拼接地图数据视图。");
            changed[i] = session.current_[i] != session.original_[i];
            if (changed[i] && member.state != MapDataState::Ready && member.state != MapDataState::Partial
                && member.state != MapDataState::Missing)
                return Failure<MapDocument>("MAP_FIELD_SOURCE_UNSAFE", "不可确定来源不能产生 typed 覆盖。");
        }
        return Result<MapDocument>::Success(Overlay(original, changed, session.current_));
    }
    static Result<PreparedMapDataFields> Prepare(const MapDocument& original, const MapDocument& current,
        std::string_view expectedSha256, std::span<const MapFieldEdit> edits) {
        if (edits.empty() || edits.size() > kMaxMapFieldEdits)
            return Failure<PreparedMapDataFields>("MAP_FIELD_EDIT_COUNT", "同一请求须包含 1 到 32 项字段修改。");
        const auto kind = edits.front().kind;
        if (std::any_of(edits.begin(), edits.end(), [kind](const auto& edit) { return edit.kind != kind; }))
            return Failure<PreparedMapDataFields>("MAP_FIELD_KIND_MIXED", "C1 单次字段批次只能修改同一类别。");
        for (const auto* document : {&original, &current}) {
            const auto checked = Editable(*document, kind);
            if (!checked.Succeeded()) return Forward<PreparedMapDataFields>(checked.diagnostics);
        }
        auto beforeSha = Sha256(current.Member(kind).sources.front().data);
        if (!beforeSha.Succeeded() || !beforeSha.value) return Forward<PreparedMapDataFields>(std::move(beforeSha.diagnostics));
        if (*beforeSha.value != expectedSha256)
            return Failure<PreparedMapDataFields>("MAP_FIELD_MEMBER_STALE", "当前成员 SHA-256 与请求不符，请重新查询。");
        const auto tables = ApplyMapFieldEdits(current, edits);
        if (!tables.Succeeded() || !tables.value) return Forward<PreparedMapDataFields>(tables.diagnostics);
        std::array<bool, 7> changed{}; changed[static_cast<std::size_t>(kind)] = true;
        if (kind == MapDataKind::Settings && tables.value->strings) {
            const auto checked = SameStringIdentity(original, current);
            if (!checked.Succeeded()) return Forward<PreparedMapDataFields>(checked.diagnostics);
            changed[static_cast<std::size_t>(MapDataKind::Strings)] = true;
        }
        std::array<std::optional<std::vector<std::byte>>, 7> bytes;
        for (std::size_t i = 0; i < changed.size(); ++i) if (changed[i]) {
            auto serialized = SerializeMapFieldTable(*tables.value, static_cast<MapDataKind>(i));
            if (!serialized.Succeeded() || !serialized.value) return Forward<PreparedMapDataFields>(std::move(serialized.diagnostics));
            bytes[i] = std::move(*serialized.value);
        }
        auto after = Overlay(current, changed, bytes);
        for (std::size_t i = 0; i < changed.size(); ++i) if (changed[i]) {
            const auto checked = Editable(after, static_cast<MapDataKind>(i));
            if (!checked.Succeeded()) return Forward<PreparedMapDataFields>(checked.diagnostics);
        }
        auto originalSha = Sha256(original.Member(kind).sources.front().data);
        auto afterSha = Sha256(after.Member(kind).sources.front().data);
        if (!originalSha.Succeeded() || !originalSha.value) return Forward<PreparedMapDataFields>(std::move(originalSha.diagnostics));
        if (!afterSha.Succeeded() || !afterSha.value) return Forward<PreparedMapDataFields>(std::move(afterSha.diagnostics));
        auto result = Result<PreparedMapDataFields>::Success(PreparedMapDataFields(kind,
            std::make_shared<const MapDocument>(original), std::make_shared<const MapDocument>(current),
            std::make_shared<const MapDocument>(std::move(after)), std::vector<MapFieldEdit>(edits.begin(), edits.end()),
            std::move(*originalSha.value), std::move(*beforeSha.value), std::move(*afterSha.value)));
        result.diagnostics = tables.diagnostics;
        return result;
    }
    static Result<MapDataFieldFreeze> Freeze(const MapDocument& original, const MapDocument& current) {
        std::vector<OriginalMapFieldOperation> operations;
        std::vector<MapFieldEdit> edits;
        std::vector<MapDataKind> changed;
        for (std::size_t i = 0; i < kMembers.size(); ++i) {
            const auto kind = static_cast<MapDataKind>(i);
            if (SameSource(original.Member(kind), current.Member(kind))) continue;
            if (!Allowed(kind)) return Failure<MapDataFieldFreeze>("MAP_FIELD_PROJECTION_SCOPE",
                "累计候选含 C1 字段之外的成员变化，不能转为字段保存请求。");
            for (const auto* document : {&original, &current}) {
                const auto checked = Editable(*document, kind);
                if (!checked.Succeeded()) return Forward<MapDataFieldFreeze>(checked.diagnostics);
            }
            const auto count = MapFieldRowCount(original, kind), nextCount = MapFieldRowCount(current, kind);
            if (!count.Succeeded() || !count.value) return Forward<MapDataFieldFreeze>(count.diagnostics);
            if (!nextCount.Succeeded() || !nextCount.value) return Forward<MapDataFieldFreeze>(nextCount.diagnostics);
            if (*count.value != *nextCount.value) return Failure<MapDataFieldFreeze>("MAP_FIELD_PROJECTION_SCOPE", "字段候选不能增加或删除行。");
            auto sha = Sha256(original.Member(kind).sources.front().data);
            if (!sha.Succeeded() || !sha.value) return Forward<MapDataFieldFreeze>(std::move(sha.diagnostics));
            for (std::size_t row = 0; row < *count.value; ++row) {
                auto key = MapFieldRowKey(original, kind, row), nextKey = MapFieldRowKey(current, kind, row);
                if (!key.Succeeded() || !key.value) return Forward<MapDataFieldFreeze>(std::move(key.diagnostics));
                if (!nextKey.Succeeded() || !nextKey.value) return Forward<MapDataFieldFreeze>(std::move(nextKey.diagnostics));
                if (*key.value != *nextKey.value) return Failure<MapDataFieldFreeze>("MAP_FIELD_PROJECTION_SCOPE", "字段候选改变了行身份或顺序。");
                for (const auto& field : MapFieldCatalog(kind)) {
                    if (!field.writable) continue;
                    auto before = ReadMapField(original, kind, row, field), after = ReadMapField(current, kind, row, field);
                    if (!before.Succeeded() || !before.value) return Forward<MapDataFieldFreeze>(std::move(before.diagnostics));
                    if (!after.Succeeded() || !after.value) return Forward<MapDataFieldFreeze>(std::move(after.diagnostics));
                    if (*before.value == *after.value) continue;
                    if (operations.size() == kMaxMapFieldEdits)
                        return Failure<MapDataFieldFreeze>("MAP_FIELD_CUMULATIVE_COUNT", "整个候选累计最多 32 项原来源字段差异。");
                    MapFieldEdit edit{kind, row, *key.value, std::string(field.id), *before.value, *after.value};
                    operations.push_back({*sha.value, edit}); edits.push_back(std::move(edit));
                }
            }
            changed.push_back(kind);
        }
        if (changed.empty()) return Result<MapDataFieldFreeze>::Success(MapDataFieldFreeze({}, {}));
        if (edits.empty()) return Failure<MapDataFieldFreeze>("MAP_FIELD_PROJECTION_SCOPE", "成员变化不能由 C1 可写字段表达。");
        const auto tables = ApplyMapFieldEdits(original, edits);
        if (!tables.Succeeded() || !tables.value) return Forward<MapDataFieldFreeze>(tables.diagnostics);
        std::vector<MapArchiveMember> members;
        for (const auto kind : changed) {
            auto expected = SerializeMapFieldTable(*tables.value, kind);
            const auto& actual = current.Member(kind).sources.front().data;
            if (!expected.Succeeded() || !expected.value || *expected.value != actual)
                return Failure<MapDataFieldFreeze>("MAP_FIELD_PROJECTION_SCOPE", "重放原来源字段操作的完整 native 字节与当前候选不相等。");
            members.push_back({current.Member(kind).memberName, actual});
        }
        return Result<MapDataFieldFreeze>::Success(MapDataFieldFreeze(std::move(operations), std::move(members)));
    }
    static Status Apply(MapDataEditSession& session, const PreparedMapDataFields& plan, const MapDataPublishValidator& validator) {
        const auto projected = Project(plan.Original(), session);
        if (!projected.Succeeded() || !projected.value) return {projected.diagnostics};
        for (std::size_t i = 0; i < kMembers.size(); ++i)
            if (!SameSource(projected.value->Member(static_cast<MapDataKind>(i)), plan.Before().Member(static_cast<MapDataKind>(i))))
                return Status::Failure("MapData.Fields", "MAP_FIELD_PLAN_STALE", "密封字段候选的前文档已过期，不能提交。");
        MapDataEditSession::Command command;
        std::size_t changedCount = 0;
        bool sharedStrings = false;
        for (std::size_t i = 0; i < kMembers.size(); ++i) {
            const auto kind = static_cast<MapDataKind>(i);
            if (SameSource(plan.Before().Member(kind), plan.After().Member(kind))) continue;
            if (kind != plan.Kind() && !(plan.Kind() == MapDataKind::Settings && kind == MapDataKind::Strings))
                return Status::Failure("MapData.Fields", "MAP_FIELD_PROJECTION_SCOPE", "字段候选产生了未授权关联成员变化。");
            ++changedCount;
            Status prepared;
            if (kind == MapDataKind::Units) {
                prepared = session.PrepareUnits(*plan.After().Units(), command.changes);
                if (!prepared.Succeeded()) return prepared;
                const auto count = plan.Before().Units()->units.size();
                if (count > std::numeric_limits<std::uint32_t>::max())
                    return Status::Failure("MapData.Fields", "MAP_FIELD_ROW_COUNT", "单位行数超过谱系计数范围。");
                PlacementRowLineage lineage; lineage.kind = MapDataKind::Units;
                lineage.beforeCount = lineage.afterCount = static_cast<std::uint32_t>(count);
                lineage.beforeToAfter.reserve(count);
                for (std::uint32_t row = 0; row < count; ++row) lineage.beforeToAfter.push_back(row);
                command.placements = std::make_shared<const std::vector<PlacementRowLineage>>(std::vector<PlacementRowLineage>{std::move(lineage)});
            } else {
                prepared = session.Prepare(kind, Result<std::vector<std::byte>>::Success(
                    plan.After().Member(kind).sources.front().data), command.changes);
                sharedStrings |= plan.Kind() == MapDataKind::Settings && kind == MapDataKind::Strings;
            }
            if (!prepared.Succeeded()) return prepared;
        }
        if (command.changes.size() != changedCount || command.changes.empty()
            || std::any_of(command.changes.begin(), command.changes.end(), [&](const auto& change) {
                return !change.after || *change.after != plan.After().Member(change.kind).sources.front().data;
            }))
            return Status::Failure("MapData.Fields", "MAP_FIELD_PROJECTION_SCOPE", "字段候选产生了未授权关联成员变化。");
        Status success;
        if (sharedStrings)
            success.diagnostics.push_back({DiagnosticSeverity::Warning, "MapData.Fields", "MAP_FIELD_SHARED_STRING",
                "本次设置修改保留 W3I 的 TRIGSTR 引用并改写共享 WTS 正文；同编号的其它地图引用也会读取新文字。"});
        auto committed = session.CommitCommand(std::move(command), [&](const auto& receipt) {
            return validator ? validator(plan.Before(), plan.After(), receipt) : Status::Ok();
        });
        return committed.Succeeded() ? std::move(success) : std::move(committed);
    }
};
} // namespace detail

std::span<const std::byte> PreparedMapDataFields::BeforeMemberBytes() const noexcept { return Before().Member(kind_).sources.front().data; }
std::span<const std::byte> PreparedMapDataFields::AfterMemberBytes() const noexcept { return After().Member(kind_).sources.front().data; }
Result<MapDocument> ProjectMapData(const MapDocument& original, const MapDataEditSession& current) {
    return detail::MapDataFieldEditingAccess::Project(original, current);
}
Result<MapDocument> ProjectMapData(const MapDocument& original, std::span<const MapArchiveMember> replacements) {
    std::array<bool, 7> changed{};
    std::array<std::optional<std::vector<std::byte>>, 7> bytes;
    for (const auto& member : replacements) {
        const auto found = std::find(kMembers.begin(), kMembers.end(), member.name);
        if (found == kMembers.end()) return Failure<MapDocument>("MAP_DATA_PROJECT_MEMBER_UNKNOWN", "投影成员不属于原生地图数据目录。");
        const auto index = static_cast<std::size_t>(found - kMembers.begin());
        if (changed[index]) return Failure<MapDocument>("MAP_DATA_PROJECT_MEMBER_DUPLICATE", "投影成员重复，不能猜测覆盖顺序。");
        changed[index] = true; bytes[index] = member.data;
    }
    return Result<MapDocument>::Success(detail::MapDataFieldEditingAccess::Overlay(original, changed, bytes));
}
Result<PreparedMapDataFields> PrepareMapDataFields(const MapDocument& original, const MapDocument& current,
    std::string_view expectedCurrentMemberSha256, std::span<const MapFieldEdit> edits) {
    return detail::MapDataFieldEditingAccess::Prepare(original, current, expectedCurrentMemberSha256, edits);
}
Result<MapDataFieldFreeze> FreezeOriginalMapDataFields(const MapDocument& original, const MapDocument& current) {
    return detail::MapDataFieldEditingAccess::Freeze(original, current);
}
Status MapDataEditSession::ApplyPreparedFields(const PreparedMapDataFields& plan, const MapDataPublishValidator& validator) {
    return detail::MapDataFieldEditingAccess::Apply(*this, plan, validator);
}

} // namespace War3Map::MapData

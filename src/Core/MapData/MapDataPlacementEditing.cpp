#include "War3MapCore/MapData/MapDataPlacementEditing.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace War3Map::MapData {
namespace {
constexpr float kTurn = 6.28318530718f;
template<class T> Result<T> Failure(const char* code, std::string text) {
    return Result<T>::Failure("MapData.Placement", code, std::move(text));
}
template<class T, class U> Result<T> Failed(const U& result) { return {std::nullopt, result.diagnostics}; }
std::vector<const MapDataPlacementSource*> Sources(const MapDataPlacementIntent& intent) {
    if (const auto* rotation = std::get_if<MapDataPlacementRotation>(&intent)) return {&rotation->source};
    std::vector<const MapDataPlacementSource*> result;
    const auto append = [&](const auto& sources) { for (const auto& source : sources) result.push_back(&source); };
    if (const auto* copy = std::get_if<MapDataPlacementCopy>(&intent)) append(copy->sources);
    if (const auto* removed = std::get_if<MapDataPlacementDelete>(&intent)) append(removed->sources);
    return result;
}
MapDataKind Kind(PlacementFile file) { return file == PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads; }
bool SameMember(const MapMemberState& a, const MapMemberState& b) {
    if (a.state != b.state || a.memberName != b.memberName || a.sources.size() != b.sources.size()) return false;
    for (std::size_t i = 0; i < a.sources.size(); ++i)
        if (a.sources[i].name != b.sources[i].name || a.sources[i].data != b.sources[i].data) return false;
    return true;
}
bool SameDocument(const MapDocument& a, const MapDocument& b) {
    for (std::size_t i = 0; i < a.Members().size(); ++i)
        if (!SameMember(a.Members()[i], b.Members()[i])) return false;
    return true;
}
Status SourceStatus(const MapDocument& document, MapDataKind kind) {
    const auto& member = document.Member(kind);
    const auto expected = kind == MapDataKind::Units ? "war3mapUnits.doo" : "war3map.doo";
    if (member.state != MapDataState::Ready || member.sources.size() != 1 || member.memberName != expected)
        return Status::Failure("MapData.Placement", "MAP_PLACEMENT_SOURCE_UNSAFE", "摆放成员须为唯一、已有且完整可解析来源。");
    const bool classic = kind == MapDataKind::Units ? (document.Units() && document.Units()->version == 8
        && document.Units()->subversion == 11 && document.Units()->recordLayout == PlacementRecordLayout::Classic)
        : (document.Doodads() && document.Doodads()->version == 8 && document.Doodads()->subversion == 11
            && document.Doodads()->recordLayout == PlacementRecordLayout::Classic && document.Doodads()->specialVersion == 0);
    return classic ? Status::Ok() : Status::Failure("MapData.Placement", "MAP_PLACEMENT_LAYOUT_READ_ONLY", "该摆放布局没有经典原生写入证明。");
}
Status Roundtrip(const MapDocument& document, MapDataKind kind) {
    const auto bytes = kind == MapDataKind::Units ? document.Units()->Serialize() : document.Doodads()->Serialize();
    return bytes.Succeeded() && bytes.value && *bytes.value == document.Member(kind).sources.front().data
        ? Status::Ok() : Status::Failure("MapData.Placement", "MAP_PLACEMENT_ROUNDTRIP_UNSAFE", "摆放来源未通过完整 native 字节往返。");
}
Result<PlacementWireProjection> Projection(const MapDocument& document, MapDataKind kind,
    std::span<const std::uint32_t> copied, PlacementCommandLimits limits, const MapDataPlacementCreate* created = nullptr) {
    PlacementReadLimits wireLimits; wireLimits.maxBytes = limits.maximumWireBytes;
    auto measured = created ? std::visit([&](const auto& records) {
        using Record = typename std::decay_t<decltype(records)>::value_type;
        if constexpr (std::is_same_v<Record, UnitPlacement>)
            return MeasureClassicPlacementAppend(*document.Units(), records, wireLimits);
        else return MeasureClassicPlacementAppend(*document.Doodads(), records, wireLimits);
    }, created->records) : kind == MapDataKind::Units ? MeasureClassicPlacementWire(*document.Units(), copied, wireLimits)
        : MeasureClassicPlacementWire(*document.Doodads(), copied, wireLimits);
    if (!measured.Succeeded() && std::any_of(measured.diagnostics.begin(), measured.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "MAP_WRITE_BUDGET"; }))
        return Failure<PlacementWireProjection>("MAP_PLACEMENT_WIRE_BUDGET", "摆放展开后的实际 native wire 超过 C2 预算；候选尚未复制。");
    return measured;
}
Result<detail::PlacementEditCandidate> Execute(const MapDocument& document, const MapDataPlacementIntent& intent) {
    if (const auto* rotation = std::get_if<MapDataPlacementRotation>(&intent)) {
        auto transform = FindPlacement(document, rotation->source.id);
        if (!transform.Succeeded()) return Failed<detail::PlacementEditCandidate>(transform);
        const PlacementMove move{rotation->source.id, transform.value->position, rotation->facingRadians};
        return detail::PreparePlacementMoves(document, std::span(&move, 1));
    }
    if (const auto* created = std::get_if<MapDataPlacementCreate>(&intent))
        return detail::PreparePlacementCreations(document, created->records);
    std::vector<PlacementId> ids;
    for (const auto* source : Sources(intent)) ids.push_back(source->id);
    if (const auto* copy = std::get_if<MapDataPlacementCopy>(&intent))
        return detail::PreparePlacementCopies(document, ids, copy->offset);
    return detail::PreparePlacementDeletions(document, ids);
}
Result<MapDocument> ProjectCandidate(const MapDocument& before, const detail::PlacementEditCandidate& candidate) {
    std::vector<MapArchiveMember> replacements;
    const auto assign = [&](MapDataKind kind, const auto& table) -> Status {
        if (!table) return Status::Ok();
        auto bytes = table->Serialize();
        if (!bytes.Succeeded()) return {bytes.diagnostics};
        replacements.push_back({before.Member(kind).memberName, std::move(*bytes.value)});
        return Status::Ok();
    };
    if (const auto status = assign(MapDataKind::Units, candidate.units); !status.Succeeded()) return Failed<MapDocument>(status);
    if (const auto status = assign(MapDataKind::Doodads, candidate.doodads); !status.Succeeded()) return Failed<MapDocument>(status);
    return ProjectMapData(before, replacements);
}
std::optional<MapDataPlacementIntent> OriginalIntent(const MapDocument& original, const MapDataPlacementIntent& intent) {
    MapDataPlacementIntent rebound = intent;
    const auto rebind = [&](MapDataPlacementSource& source) {
        const auto kind = Kind(source.id.file);
        if (!SourceStatus(original, kind).Succeeded() || !FindPlacement(original, source.id).Succeeded()) return false;
        const auto key = MapFieldRowKey(original, kind, source.id.index);
        const auto sha = Sha256(original.Member(kind).sources.front().data);
        if (!key.Succeeded() || !sha.Succeeded()) return false;
        source.expectedRowKey = *key.value; source.expectedMemberSha256 = *sha.value;
        return true;
    };
    if (auto* rotation = std::get_if<MapDataPlacementRotation>(&rebound)) {
        if (!rebind(rotation->source)) return std::nullopt;
    } else if (auto* copy = std::get_if<MapDataPlacementCopy>(&rebound)) {
        for (auto& source : copy->sources) if (!rebind(source)) return std::nullopt;
    } else if (auto* removed = std::get_if<MapDataPlacementDelete>(&rebound)) {
        for (auto& source : removed->sources) if (!rebind(source)) return std::nullopt;
    } else if (auto* created = std::get_if<MapDataPlacementCreate>(&rebound)) {
        const auto kind = Kind(PlacementIntentFile(rebound));
        if (!SourceStatus(original, kind).Succeeded()) return std::nullopt;
        const auto sha = Sha256(original.Member(kind).sources.front().data);
        if (!sha.Succeeded()) return std::nullopt;
        created->expectedMemberSha256 = *sha.value;
    }
    return rebound;
}
} // namespace

namespace detail {
struct MapDataPlacementEditingAccess {
    static Result<PreparedMapDataPlacement> Prepare(const MapDocument& original, const MapDocument& current,
        const MapDataPlacementIntent& intent, PlacementCommandLimits limits) {
        const auto* created = std::get_if<MapDataPlacementCreate>(&intent);
        const auto sources = Sources(intent);
        const auto sourceCount = created ? std::visit([](const auto& records) { return records.size(); }, created->records) : sources.size();
        if (!limits.maximumSources || limits.maximumSources > 32 || !limits.maximumWireBytes
            || limits.maximumWireBytes > 64U * 1024U * 1024U || !sourceCount || sourceCount > limits.maximumSources)
            return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_LIMIT", "C2 单批须为 1..32 来源，wire 预算只能收紧 64MiB 上限。");
        const auto file = PlacementIntentFile(intent);
        if (file != PlacementFile::Units && file != PlacementFile::Doodads)
            return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_KIND_UNSUPPORTED", "C2 只支持单位与普通地物。");
        const auto kind = Kind(file);
        if (const auto status = SourceStatus(current, kind); !status.Succeeded()) return Failed<PreparedMapDataPlacement>(status);
        if (const auto* rotation = std::get_if<MapDataPlacementRotation>(&intent))
            if (!std::isfinite(rotation->facingRadians) || rotation->facingRadians < 0 || rotation->facingRadians >= kTurn)
                return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_FACING_INVALID", "绝对朝向须为 [0, 2π) 有限弧度。");
        std::vector<std::uint32_t> copied;
        const bool copy = std::holds_alternative<MapDataPlacementCopy>(intent);
        for (const auto* source : sources) {
            if (source->id.file != file)
                return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_KIND_MIXED", "C2 单次来源须为同一摆放类别。");
            if (copy) copied.push_back(source->id.index);
        }
        // 第一次整表复制、来源 Serialize、逐记录 clone 前，按真正 writer traversal 计量。
        const auto projected = Projection(current, kind, copied, limits, created);
        if (!projected.Succeeded()) return Failed<PreparedMapDataPlacement>(projected);
        if (current.Member(kind).sources.front().data.size() > limits.maximumWireBytes)
            return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_WIRE_BUDGET", "当前 native 来源超过 C2 wire 预算。");
        if (SourceStatus(original, kind).Succeeded()) {
            const auto originalSize = Projection(original, kind, {}, limits);
            if (!originalSize.Succeeded()) return Failed<PreparedMapDataPlacement>(originalSize);
        }
        if (const auto status = Roundtrip(current, kind); !status.Succeeded()) return Failed<PreparedMapDataPlacement>(status);
        const auto beforeSha = Sha256(current.Member(kind).sources.front().data);
        if (!beforeSha.Succeeded()) return Failed<PreparedMapDataPlacement>(beforeSha);
        if (created) {
            if (created->expectedMemberSha256 != *beforeSha.value)
                return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_MEMBER_STALE", "新增摆放绑定的成员 SHA 与当前来源不同。");
            const auto* units = std::get_if<std::vector<UnitPlacement>>(&created->records);
            const auto start = units && std::any_of(units->begin(), units->end(), [](const auto& record) { return record.rawcode == "sloc"; });
            if (start) return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_START_CREATE_UNSUPPORTED", "起始位置需要同步玩家设置，不能经普通实体新建命令创建。");
        }
        for (const auto* source : sources) {
            if (source->expectedMemberSha256 != *beforeSha.value)
                return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_MEMBER_STALE", "摆放成员 SHA 与当前来源不同。");
            const auto key = MapFieldRowKey(current, kind, source->id.index);
            if (!key.Succeeded()) return Failed<PreparedMapDataPlacement>(key);
            if (*key.value != source->expectedRowKey)
                return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_ROW_STALE", "摆放行身份已变化，请重新查询。");
            if (!FindPlacement(current, source->id).Succeeded())
                return Failure<PreparedMapDataPlacement>("PLACEMENT_STALE", "摆放类型、创建编号或行索引已变化。");
        }
        auto candidate = Execute(current, intent);
        if (!candidate.Succeeded()) return Failed<PreparedMapDataPlacement>(candidate);
        auto after = ProjectCandidate(current, *candidate.value);
        if (!after.Succeeded()) return Failed<PreparedMapDataPlacement>(after);
        if (const auto status = SourceStatus(*after.value, kind); !status.Succeeded()) return Failed<PreparedMapDataPlacement>(status);
        const auto afterSha = Sha256(after.value->Member(kind).sources.front().data);
        if (!afterSha.Succeeded()) return Failed<PreparedMapDataPlacement>(afterSha);
        return Result<PreparedMapDataPlacement>::Success(PreparedMapDataPlacement(kind,
            std::make_shared<const MapDocument>(original), std::make_shared<const MapDocument>(current),
            std::make_shared<const MapDocument>(std::move(*after.value)), intent, OriginalIntent(original, intent),
            std::move(candidate.value->lineage), std::move(candidate.value->newIds), *beforeSha.value, *afterSha.value));
    }
    static Result<FrozenMapDataPlacement> Freeze(const MapDocument& original, const MapDocument& current,
        const MapDataPlacementIntent& intent, PlacementCommandLimits limits) {
        auto replay = Prepare(original, original, intent, limits);
        if (!replay.Succeeded()) return Failed<FrozenMapDataPlacement>(replay);
        if (!SameDocument(replay.value->After(), current))
            return Failure<FrozenMapDataPlacement>("MAP_PLACEMENT_REPLAY_MISMATCH", "累计候选不等于唯一原来源摆放意图的完整 native 重放。");
        std::vector<MapArchiveMember> members;
        const auto kind = replay.value->Kind();
        if (!SameMember(original.Member(kind), current.Member(kind))) {
            // 来源视图可保留 map/ 别名；封包成员仍必须写为规范原生名称。
            const auto& member = current.Member(kind);
            members.push_back({member.memberName, member.sources.front().data});
        }
        return Result<FrozenMapDataPlacement>::Success(FrozenMapDataPlacement(intent, std::move(members)));
    }
};
} // namespace detail

PlacementFile PlacementIntentFile(const MapDataPlacementIntent& intent) {
    return std::visit([](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, MapDataPlacementRotation>) return operation.source.id.file;
        else if constexpr (std::is_same_v<Operation, MapDataPlacementCreate>)
            return std::holds_alternative<std::vector<UnitPlacement>>(operation.records) ? PlacementFile::Units : PlacementFile::Doodads;
        else return operation.sources.empty() ? PlacementFile::Units : operation.sources.front().id.file;
    }, intent);
}

std::span<const std::byte> PreparedMapDataPlacement::BeforeMemberBytes() const noexcept { return Before().Member(kind_).sources.front().data; }
std::span<const std::byte> PreparedMapDataPlacement::AfterMemberBytes() const noexcept { return After().Member(kind_).sources.front().data; }
Result<PreparedMapDataPlacement> PrepareMapDataPlacement(const MapDocument& original, const MapDocument& current,
    const MapDataPlacementIntent& intent, PlacementCommandLimits limits) {
    try { return detail::MapDataPlacementEditingAccess::Prepare(original, current, intent, limits); }
    catch (const std::bad_alloc&) { return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_ALLOCATION", "摆放候选内存不足，未发布。"); }
    catch (const std::length_error&) { return Failure<PreparedMapDataPlacement>("MAP_PLACEMENT_ALLOCATION", "摆放候选超过容器容量，未发布。"); }
}
Result<FrozenMapDataPlacement> FreezeOriginalMapDataPlacement(const MapDocument& original, const MapDocument& current,
    const MapDataPlacementIntent& intent, PlacementCommandLimits limits) {
    try { return detail::MapDataPlacementEditingAccess::Freeze(original, current, intent, limits); }
    catch (const std::bad_alloc&) { return Failure<FrozenMapDataPlacement>("MAP_PLACEMENT_ALLOCATION", "摆放冻结内存不足，未发布。"); }
    catch (const std::length_error&) { return Failure<FrozenMapDataPlacement>("MAP_PLACEMENT_ALLOCATION", "摆放冻结超过容器容量，未发布。"); }
}
Status MapDataEditSession::ApplyPreparedPlacement(const PreparedMapDataPlacement& plan, const MapDataPublishValidator& validator) {
    auto before = ProjectMapData(plan.Original(), *this);
    if (!before.Succeeded()) return {before.diagnostics};
    if (!SameDocument(*before.value, plan.Before()))
        return Status::Failure("MapData.Placement", "MAP_PLACEMENT_PLAN_STALE", "原会话已变化，摆放候选未发布。");
    if (SameDocument(plan.Before(), plan.After()))
        return Status::Failure("MapData.Placement", "MAP_PLACEMENT_NO_CHANGE", "摆放请求没有改变原生字节。");
    const auto units = plan.Kind() == MapDataKind::Units ? &*plan.After().Units() : nullptr;
    const auto doodads = plan.Kind() == MapDataKind::Doodads ? &*plan.After().Doodads() : nullptr;
    std::vector<PlacementRowLineage> lineage(plan.Lineage().begin(), plan.Lineage().end());
    return EditPlacementRows(plan.Before(), units, doodads, std::move(lineage), [&](const auto& receipt) {
        return validator ? validator(plan.Before(), plan.After(), receipt) : Status::Ok();
    });
}

} // namespace War3Map::MapData

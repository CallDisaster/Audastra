#include "War3MapCore/MapData/TerrainEditInspection.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace War3Map::MapData {
namespace detail {

// 仅在宿主串行排除编辑期间借用；没有把会话命令历史或可变字节暴露到公共合同。
struct TerrainInspectionAccess {
    using Bytes = std::vector<std::byte>;
    struct NativeMember {
        MapDataState state;
        std::string_view name;
        const std::optional<Bytes>* original;
        const std::optional<Bytes>* current;
    };
    struct View {
        std::array<NativeMember, 3> native;
        std::shared_ptr<const TerrainGridSnapshot> terrainOriginal, terrain;
        const std::optional<Bytes>* pathingOriginal;
        const std::optional<Bytes>* pathing;
        std::shared_ptr<const Bytes> pathingIntent;
        bool terrainChanged;
    };

    static View Read(const MapDataEditSession& edits) {
        const auto member = [&](MapDataKind kind) {
            const auto index = MapDataEditSession::Index(kind);
            return NativeMember{edits.states_[index], edits.memberNames_[index],
                &edits.original_[index], &edits.current_[index]};
        };
        return {{member(MapDataKind::Doodads), member(MapDataKind::Units), member(MapDataKind::Settings)},
            edits.terrainOriginal_, edits.terrain_, &edits.pathingOriginal_, &edits.pathing_, edits.pathingRestrictions_, edits.TerrainDirty()};
    }
};

} // namespace detail
namespace {

using Bytes = std::vector<std::byte>;
constexpr std::string_view kSubsystem = "MapData.TerrainInspection";
constexpr std::string_view kVersion = "forge-terrain-edit-inspection-v3";
const std::array<std::string, 5> kMembers{
    "war3map.w3e", "war3map.wpm", "war3map.doo", "war3mapUnits.doo", "war3map.w3i"};

struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(std::string code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, std::string(kSubsystem),
        std::move(code), std::move(message), std::nullopt}}};
}

void RequireStatus(Status status) {
    if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)};
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

void AddBytes(std::size_t bytes, std::size_t& used, std::size_t maximum) {
    if (used > maximum || bytes > maximum - used)
        Fail("TERRAIN_EDIT_INSPECTION_BUDGET", "地形影响捕获的原／现拥有字节超过累计预算。");
    used += bytes;
}

bool SameRawTerrain(const TerrainGridSnapshot& a, const TerrainGridSnapshot& b) {
    const auto& first = a.header;
    const auto& second = b.header;
    if (first.version != second.version || first.mainTileset != second.mainTileset
        || first.usesCustomTileset != second.usesCustomTileset
        || first.groundTilesets != second.groundTilesets || first.cliffTilesets != second.cliffTilesets
        || first.width != second.width || first.height != second.height
        || std::bit_cast<std::array<std::uint32_t, 2>>(first.offset)
            != std::bit_cast<std::array<std::uint32_t, 2>>(second.offset)) return false;
    // position／包围盒是派生值，不拿来替代原始字段；v12 扩展位也不能被忽略。
    return std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(), b.vertices.end(),
        [](const TerrainVertex& lhs, const TerrainVertex& rhs) {
            return lhs.groundHeightRaw == rhs.groundHeightRaw
                && lhs.waterHeightAndFlagsRaw == rhs.waterHeightAndFlagsRaw
                && lhs.textureAndFlagsRaw == rhs.textureAndFlagsRaw
                && lhs.variationRaw == rhs.variationRaw && lhs.cliffAndLayerRaw == rhs.cliffAndLayerRaw
                && lhs.textureFlagsExtensionRaw == rhs.textureFlagsExtensionRaw
                && lhs.extendedTextureLayout == rhs.extendedTextureLayout;
        });
}

void RequireNativeBaseline(const detail::TerrainInspectionAccess::NativeMember& view,
    std::string_view memberName, bool exists, std::span<const std::byte> bytes) {
    if (view.state != MapDataState::Ready && view.state != MapDataState::Partial
        && view.state != MapDataState::Missing)
        Fail("TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", std::string(memberName) + " 的编辑会话源无效、歧义或不可读，不能当成缺失。");
    const auto& original = *view.original;
    const bool expectedPresent = view.state != MapDataState::Missing;
    if (view.name != memberName || expectedPresent != original.has_value() || (!exists && !bytes.empty()))
        Fail("TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", std::string(memberName) + " 的编辑会话原字节与来源状态不一致。");
    if (original.has_value() != exists
        || (original && !std::equal(original->begin(), original->end(), bytes.begin(), bytes.end())))
        Fail("TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", std::string(memberName) + " 与编辑会话绑定的原生成员不同。");
}

// 在为 dirty W3E 分配输出前检查它会增加的原字节容量；格式合法性仍由原序列化器判断。
void CheckTerrainOutputBudget(const TerrainGridSnapshot& terrain, std::size_t used, std::size_t maximum) {
    if (terrain.header.version != 11) return; // 序列化器负责给出原有只读版本错误。
    AddBytes(37, used, maximum);
    for (const auto count : {terrain.header.groundTilesets.size(), terrain.header.cliffTilesets.size()}) {
        if (count > (maximum - used) / 4)
            Fail("TERRAIN_EDIT_INSPECTION_BUDGET", "地形贴图表的最终字节超过影响捕获预算。");
        used += count * 4;
    }
    if (terrain.vertices.size() > (maximum - used) / 7)
        Fail("TERRAIN_EDIT_INSPECTION_BUDGET", "地形顶点的最终字节超过影响捕获预算。");
}

void SetCurrent(TerrainInspectionMember& member, const std::optional<Bytes>& bytes,
    std::size_t& used, std::size_t maximum) {
    if (!bytes) {
        member.current.reset();
        member.currentSha256.reset();
    } else if (!member.original || *member.original != *bytes) {
        AddBytes(bytes->size(), used, maximum);
        member.current = std::make_shared<const Bytes>(*bytes);
        member.currentSha256 = Take(Sha256(*member.current));
    }
}

Result<PathingPreservationProof> CapturePathingPreservation(const TerrainEditInspection& snapshot,
    const MapDataEditSession& edits, bool contextAttached) {
    const auto& terrain = snapshot.Members()[0];
    const auto& pathing = snapshot.Members()[1];
    if (!contextAttached || !terrain.original || !terrain.current || !pathing.original || !pathing.current)
        return Result<PathingPreservationProof>::Failure("Pathing.SavePolicy", "PATHING_POLICY_CONTEXT_MISSING",
            "只读通行保留检查需要真实归档及已绑定的源 W3E/WPM；缺失、只读或未附入不伪造证明。");
    PathingPreservationBinding binding{snapshot.SourceSha256(), *terrain.originalSha256, *pathing.originalSha256,
        snapshot.SessionIdentity(), snapshot.Revision(), snapshot.TerrainSourceRevision()};
    // APR 由同一冻结点的已有会话编码器产生，不接受外部替换。它仅暂存至分类完成，
    // WPM解析器先限制其来源大小；proof对五个span再执行独立64 MiB累计输入预算。
    const auto draft = edits.SerializePathingRestrictionDraft();
    if (!draft.Succeeded() || !draft.value) return {{}, draft.diagnostics};
    return PathingPreservationProof::Prepare(binding, *terrain.original, *pathing.original,
        *terrain.current, *pathing.current, *draft.value);
}

std::string Identity(const TerrainEditInspection& snapshot) {
    std::string manifest;
    const auto append = [&](std::string_view value) {
        manifest += std::to_string(value.size());
        manifest += ':';
        manifest += value;
    };
    append(kVersion);
    append(snapshot.SourceSha256());
    append(std::to_string(snapshot.SessionIdentity()));
    append(std::to_string(snapshot.Revision()));
    append(std::to_string(snapshot.TerrainSourceRevision()));
    append(std::to_string(snapshot.Limits().maximumOwnedBytes));
    append(std::to_string(snapshot.OwnedBytes()));
    append(snapshot.TerrainChanged() ? "changed" : "unchanged");
    append(snapshot.PathingRestrictionIntent() ? Take(Sha256(*snapshot.PathingRestrictionIntent())) : "no-pathing-intent");
    for (const auto& member : snapshot.Members()) {
        append(member.name);
        for (bool current : {false, true}) {
            const auto& bytes = current ? member.current : member.original;
            const auto& sha = current ? member.currentSha256 : member.originalSha256;
            append(bytes ? "present" : "missing");
            if (bytes) {
                append(std::to_string(bytes->size()));
                append(*sha);
            }
        }
    }
    const auto appendDiagnostics = [&](const std::vector<Diagnostic>& diagnostics) {
        append(std::to_string(diagnostics.size()));
        for (const auto& diagnostic : diagnostics) {
            append(std::to_string(static_cast<int>(diagnostic.severity)));
            append(diagnostic.subsystem);
            append(diagnostic.code);
            append(diagnostic.message);
            append(diagnostic.origin ? "origin" : "no-origin");
            if (diagnostic.origin) {
                const auto path = diagnostic.origin->source.generic_u8string();
                append({reinterpret_cast<const char*>(path.data()), path.size()});
                append(std::to_string(diagnostic.origin->offset));
                append(std::to_string(diagnostic.origin->length));
                append(diagnostic.origin->fingerprint);
            }
        }
    };
    const auto& gate = snapshot.TerrainPathingGate();
    append(gate.Succeeded() ? "allowed" : "rejected");
    appendDiagnostics(gate.diagnostics);
    const auto& preservation = snapshot.PathingPreservation();
    append(std::to_string(PathingPreservationProof::RuleVersion()));
    append(preservation.Succeeded() ? "proven" : "rejected");
    if (preservation.value) {
        const auto& proof = *preservation.value;
        append(std::to_string(static_cast<int>(proof.Kind())));
        append(std::to_string(proof.ChangedCells()));
        append(proof.OutputPathingSha256());
        append(proof.DeclarationSha256());
    }
    appendDiagnostics(preservation.diagnostics);
    return Take(Sha256Text(manifest));
}

} // namespace

Status ValidateTerrainSettingsSourceBaseline(const MapDataEditSession& edits,
    bool exists, std::span<const std::byte> bytes) {
    try {
        const auto view = detail::TerrainInspectionAccess::Read(edits);
        RequireNativeBaseline(view.native[2], kMembers[4], exists, bytes);
        return Status::Ok();
    } catch (Failure& failure) {
        return Status{std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Status::Failure(std::string(kSubsystem), "TERRAIN_EDIT_INSPECTION_ALLOCATION",
            "地形原始基线比较内存分配失败。");
    } catch (const std::length_error&) {
        return Status::Failure(std::string(kSubsystem), "TERRAIN_EDIT_INSPECTION_ALLOCATION",
            "地形原始基线比较超出容器容量。");
    }
}

Status ValidateTerrainSettingsPathingChange(const std::optional<std::vector<std::byte>>& before,
    const std::optional<std::vector<std::byte>>& after) {
    if (before == after) return Status::Ok();
    const auto reject = [] {
        return Status::Failure("MapData.Edit", "TERRAIN_SETTINGS_PATHING_CHANGED",
            "地形同次保存仅允许修改地图名称、作者、说明和建议人数；范围及其它设置请分开保存。");
    };
    // 先限制实际输入，再解析两份可变长设置。此门不扩大通常设置保存的容量。
    constexpr std::size_t maximumBytes = 1024u * 1024u;
    if (!before || !after || before->size() > maximumBytes || after->size() > maximumBytes)
        return reject();
    try {
        auto original = MapSettings::Parse(*before), current = MapSettings::Parse(*after);
        if (!original.Succeeded() || !original.value || !current.Succeeded() || !current.value
            || original.value->formatVersion != 25 || current.value->formatVersion != 25
            || !original.value->FullyParsed() || !current.value->FullyParsed()) return reject();
        const auto originalWire = original.value->Serialize(), currentWire = current.value->Serialize();
        if (!originalWire.Succeeded() || !originalWire.value || *originalWire.value != *before
            || !currentWire.Succeeded() || !currentWire.value || *currentWire.value != *after) return reject();
        // 从当前完整结构只替回四个说明字段，再与来源全文比较；不列举可能遗漏的
        // 通行字段，也不容许未知数据、浮点bit变化或其它字段被规范化后静默通过。
        current.value->mapName = original.value->mapName;
        current.value->author = original.value->author;
        current.value->description = original.value->description;
        current.value->recommendedPlayers = original.value->recommendedPlayers;
        const auto normalized = current.value->Serialize();
        return normalized.Succeeded() && normalized.value && *normalized.value == *before
            ? Status::Ok() : reject();
    } catch (const std::bad_alloc&) {
        return Status::Failure("MapData.Edit", "TERRAIN_SETTINGS_PATHING_ALLOCATION", "地图说明与地形的合并检查内存不足，原编辑保留。");
    } catch (const std::length_error&) {
        return Status::Failure("MapData.Edit", "TERRAIN_SETTINGS_PATHING_ALLOCATION", "地图说明与地形的合并检查超过容器容量，原编辑保留。");
    }
}

Result<TerrainEditInspection> CaptureTerrainEditInspection(const MapArchiveSession& source,
    const MapDataEditSession& edits, const TerrainInspectionLimits& limits) {
    try {
        if (limits.maximumOwnedBytes > TerrainInspectionLimits{}.maximumOwnedBytes)
            Fail("TERRAIN_EDIT_INSPECTION_LIMITS", "地形影响捕获只能收紧64 MiB累计字节预算。");
        TerrainEditInspection result;
        result.sourceSha256_ = source.SourceSha256();
        result.sessionIdentity_ = edits.SessionIdentity();
        result.revision_ = edits.Revision();
        result.terrainSourceRevision_ = edits.TerrainSourceRevision();
        result.limits_ = limits;
        RequireStatus(source.VerifySourceUnchanged());

        ArchiveExactMemberLimits archiveLimits;
        archiveLimits.memberCount = kMembers.size();
        archiveLimits.memberBytes = limits.maximumOwnedBytes;
        archiveLimits.totalBytes = limits.maximumOwnedBytes;
        auto originals = Take(source.CaptureExactMembers(kMembers, archiveLimits));
        const auto view = detail::TerrainInspectionAccess::Read(edits);
        result.terrainChanged_ = view.terrainChanged;
        for (std::size_t index = 0; index < result.members_.size(); ++index) {
            auto& member = result.members_[index];
            member.name = kMembers[index];
            if (!originals[index].present) continue;
            AddBytes(originals[index].bytes.size(), result.ownedBytes_, limits.maximumOwnedBytes);
            member.original = std::make_shared<const Bytes>(std::move(originals[index].bytes));
            member.originalSha256 = Take(Sha256(*member.original));
            member.current = member.original;
            member.currentSha256 = member.originalSha256;
        }

        for (std::size_t index = 0; index < view.native.size(); ++index) {
            const auto& member = result.members_[index + 2];
            RequireNativeBaseline(view.native[index], member.name, static_cast<bool>(member.original),
                member.original ? std::span<const std::byte>(*member.original) : std::span<const std::byte>{});
        }
        if (static_cast<bool>(view.terrainOriginal) != static_cast<bool>(view.terrain))
            Fail("TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", "地形会话缺少成对的原／现快照。");
        if (view.terrainOriginal) {
            if (!result.members_[0].original)
                Fail("TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", "编辑会话附有地形，但源归档缺少W3E。");
            const auto parsed = Take(ParseMapTerrain(*result.members_[0].original));
            if (!SameRawTerrain(*parsed.Snapshot(), *view.terrainOriginal))
                Fail("TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", "W3E的完整头部或原始顶点与编辑会话基线不同。");
        }
        if (view.pathingOriginal->has_value() != view.pathing->has_value())
            Fail("TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", "通行性会话缺少成对的原／现字节。");
        if (*view.pathingOriginal) {
            if (!result.members_[1].original || **view.pathingOriginal != *result.members_[1].original)
                Fail("TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH", "WPM原字节与编辑会话基线不同。");
            SetCurrent(result.members_[1], *view.pathing, result.ownedBytes_, limits.maximumOwnedBytes);
        }
        if (view.pathingIntent && std::any_of(view.pathingIntent->begin(), view.pathingIntent->end(),
            [](std::byte value) { return value != std::byte{0}; })) {
            AddBytes(view.pathingIntent->size(), result.ownedBytes_, limits.maximumOwnedBytes);
            result.pathingIntent_ = view.pathingIntent;
        }
        // 未附入编辑会话的地形／通行成员仅透传，包括只读v12和真实空成员。
        for (std::size_t index = 0; index < view.native.size(); ++index)
            SetCurrent(result.members_[index + 2], *view.native[index].current, result.ownedBytes_, limits.maximumOwnedBytes);
        if (view.terrainChanged) {
            if (!view.terrain)
                Fail("TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE", "地形标记有修改但没有当前快照。");
            CheckTerrainOutputBudget(*view.terrain, result.ownedBytes_, limits.maximumOwnedBytes);
            auto bytes = Take(SerializeMapTerrain(*view.terrain));
            auto& member = result.members_[0];
            if (!member.original || bytes != *member.original) {
                AddBytes(bytes.size(), result.ownedBytes_, limits.maximumOwnedBytes);
                member.current = std::make_shared<const Bytes>(std::move(bytes));
                member.currentSha256 = Take(Sha256(*member.current));
            }
        }
        // 门禁拒绝属于快照内容，不能让Result把完整只读快照也判作失败。
        result.terrainPathingGate_ = edits.VerifyTerrainPathingForSave();
        result.pathingPreservation_ = CapturePathingPreservation(result, edits,
            view.terrainOriginal && view.pathingOriginal->has_value());
        result.identitySha256_ = Identity(result);
        RequireStatus(source.VerifySourceUnchanged());
        if (source.SourceSha256() != result.sourceSha256_ || edits.SessionIdentity() != result.sessionIdentity_
            || edits.Revision() != result.revision_ || edits.TerrainSourceRevision() != result.terrainSourceRevision_)
            Fail("TERRAIN_EDIT_INSPECTION_STALE", "地形影响捕获期间来源或编辑修订发生变化，须重新捕获。");
        return Result<TerrainEditInspection>::Success(std::move(result));
    } catch (Failure& failure) {
        Result<TerrainEditInspection> result;
        result.diagnostics = std::move(failure.diagnostics);
        return result;
    } catch (const std::bad_alloc&) {
        return Result<TerrainEditInspection>::Failure(std::string(kSubsystem), "TERRAIN_EDIT_INSPECTION_ALLOCATION",
            "地形影响捕获内存分配失败，未返回部分快照。");
    } catch (const std::length_error&) {
        return Result<TerrainEditInspection>::Failure(std::string(kSubsystem), "TERRAIN_EDIT_INSPECTION_ALLOCATION",
            "地形影响捕获超出容器容量，未返回部分快照。");
    }
}

} // namespace War3Map::MapData

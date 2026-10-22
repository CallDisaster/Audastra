#include "War3MapCore/MapData/TerrainSavePlan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PathingSavePolicy.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace War3Map::MapData {
namespace {

constexpr std::array<std::string_view, 5> kNames{
    "war3map.w3e", "war3map.wpm", "war3map.shd", "war3map.mmp", "war3mapMap.blp"};
constexpr std::array<std::string_view, 10> kInvariantNames{
    "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3u", "war3map.w3t",
    "war3map.w3b", "war3map.w3d", "war3map.w3a", "war3map.w3h", "war3map.w3q"};
constexpr std::string_view kSubsystem = "Terrain.SavePlan";

std::vector<std::string> ExactNames() {
    std::vector<std::string> names;
    for (const auto name : kNames) names.emplace_back(name);
    for (const auto name : kInvariantNames) names.emplace_back(name);
    return names;
}

Status Reject(std::string code, std::string message) {
    return Status::Failure(std::string(kSubsystem), std::move(code), std::move(message));
}

struct Failure { std::vector<Diagnostic> diagnostics; };

void RequireStatus(const Status& status) {
    if (!status.Succeeded()) throw Failure{status.diagnostics};
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}

std::string Canonical(std::string_view value) {
    std::string name(value);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    return name;
}

bool ValidSha(std::string_view sha) {
    return sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void AddBytes(std::size_t bytes, std::size_t& total, const TerrainSavePlanLimits& limits) {
    if (total > limits.maximumBytes || bytes > limits.maximumBytes - total)
        throw Failure{Reject("TERRAIN_SAVE_PLAN_BUDGET", "地形保存计划超过来源与输出的累计字节预算。").diagnostics};
    total += bytes;
}

void CheckLimits(const TerrainSavePlanLimits& limits) {
    if (limits.maximumBytes > TerrainSavePlanLimits{}.maximumBytes)
        throw Failure{Reject("TERRAIN_SAVE_PLAN_BUDGET", "地形保存计划只能收紧 64 MiB 字节预算。").diagnostics};
}

// 序列化前按 W3E v11 固定布局计入预算，避免先分配一个超过预算的输出。
std::vector<std::byte> TerrainBytes(const TerrainGridSnapshot& terrain, std::size_t& total,
    const TerrainSavePlanLimits& limits) {
    const auto count = terrain.vertices.size();
    const auto tables = terrain.header.groundTilesets.size() + terrain.header.cliffTilesets.size();
    if (tables > (std::numeric_limits<std::size_t>::max() - 37) / 4)
        throw Failure{Reject("TERRAIN_SAVE_PLAN_BUDGET", "地形贴图表大小超出保存计划预算。").diagnostics};
    const auto header = 37 + tables * 4;
    if (count > (std::numeric_limits<std::size_t>::max() - header) / 7)
        throw Failure{Reject("TERRAIN_SAVE_PLAN_BUDGET", "地形顶点数量超出保存计划预算。").diagnostics};
    AddBytes(header + count * 7, total, limits);
    return Take(SerializeMapTerrain(terrain));
}

Status MatchBytes(const TerrainSaveMemberPlan& member, std::span<const std::byte> bytes, bool output) {
    const auto& size = output ? member.outputBytes : member.sourceBytes;
    const auto& expected = output ? member.outputSha256 : member.sourceSha256;
    if (!size || !expected || *size != bytes.size())
        return Reject("TERRAIN_SAVE_PLAN_MEMBER_MISMATCH", "成员长度或来源状态与地形保存计划不符：" + member.name);
    const auto sha = Sha256(bytes);
    if (!sha.Succeeded() || !sha.value) return {sha.diagnostics};
    if (*sha.value != *expected)
        return Reject("TERRAIN_SAVE_PLAN_MEMBER_MISMATCH", "成员字节与地形保存计划不符：" + member.name);
    return Status::Ok();
}

template<class Function> Result<TerrainSavePlan> Guard(Function&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        return {{}, std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_ALLOCATION",
            "地形保存计划内存分配失败，未返回部分计划。");
    } catch (const std::length_error&) {
        return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_ALLOCATION",
            "地形保存计划超出容器容量，未返回部分计划。");
    }
}

} // namespace

bool TerrainSavePlan::HasTerrainChanges(const MapDataEditSession& edits) noexcept {
    return edits.TerrainDirty() || edits.pathing_ != edits.pathingOriginal_;
}

Result<TerrainSavePlan> TerrainSavePlan::Prepare(const MapArchiveSession& source,
    const MapDataEditSession& edits, const TerrainSavePlanLimits& limits) {
    return PrepareArchive(source, edits, limits, nullptr);
}

Result<TerrainSavePlan> TerrainSavePlan::PrepareForSave(const MapArchiveSession& source,
    const MapDataEditSession& edits, const std::filesystem::path& gameRoot, const TerrainSavePlanLimits& limits) {
    return PrepareArchive(source, edits, limits, &gameRoot);
}

Result<TerrainSavePlan> TerrainSavePlan::PrepareArchive(const MapArchiveSession& source,
    const MapDataEditSession& edits, const TerrainSavePlanLimits& limits, const std::filesystem::path* gameRoot) {
    return Guard([&]() {
        CheckLimits(limits);
        TerrainSavePlan plan;
        plan.sourceSha256_ = source.SourceSha256();
        ArchiveExactMemberLimits captureLimits;
        captureLimits.memberCount = kNames.size() + kInvariantNames.size();
        captureLimits.memberBytes = limits.maximumBytes;
        captureLimits.totalBytes = limits.maximumBytes;
        // listfile不是完整性证据。明确查询地形与不变性依赖，连同未列名成员在同一
        // 已核源 SHA 的只读租约内捕获；解码失败不得降为 Missing/Unknown。
        const auto names = ExactNames();
        const auto captured = Take(source.DescribeExactMemberIdentities(plan.sourceSha256_, names, captureLimits));
        std::size_t total = 0;
        for (std::size_t index = 0; index < kNames.size(); ++index) {
            auto& member = plan.members_[index];
            member.name = kNames[index];
            const auto& identity = captured.members[index];
            if (identity.present) {
                AddBytes(static_cast<std::size_t>(*identity.byteSize), total, limits);
                member.sourceState = TerrainSaveSourceState::Present;
                member.sourceBytes = static_cast<std::size_t>(*identity.byteSize);
                member.sourceSha256 = identity.sha256;
            } else {
                member.sourceState = TerrainSaveSourceState::Missing;
            }
        }
        for (std::size_t index = 0; index < kInvariantNames.size(); ++index) {
            auto& member = plan.invariantMembers_[index]; member.name = kInvariantNames[index];
            const auto& identity = captured.members[kNames.size() + index];
            member.sourceState = identity.present ? TerrainSaveSourceState::Present : TerrainSaveSourceState::Missing;
            if (identity.present) {
                AddBytes(static_cast<std::size_t>(*identity.byteSize), total, limits);
                member.sourceBytes = static_cast<std::size_t>(*identity.byteSize); member.sourceSha256 = identity.sha256;
            }
        }
        return Complete(std::move(plan), edits, limits, total, &source, gameRoot);
    });
}

Result<TerrainSavePlan> TerrainSavePlan::Prepare(const WorkspaceSnapshot& source,
    const MapDataEditSession& edits, const TerrainSavePlanLimits& limits) {
    return Guard([&]() {
        CheckLimits(limits);
        TerrainSavePlan plan;
        plan.sourceSha256_ = source.aggregateSha256;
        std::size_t total = 0;
        for (std::size_t index = 0; index < kNames.size(); ++index) {
            auto& member = plan.members_[index];
            member.name = kNames[index];
            member.sourceState = TerrainSaveSourceState::Missing;
            const auto key = Canonical(member.name);
            for (const auto& entry : source.files) {
                const auto path = Canonical(IO::GenericPathUtf8(entry.relativePath));
                if (path != key && path != "map/" + key) continue;
                if (member.sourceState == TerrainSaveSourceState::Present)
                    return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_SOURCE_AMBIGUOUS",
                        "目录中的地形保存成员存在多个来源：" + member.name);
                if (!ValidSha(entry.sha256) || entry.size > std::numeric_limits<std::size_t>::max())
                    return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_SOURCE_INVALID",
                        "目录成员缺少有效的冻结长度或 SHA-256：" + member.name);
                AddBytes(static_cast<std::size_t>(entry.size), total, limits);
                member.sourceState = TerrainSaveSourceState::Present;
                member.sourceBytes = static_cast<std::size_t>(entry.size);
                member.sourceSha256 = entry.sha256;
            }
        }
        for (std::size_t index = 0; index < kInvariantNames.size(); ++index) {
            auto& member = plan.invariantMembers_[index]; member.name = kInvariantNames[index];
            member.sourceState = TerrainSaveSourceState::Missing;
            const auto key = Canonical(member.name);
            for (const auto& entry : source.files) {
                const auto path = Canonical(IO::GenericPathUtf8(entry.relativePath));
                if (path != key && path != "map/" + key) continue;
                if (member.sourceState == TerrainSaveSourceState::Present || !ValidSha(entry.sha256)
                    || entry.size > std::numeric_limits<std::size_t>::max())
                    return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_SOURCE_AMBIGUOUS",
                        "干地高度依赖缺少唯一的冻结来源：" + member.name);
                AddBytes(static_cast<std::size_t>(entry.size), total, limits);
                member.sourceState = TerrainSaveSourceState::Present; member.sourceBytes = static_cast<std::size_t>(entry.size);
                member.sourceSha256 = entry.sha256;
            }
        }
        return Complete(std::move(plan), edits, limits, total);
    });
}

Result<TerrainSavePlan> TerrainSavePlan::Complete(TerrainSavePlan plan,
    const MapDataEditSession& edits, const TerrainSavePlanLimits& limits, std::size_t total,
    const MapArchiveSession* sourceArchive, const std::filesystem::path* gameRoot) {
    if (!ValidSha(plan.sourceSha256_))
        return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_SOURCE_INVALID",
            "地形保存计划需要有效的来源 SHA-256。");
    plan.sessionIdentity_ = edits.SessionIdentity();
    plan.revision_ = edits.Revision();
    plan.terrainSourceRevision_ = edits.TerrainSourceRevision();
    plan.terrainChanged_ = HasTerrainChanges(edits);
    if (!edits.terrainOriginal_ || !edits.terrain_ || !edits.pathingOriginal_ || !edits.pathing_)
        return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_BASELINE_MISSING",
            "地形保存计划需要完整附入的原始和当前 W3E/WPM。");

    // 用真实会话基线核对冻结来源，不接受调用方传入任意旧字节来替代原图。
    auto original = TerrainBytes(*edits.terrainOriginal_, total, limits);
    RequireStatus(MatchBytes(plan.members_[0], original, false));
    RequireStatus(MatchBytes(plan.members_[1], *edits.pathingOriginal_, false));
    plan.saveGate_ = edits.VerifyTerrainPathingForSave();
    if (plan.saveGate_.Succeeded() && !edits.TerrainDirty() && edits.pathing_ != edits.pathingOriginal_) {
        const bool savedIntent = std::any_of(edits.originalPathingRestrictions_->begin(),
            edits.originalPathingRestrictions_->end(), [](std::byte value) { return value != std::byte{0}; });
        const auto layers = savedIntent ? 3u : 1u;
        AddBytes(132, total, limits);
        for (unsigned i = 0; i < layers; ++i) AddBytes(edits.pathingRestrictions_->size(), total, limits);
        auto draft = Take(edits.SerializePathingRestrictionDraft());
        const PathingPreservationBinding binding{plan.sourceSha256_, *plan.members_[0].sourceSha256,
            *plan.members_[1].sourceSha256, plan.sessionIdentity_, plan.revision_, plan.terrainSourceRevision_};
        const auto proof = PathingPreservationProof::Prepare(binding, original, *edits.pathingOriginal_,
            original, *edits.pathing_, draft);
        if (!proof.Succeeded() || !proof.value)
            plan.saveGate_ = proof.diagnostics.empty()
                ? Reject("TERRAIN_SAVE_PLAN_PATHING_PROOF", "未取得完整的通行限制来源验证结果。")
                : Status{proof.diagnostics};
    }
    if (!plan.saveGate_.Succeeded() && sourceArchive && gameRoot
        && TerrainPathingResources::Needed(*edits.terrainOriginal_, *edits.terrain_)) {
        auto resources = Take(TerrainPathingResources::Capture(*sourceArchive, *edits.terrainOriginal_, *edits.terrain_,
            *gameRoot, limits.maximumBytes - total));
        AddBytes(resources.SourceBytes(), total, limits);
        plan.pathingResources_ = std::make_shared<const TerrainPathingResources>(std::move(resources));
        plan.saveGate_ = edits.VerifyTerrainCandidateForSave(edits.terrain_, edits.pathing_,
            edits.current_[MapDataEditSession::Index(MapDataKind::Doodads)], plan.pathingResources_.get());
    }
    plan.pathingInvariant_ = edits.terrain_->header.groundTilesets != edits.terrainOriginal_->header.groundTilesets
        || edits.terrain_->header.usesCustomTileset != edits.terrainOriginal_->header.usesCustomTileset;
    if (edits.pathing_->size() == edits.pathingOriginal_->size())
        for (std::size_t i = 16; i < edits.pathing_->size(); ++i)
            plan.pathingInvariant_ = plan.pathingInvariant_
                || (((*edits.pathing_)[i] ^ (*edits.pathingOriginal_)[i]) & std::byte{kPathingRestrictionBits}) != std::byte{0};
    if (edits.terrain_->vertices.size() == edits.terrainOriginal_->vertices.size()) {
        for (std::size_t index = 0; index < edits.terrain_->vertices.size(); ++index) {
            const auto& before = edits.terrainOriginal_->vertices[index];
            const auto& after = edits.terrain_->vertices[index];
            plan.pathingInvariant_ = plan.pathingInvariant_ || before.groundHeightRaw != after.groundHeightRaw
                || before.waterHeightAndFlagsRaw != after.waterHeightAndFlagsRaw || before.variationRaw != after.variationRaw
                || ((before.textureAndFlagsRaw ^ after.textureAndFlagsRaw) & ~0x20u) != 0
                || before.cliffAndLayerRaw != after.cliffAndLayerRaw;
        }
    }
    if (plan.pathingInvariant_ && plan.saveGate_.Succeeded()) {
        for (auto& member : plan.invariantMembers_) {
            member.outputBytes = member.sourceBytes;
            member.outputSha256 = member.sourceSha256;
        }
        constexpr std::array<MapDataKind, 3> kinds{MapDataKind::Settings, MapDataKind::Units, MapDataKind::Doodads};
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            const auto& originalBytes = edits.original_[MapDataEditSession::Index(kinds[index])];
            auto& source = plan.invariantMembers_[index];
            if ((source.sourceState == TerrainSaveSourceState::Present) != originalBytes.has_value())
                return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_INVARIANT_SOURCE",
                    "地形通行不变的范围/摆放会话基线与真实来源成员存在性不同：" + source.name);
            if (originalBytes) RequireStatus(MatchBytes(source, *originalBytes, false));
            const auto& currentBytes = edits.current_[MapDataEditSession::Index(kinds[index])];
            if (currentBytes != originalBytes) {
                // 保存门已核对两DOO仅Z改变；这里先核真实原字节，再冻结该次确切输出。
                // 不把当前字节伪装成源，也不允许后台省略或二次改写摆放成员。
                if (index == 0 || !currentBytes || !originalBytes)
                    return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_INVARIANT_CHANGED",
                        "范围或摆放成员存在性改变，无法保持原通行：" + source.name);
                AddBytes(currentBytes->size(), total, limits);
                source.outputBytes = currentBytes->size();
                source.outputSha256 = Take(Sha256(*currentBytes));
                source.action = TerrainSaveMemberAction::WriteEdited;
            }
        }
    }
    for (auto& member : plan.members_) {
        member.outputBytes = member.sourceBytes;
        member.outputSha256 = member.sourceSha256;
        member.reasonCode = "TERRAIN_SAVE_PRESERVE_SOURCE";
        member.reason = "保留冻结来源的原始字节。";
    }

    const auto setOutput = [&](std::size_t index, std::vector<std::byte> bytes, std::string reason) {
        auto& member = plan.members_[index];
        member.outputBytes = bytes.size();
        member.outputSha256 = Take(Sha256(bytes));
        member.action = plan.saveGate_.Succeeded() ? TerrainSaveMemberAction::WriteEdited : TerrainSaveMemberAction::Reject;
        member.reasonCode = plan.saveGate_.Succeeded() ? "TERRAIN_SAVE_WRITE_VERIFIED_EDIT" : "TERRAIN_SAVE_GATE_REJECTED";
        member.reason = plan.saveGate_.Succeeded() ? std::move(reason)
            : "现有地形与通行性保存门拒绝此变化；本计划不授予写出资格。";
        plan.replacements_.push_back({member.name, std::move(bytes)});
    };
    if (edits.TerrainDirty()) {
        setOutput(0, TerrainBytes(*edits.terrain_, total, limits),
            "写出已编辑 W3E；保留原通行的修改已核对原版语义与实际输入，荒芜按原位联动。");
    }
    if (edits.pathing_ != edits.pathingOriginal_) {
        AddBytes(edits.pathing_->size(), total, limits);
        setOutput(1, *edits.pathing_, "写出来源绑定的本工程通行限制与对应荒芜变化；原有限制、重叠限制和其它位保持，不作通用通行重建。");
    }
    if (plan.terrainChanged_) {
        constexpr std::array<std::string_view, 3> descriptions{
            "阴影没有重建，地形变化后的阴影外观仍需原版验证。",
            "小地图标记没有重建，标记内容与位置仍需原版验证。",
            "小地图图像没有重建，可能仍显示修改前的地形外观。"};
        constexpr std::array<std::string_view, 3> codes{
            "TERRAIN_SAVE_SHADOW_NOT_REBUILT", "TERRAIN_SAVE_MARKERS_NOT_REBUILT", "TERRAIN_SAVE_MINIMAP_NOT_REBUILT"};
        for (std::size_t index = 2; index < plan.members_.size(); ++index) {
            auto& member = plan.members_[index];
            member.reasonCode = codes[index - 2];
            const auto state = member.sourceState == TerrainSaveSourceState::Present ? "原样保留；"
                : member.sourceState == TerrainSaveSourceState::Missing ? "源图缺少此成员，本次不新建；"
                : "成员未列名，无法确定是否存在，保持源归档原块；";
            member.reason = std::string(state) + std::string(descriptions[index - 2]);
            plan.diagnostics_.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), member.reasonCode,
                member.name + "：" + member.reason, std::nullopt});
        }
    }
    if (edits.SessionIdentity() != plan.sessionIdentity_ || edits.Revision() != plan.revision_
        || edits.TerrainSourceRevision() != plan.terrainSourceRevision_)
        return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_STALE",
            "生成地形保存计划期间会话、编辑修订或地形来源发生变化。");
    plan.cumulativeBytes_ = total;
    plan.maximumBytes_ = limits.maximumBytes;
    return Result<TerrainSavePlan>::Success(std::move(plan));
}

Status TerrainSavePlan::VerifyMissingMinimapEligibility(const MapDataEditSession& edits) const {
    if (edits.SessionIdentity() != sessionIdentity_ || edits.Revision() != revision_
        || edits.TerrainSourceRevision() != terrainSourceRevision_)
        return Reject("TERRAIN_SAVE_PLAN_STALE", "missing-only小地图准备必须绑定原会话、修订和地形来源。");
    if (!saveGate_.Succeeded()) return saveGate_;
    const auto& member = members_[4];
    if (member.sourceState != TerrainSaveSourceState::Missing || member.sourceBytes || member.sourceSha256
        || member.action != TerrainSaveMemberAction::Preserve || member.outputBytes || member.outputSha256)
        return Reject("MINIMAP_SAVE_NOT_MISSING", "只允许新建准确Missing的war3mapMap.blp；空成员、已有自定义图或Unknown均拒绝。");
    if (!terrainChanged_ || !edits.terrain_ || !edits.terrainOriginal_ || !edits.pathing_ || !edits.pathingOriginal_
        || edits.pathing_ != edits.pathingOriginal_ || edits.current_ != edits.original_)
        return Reject("MINIMAP_SAVE_DOMAIN", "首次missing-only小地图保存要求纯高度净变化，WPM与其它地图数据成员必须保持原字。");
    // 原SaveGate已核header、干地通行不变条件、安全一格与DOO；这里只求更窄的交集，
    // 不另造高度保存规则，也不把荒芜、贴图或pathing资格扩到小地图生成域。
    bool changed = false;
    for (std::size_t index = 0; index < edits.terrain_->vertices.size(); ++index) {
        const auto& before = edits.terrainOriginal_->vertices[index];
        const auto& after = edits.terrain_->vertices[index];
        if (before.textureAndFlagsRaw != after.textureAndFlagsRaw
            || before.waterHeightAndFlagsRaw != after.waterHeightAndFlagsRaw
            || before.variationRaw != after.variationRaw || before.cliffAndLayerRaw != after.cliffAndLayerRaw)
            return Reject("MINIMAP_SAVE_DOMAIN", "missing-only小地图只支持非空干地height净差，不包含荒芜或其它原始字段变化。");
        changed = changed || before.groundHeightRaw != after.groundHeightRaw;
    }
    return changed ? Status::Ok()
        : Reject("MINIMAP_SAVE_DOMAIN", "missing-only小地图要求至少一个实际groundHeightRaw净差。");
}

Status TerrainSavePlan::AdmitMissingMinimap(const MapDataEditSession& edits, std::vector<std::byte> generatedBytes) {
    try {
        RequireStatus(VerifyMissingMinimapEligibility(edits));
        if (generatedBytes.empty()) return Reject("MINIMAP_SAVE_DOMAIN", "实际生成的MapBLP不能是空字节。");
        auto total = cumulativeBytes_;
        AddBytes(generatedBytes.size(), total, {maximumBytes_});
        // 仅Services的真实Capture/Build/Decode证明持有者能调用此私有入口；
        // 摘要在Core对实际字节自行计算，没有公开任意bytes/SHA setter。
        auto sha = Take(Sha256(generatedBytes));
        auto& member = members_[4];
        replacements_.push_back({member.name, std::move(generatedBytes)});
        member.outputBytes = replacements_.back().data.size();
        member.outputSha256 = std::move(sha);
        member.action = TerrainSaveMemberAction::WriteEdited;
        member.reasonCode = "TERRAIN_SAVE_WRITE_MISSING_MINIMAP";
        member.reason = "显式单次新建Missing MapBLP；原干地height门与显式ground-only生成政策交集，实际范围见生成清单，不含阴影/标记重建或native验收。";
        std::erase_if(diagnostics_, [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_SAVE_MINIMAP_NOT_REBUILT";
        });
        cumulativeBytes_ = total;
        return Status::Ok();
    } catch (Failure& failure) {
        return {std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "加入missing-only小地图时分配失败；调用者必须丢弃此部分计划。");
    } catch (const std::length_error&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "加入missing-only小地图时容量异常；调用者必须丢弃此部分计划。");
    }
}

Status TerrainSavePlan::AdmitGeneratedMinimap(const MapDataEditSession& edits, std::vector<std::byte> generatedBytes) {
    try {
        if (edits.SessionIdentity() != sessionIdentity_ || edits.Revision() != revision_
            || edits.TerrainSourceRevision() != terrainSourceRevision_)
            return Reject("TERRAIN_SAVE_PLAN_STALE", "自动小地图重建必须绑定原会话、修订和地形来源。");
        if (!saveGate_.Succeeded()) return saveGate_;
        // SaveGate 是同一正式地形保存门；生成 MapBLP 不会授予任何新的
        // W3E/WPM/SHD/MMP 编辑能力。也允许未修改地形的新图补建缺失图像，
        // 或对象层变化后仅重建小地图。
        if (members_[4].sourceState == TerrainSaveSourceState::Unknown)
            return Reject("MINIMAP_SAVE_SOURCE_UNKNOWN", "自动小地图重建要求准确已知的源MapBLP状态。");
        if (generatedBytes.empty()) return Reject("MINIMAP_SAVE_DOMAIN", "实际生成的MapBLP不能是空字节。");
        auto total = cumulativeBytes_;
        AddBytes(generatedBytes.size(), total, {maximumBytes_});
        auto sha = Take(Sha256(generatedBytes));
        auto& member = members_[4];
        replacements_.erase(std::remove_if(replacements_.begin(), replacements_.end(), [&](const auto& replacement) {
            return Canonical(replacement.name) == Canonical(member.name);
        }), replacements_.end());
        replacements_.push_back({member.name, std::move(generatedBytes)});
        member.outputBytes = replacements_.back().data.size();
        member.outputSha256 = std::move(sha);
        member.action = TerrainSaveMemberAction::WriteEdited;
        member.reasonCode = "TERRAIN_SAVE_WRITE_REBUILT_MINIMAP";
        member.reason = "正常保存按当前地形重建MapBLP；同一地形保存门、来源和回读门仍然有效。";
        std::erase_if(diagnostics_, [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_SAVE_MINIMAP_NOT_REBUILT";
        });
        cumulativeBytes_ = total;
        return Status::Ok();
    } catch (Failure& failure) {
        return {std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "自动小地图重建分配失败；调用方必须丢弃此部分计划。");
    } catch (const std::length_error&) {
        return Reject("TERRAIN_SAVE_PLAN_ALLOCATION", "自动小地图重建容量异常；调用方必须丢弃此部分计划。");
    }
}

std::size_t TerrainSavePlan::OwnedCapacityBytes() const noexcept {
    std::size_t bytes = sizeof(*this);
    const auto add = [&](std::size_t value) {
        bytes = value > std::numeric_limits<std::size_t>::max() - bytes
            ? std::numeric_limits<std::size_t>::max() : bytes + value;
    };
    const auto text = [&](const std::string& value) { add(value.capacity()); add(1); };
    text(sourceSha256_);
    if (pathingResources_) add(pathingResources_->OwnedCapacityBytes());
    for (const auto& member : members_) {
        text(member.name); text(member.reasonCode); text(member.reason);
        if (member.sourceSha256) text(*member.sourceSha256);
        if (member.outputSha256) text(*member.outputSha256);
    }
    for (const auto& member : invariantMembers_) {
        text(member.name); text(member.reasonCode); text(member.reason);
        if (member.sourceSha256) text(*member.sourceSha256);
        if (member.outputSha256) text(*member.outputSha256);
    }
    add(replacements_.capacity() * sizeof(MemberReplacement));
    for (const auto& member : replacements_) { text(member.name); add(member.data.capacity()); }
    add((diagnostics_.capacity() + saveGate_.diagnostics.capacity()) * sizeof(Diagnostic));
    for (const auto* diagnostics : {&diagnostics_, &saveGate_.diagnostics})
        for (const auto& diagnostic : *diagnostics) {
            text(diagnostic.subsystem); text(diagnostic.code); text(diagnostic.message);
            if (diagnostic.origin) {
                add((diagnostic.origin->source.native().capacity() + 1) * sizeof(std::filesystem::path::value_type));
                text(diagnostic.origin->fingerprint);
            }
        }
    return bytes;
}

Status TerrainSavePlan::VerifyForSave(const MapDataEditSession& edits, std::string_view sourceSha256,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    if (edits.SessionIdentity() != sessionIdentity_ || edits.Revision() != revision_
        || edits.TerrainSourceRevision() != terrainSourceRevision_)
        return Reject("TERRAIN_SAVE_PLAN_STALE", "地形保存计划已过期，请按当前会话、编辑修订和来源重新准备。");
    return VerifyFrozenOutput(sourceSha256, replacements, removals);
}

Status TerrainSavePlan::VerifyFrozenOutput(std::string_view sourceSha256,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    if (sourceSha256 != sourceSha256_)
        return Reject("TERRAIN_SAVE_PLAN_SOURCE_MISMATCH", "保存任务不是地形计划冻结的来源。");
    if (!saveGate_.Succeeded()) return saveGate_;
    for (const auto& member : members_) {
        const auto key = Canonical(member.name);
        for (const auto& removed : removals) {
            if (Canonical(removed) == key)
                return Reject("TERRAIN_SAVE_PLAN_REMOVAL", "地形保存计划不允许删除成员：" + member.name);
        }
        const MemberReplacement* found = nullptr;
        for (const auto& replacement : replacements) {
            if (Canonical(replacement.name) != key) continue;
            if (found) return Reject("TERRAIN_SAVE_PLAN_DUPLICATE", "地形保存计划成员被重复覆盖：" + member.name);
            found = &replacement;
        }
        if (!found) {
            if (member.action != TerrainSaveMemberAction::Preserve)
                return Reject("TERRAIN_SAVE_PLAN_OUTPUT_MISSING", "保存任务缺少计划中的地形输出：" + member.name);
        } else {
            const auto checked = MatchBytes(member, found->data, true);
            if (!checked.Succeeded()) return checked;
        }
    }
    if (pathingInvariant_) {
        for (const auto& member : invariantMembers_) {
            const auto key = Canonical(member.name);
            const MemberReplacement* actual = nullptr;
            for (const auto& replacement : replacements) if (Canonical(replacement.name) == key) {
                if (actual) return Reject("TERRAIN_SAVE_PLAN_DUPLICATE", "地形通行不变依赖被重复覆盖：" + member.name);
                actual = &replacement;
            }
            for (const auto& removed : removals) if (Canonical(removed) == key && member.sourceState != TerrainSaveSourceState::Missing)
                return Reject("TERRAIN_SAVE_PLAN_INVARIANT_CHANGED", "地形通行不变保存不能同时删除范围、摆放或物编来源：" + member.name);
            if (actual) {
                const auto matched = MatchBytes(member, actual->data, true);
                if (!matched.Succeeded()) return matched;
            } else if (member.action == TerrainSaveMemberAction::WriteEdited) {
                return Reject("TERRAIN_SAVE_PLAN_OUTPUT_MISSING", "保存任务缺少已核对的摆放高度输出：" + member.name);
            }
        }
        const auto additionalSafe = [](const std::string& name) {
            return name == "war3map.wtg" || name == "war3map.wct" || name == "war3map.w3r" || name == "war3map.w3c"
                || name == "war3map.w3s" || name == "war3map.wts" || name == "war3map.imp"
                || name.ends_with(".j") || name.ends_with(".lua") || name.ends_with(".dll");
        };
        const auto frozenInput = [&](const std::string& name) {
            return std::any_of(members_.begin(), members_.end(), [&](const auto& member) { return Canonical(member.name) == name; })
                || std::any_of(invariantMembers_.begin(), invariantMembers_.end(), [&](const auto& member) { return Canonical(member.name) == name; });
        };
        for (const auto& replacement : replacements) {
            const auto key = Canonical(replacement.name);
            if (!frozenInput(key) && !additionalSafe(key))
                return Reject("TERRAIN_SAVE_PLAN_HEIGHT_RESOURCE_SCOPE",
                    "地形保留原WPM时不能同时导入或覆盖资源；请先单独保存资源变更：" + replacement.name);
        }
        for (const auto& removed : removals) {
            const auto key = Canonical(removed);
            if (!frozenInput(key) && !additionalSafe(key))
                return Reject("TERRAIN_SAVE_PLAN_HEIGHT_RESOURCE_SCOPE",
                    "地形保留原WPM时不能同时删除资源：" + removed);
        }
    }
    if (pathingResources_) return pathingResources_->VerifyFrozenOutput(replacements, removals);
    return Status::Ok();
}

Status TerrainSavePlan::VerifyReadback(const MapArchiveSession& saved, const TerrainSavePlanLimits& readLimits) const {
    if (!saveGate_.Succeeded()) return saveGate_;
    if (readLimits.maximumBytes > TerrainSavePlanLimits{}.maximumBytes)
        return Reject("TERRAIN_SAVE_PLAN_BUDGET", "地形五成员回读只能收紧64MiB读取预算。");
    const auto names = ExactNames();
    ArchiveExactMemberLimits limits;
    limits.memberCount = kNames.size() + kInvariantNames.size();
    limits.memberBytes = limits.totalBytes = readLimits.maximumBytes;
    const auto captured = saved.CaptureExactMembers(names, limits);
    if (!captured.Succeeded() || !captured.value) return {captured.diagnostics};
    // 共享保存单元预收3×读取上限，另外保留固定hash/metadata工作；这里再核实际拥有
    // 的body/name/descriptor capacity。普通调用默认64MiB读取语义不变。
    std::size_t capacity = sizeof(*captured.value) + captured.value->capacity() * sizeof(ArchiveExactMember);
    for (const auto& member : *captured.value) {
        if (member.bytes.capacity() > std::numeric_limits<std::size_t>::max() - capacity)
            return Reject("TERRAIN_SAVE_PLAN_BUDGET", "五成员回读capacity溢出。");
        capacity += member.bytes.capacity();
        if (member.name.capacity() + 1 > std::numeric_limits<std::size_t>::max() - capacity)
            return Reject("TERRAIN_SAVE_PLAN_BUDGET", "五成员回读名称capacity溢出。");
        capacity += member.name.capacity() + 1;
    }
    if (capacity > readLimits.maximumBytes * 3 + 64 * 1024)
        return Reject("TERRAIN_SAVE_PLAN_BUDGET", "五成员回读实际capacity超过预收工作上界。");
    for (std::size_t index = 0; index < members_.size(); ++index) {
        const auto& member = members_[index];
        const auto& actual = (*captured.value)[index];
        if (member.sourceState == TerrainSaveSourceState::Unknown)
            return Reject("TERRAIN_SAVE_PLAN_READBACK_UNKNOWN", "地形保存计划缺少可核对的成员来源：" + member.name);
        if (member.outputSha256) {
            if (!actual.present)
                return Reject("TERRAIN_SAVE_PLAN_MEMBER_MISMATCH", "回读缺少地形保存计划要求的成员：" + member.name);
            const auto matched = MatchBytes(member, actual.bytes, true);
            if (!matched.Succeeded()) return matched;
        }
        if (!member.outputSha256 && actual.present) {
            return Reject("TERRAIN_SAVE_PLAN_READBACK_UNEXPECTED", "回读发现计划之外的新地形成员：" + member.name);
        }
    }
    if (pathingInvariant_) for (std::size_t index = 0; index < invariantMembers_.size(); ++index) {
        const auto& member = invariantMembers_[index];
        const auto& actual = (*captured.value)[kNames.size() + index];
        if (actual.present != (member.sourceState == TerrainSaveSourceState::Present))
            return Reject("TERRAIN_SAVE_PLAN_INVARIANT_CHANGED", "保存后的范围/摆放/物编依赖存在性不同：" + member.name);
        if (actual.present) {
            const auto matched = MatchBytes(member, actual.bytes, true);
            if (!matched.Succeeded()) return matched;
        }
    }
    if (pathingResources_) {
        std::size_t consumed = 0;
        for (const auto& member : *captured.value) consumed += member.bytes.size();
        return pathingResources_->VerifyReadback(saved, readLimits.maximumBytes - consumed);
    }
    return Status::Ok();
}

} // namespace War3Map::MapData

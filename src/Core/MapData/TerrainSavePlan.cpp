#include "War3MapCore/MapData/TerrainSavePlan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace War3Map::MapData {
namespace {

constexpr std::array<std::string_view, 5> kNames{
    "war3map.w3e", "war3map.wpm", "war3map.shd", "war3map.mmp", "war3mapMap.blp"};
constexpr std::string_view kSubsystem = "Terrain.SavePlan";

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
    return Guard([&]() {
        CheckLimits(limits);
        TerrainSavePlan plan;
        plan.sourceSha256_ = source.SourceSha256();
        std::size_t total = 0;
        for (std::size_t index = 0; index < kNames.size(); ++index) {
            auto& member = plan.members_[index];
            member.name = kNames[index];
            const auto key = Canonical(member.name);
            const MapArchiveMember* found = nullptr;
            for (const auto& entry : source.Members()) {
                if (Canonical(entry.name) != key) continue;
                if (found)
                    return Result<TerrainSavePlan>::Failure(std::string(kSubsystem), "TERRAIN_SAVE_PLAN_SOURCE_AMBIGUOUS",
                        "地形保存成员存在多个来源：" + member.name);
                found = &entry;
            }
            if (found) {
                AddBytes(found->data.size(), total, limits);
                member.sourceState = TerrainSaveSourceState::Present;
                member.sourceBytes = found->data.size();
                member.sourceSha256 = Take(Sha256(found->data));
            } else {
                // 匿名 MPQ 块可能包含这个名字；不能从 listfile 未列出推断缺失。
                member.sourceState = source.HasOpaqueMembers()
                    ? TerrainSaveSourceState::Unknown : TerrainSaveSourceState::Missing;
            }
        }
        return Complete(std::move(plan), edits, limits, total);
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
        return Complete(std::move(plan), edits, limits, total);
    });
}

Result<TerrainSavePlan> TerrainSavePlan::Complete(TerrainSavePlan plan,
    const MapDataEditSession& edits, const TerrainSavePlanLimits& limits, std::size_t total) {
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
            "写出已编辑 W3E；资格仍限于现有门允许的干地高度和荒芜净变化。");
    }
    if (edits.pathing_ != edits.pathingOriginal_) {
        AddBytes(edits.pathing_->size(), total, limits);
        setOutput(1, *edits.pathing_, "仅写出与 W3E 荒芜逐格一致的 WPM 位变化，保留其它原始位；未通用重建通行性。");
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
    return Result<TerrainSavePlan>::Success(std::move(plan));
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
    return Status::Ok();
}

Status TerrainSavePlan::VerifyReadback(const MapArchiveSession& saved) const {
    if (!saveGate_.Succeeded()) return saveGate_;
    for (const auto& member : members_) {
        if (member.sourceState == TerrainSaveSourceState::Unknown) continue;
        if (member.outputSha256) {
            const auto bytes = saved.ReadMember(member.name);
            if (!bytes.Succeeded() || !bytes.value) return {bytes.diagnostics};
            const auto checked = MatchBytes(member, *bytes.value, true);
            if (!checked.Succeeded()) return checked;
        } else if (saved.HasMember(member.name)) {
            return Reject("TERRAIN_SAVE_PLAN_READBACK_UNEXPECTED", "回读发现计划之外的新地形成员：" + member.name);
        }
    }
    return Status::Ok();
}

} // namespace War3Map::MapData

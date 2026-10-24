#include "War3MapCore/Services/TerrainShadowBake.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainShadowCaster.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainShadowCasterResources.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "Terrain.Shadow";
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure(kSubsystem, code, message).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Charge(std::size_t& used, std::size_t count, std::size_t maximum, std::size_t multiple = 1) {
    if (!multiple || used > maximum || count > (maximum - used) / multiple)
        Fail("TERRAIN_SHADOW_BUDGET", "阴影计算的累计输入、内存或工作超过预算。");
    used += count * multiple;
}
bool IsShadow(std::string_view name) { return Text::LowerAscii(name) == "war3map.shd"; }
Status CheckOutput(std::span<const MemberReplacement> output, std::span<const std::string> removed,
    std::span<const std::byte> expected) {
    const MemberReplacement* selected = nullptr;
    if (std::any_of(removed.begin(), removed.end(), IsShadow))
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_OUTPUT_CONFLICT", "计算阴影不能与删除SHD混用。");
    for (const auto& member : output) if (IsShadow(member.name)) {
        if (selected) return Status::Failure(kSubsystem, "TERRAIN_SHADOW_OUTPUT_CONFLICT", "最终列表重复写入SHD。");
        selected = &member;
    }
    if (!selected || !std::equal(selected->data.begin(), selected->data.end(), expected.begin(), expected.end()))
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_OUTPUT_STALE", "最终SHD与本次计算结果不同。");
    return Status::Ok();
}
} // namespace

Result<std::shared_ptr<const TerrainShadowBake>> TerrainShadowBake::Capture(const MapArchiveSession& source,
    const std::filesystem::path& gameRoot, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const TerrainShadowBakeLimits& limits, std::stop_token cancellation) {
    using Output = Result<std::shared_ptr<const TerrainShadowBake>>;
    try {
        const TerrainShadowBakeLimits hard;
        if (!limits.maximumSourceBytes || limits.maximumSourceBytes > hard.maximumSourceBytes
            || !limits.maximumWorkingBytes || limits.maximumWorkingBytes > hard.maximumWorkingBytes
            || !limits.maximumWork || limits.maximumWork > hard.maximumWork)
            Fail("TERRAIN_SHADOW_LIMITS", "阴影计算预算只能收紧非零硬上限。");
        if (cancellation.stop_requested()) Fail("TERRAIN_SHADOW_CANCELLED", "阴影计算已取消，原阴影保留。");
        auto owner = std::shared_ptr<TerrainShadowBake>(new TerrainShadowBake);
        owner->sourceSha256_ = source.SourceSha256();
        TerrainShadowCasterLimits casterLimits;
        casterLimits.maximumSourceBytes = std::min(casterLimits.maximumSourceBytes, limits.maximumSourceBytes);
        casterLimits.maximumWorkingBytes = std::min(casterLimits.maximumWorkingBytes, limits.maximumWorkingBytes);
        owner->casters_ = Take(TerrainShadowCasterResources::Capture(source, gameRoot, replacements, removals,
            casterLimits, cancellation));
        owner->casterCount_ = owner->casters_->Casters().size();
        std::size_t sourceBytes = owner->casters_->SourceBytes(), working = owner->casters_->WorkingBytes(), work = 0;
        owner->light_ = Take(MapData::TerrainShadowResources::Capture(gameRoot,
            Game::NativeEditorStartupRules::Classic127aExpansion,
            std::min(limits.maximumSourceBytes - sourceBytes, limits.maximumWorkingBytes - working)));
        Charge(sourceBytes, owner->light_->SourceBytes(), limits.maximumSourceBytes);
        Charge(working, owner->light_->CaptureBudgetBytes(), limits.maximumWorkingBytes);
        const auto input = std::find_if(owner->casters_->Dependencies().begin(), owner->casters_->Dependencies().end(),
            [](const auto& fact) { return Text::LowerAscii(fact.name) == "war3map.w3e"; });
        if (input == owner->casters_->Dependencies().end() || input->sha256.empty())
            Fail("TERRAIN_SHADOW_SOURCE_UNKNOWN", "阴影计算缺少实际最终地形身份。");
        owner->terrainSha256_ = input->sha256;
        auto terrainLimits = limits.terrain;
        terrainLimits.maximumSampleBytes = std::min(terrainLimits.maximumSampleBytes, limits.maximumWorkingBytes - working);
        terrainLimits.maximumWork = std::min(terrainLimits.maximumWork, limits.maximumWork - work);
        auto receiver = Take(MapData::SampleOrdinaryTerrainShadowHeights(owner->casters_->Terrain(),
            owner->casters_->SpecialDoodads(), terrainLimits, cancellation));
        Charge(work, receiver.work, limits.maximumWork);
        Charge(working, receiver.receiverHeights.size(), limits.maximumWorkingBytes, sizeof(float));
        terrainLimits.maximumWork = std::min(terrainLimits.maximumWork, limits.maximumWork - work);
        // 投影位、codec强度和最终原字输出三份，先收容量再进入分配阶段。
        Charge(working, receiver.receiverHeights.size(), limits.maximumWorkingBytes, 3);
        auto projection = Take(MapData::ProjectTerrainShadow({receiver.width, receiver.height,
            receiver.receiverHeights, owner->light_->LightDirection()}, terrainLimits, cancellation));
        Charge(work, projection.work, limits.maximumWork);
        if (owner->casterCount_) {
            const auto& bounds = owner->light_->TerrainHeightBounds();
            if (!bounds)
                Fail("TERRAIN_SHADOW_HEIGHT_BOUNDS_UNKNOWN", "投影模型缺少同次游戏来源的地形最高和最低高度，原阴影保留。");
            const auto& sourceModels = owner->casters_->Models();
            const auto& sourceCasters = owner->casters_->Casters();
            Charge(working, sourceModels.size(), limits.maximumWorkingBytes, sizeof(MapData::TerrainShadowCasterModelInput));
            Charge(working, sourceCasters.size(), limits.maximumWorkingBytes, sizeof(MapData::TerrainShadowCasterInstanceInput));
            Charge(working, receiver.receiverHeights.size(), limits.maximumWorkingBytes);
            std::vector<MapData::TerrainShadowCasterModelInput> models;
            std::vector<MapData::TerrainShadowCasterInstanceInput> instances;
            models.reserve(sourceModels.size()); instances.reserve(sourceCasters.size());
            Charge(work, sourceModels.size(), limits.maximumWork);
            Charge(work, sourceCasters.size(), limits.maximumWork, 4);
            for (const auto& model : sourceModels) {
                if (cancellation.stop_requested()) Fail("TERRAIN_SHADOW_CANCELLED", "阴影模型准备已取消。");
                models.push_back({model.path, model.bytes});
            }
            for (const auto& caster : sourceCasters) {
                if (cancellation.stop_requested()) Fail("TERRAIN_SHADOW_CANCELLED", "阴影实例准备已取消。");
                MapData::TerrainShadowCasterInstanceInput instance{caster.modelIndex, caster.nativeMatrix};
                for (std::size_t axis = 0; axis < 3; ++axis)
                    instance.actualScale[axis] = caster.placement.scale[axis] * caster.defaultScale;
                instances.push_back(instance);
            }
            MapData::TerrainShadowCasterLimits rayLimits;
            rayLimits.maximumDimension = std::min(rayLimits.maximumDimension, terrainLimits.maximumDimension);
            rayLimits.maximumPixels = std::min(rayLimits.maximumPixels, terrainLimits.maximumPixels);
            rayLimits.maximumSampleBytes = std::min(rayLimits.maximumSampleBytes, terrainLimits.maximumSampleBytes);
            rayLimits.maximumWork = std::min(rayLimits.maximumWork, limits.maximumWork - work);
            // 共享解析器先计量再展开，解析模型、单模型编译与局部/世界几何
            // 分别预留，三者可同时驻留；来源owner已计入借用的原字节。
            const auto scratch = limits.maximumWorkingBytes - working;
            rayLimits.maximumParsedBytes = std::min(rayLimits.maximumParsedBytes, scratch / 3);
            rayLimits.maximumCompiledBytes = std::min(rayLimits.maximumCompiledBytes, (scratch - rayLimits.maximumParsedBytes) / 2);
            rayLimits.maximumGeometryBytes = std::min(rayLimits.maximumGeometryBytes,
                scratch - rayLimits.maximumParsedBytes - rayLimits.maximumCompiledBytes);
            const auto casterProjection = Take(MapData::ProjectFilteredStaticTerrainShadowCasters(
                {receiver.width, receiver.height, receiver.receiverHeights, owner->light_->LightDirection()},
                owner->casters_->Terrain().header.offset, models, instances, {(*bounds)[0], (*bounds)[1]}, rayLimits, cancellation));
            Charge(work, casterProjection.work, limits.maximumWork);
            Charge(work, projection.terrainShadowBits.size(), limits.maximumWork);
            for (std::size_t i = 0; i < projection.terrainShadowBits.size(); ++i) {
                if ((i % 4096) == 0 && cancellation.stop_requested())
                    Fail("TERRAIN_SHADOW_CANCELLED", "阴影合成已取消，未返回部分输出。");
                projection.terrainShadowBits[i] |= casterProjection.casterShadowBits[i];
            }
        }
        Charge(work, projection.terrainShadowBits.size(), limits.maximumWork, 3);
        MapData::ShadowGridSnapshot shadow;
        shadow.width = owner->width_ = projection.width; shadow.height = owner->height_ = projection.height;
        shadow.intensities.reserve(projection.terrainShadowBits.size());
        // 原版562750写出时只测试运行数组bit0，将其编码为00/FF；高位仅供
        // 即时显示，不能直接把0/1位图交给无损SHD codec，也不能复制旧高位。
        for (std::size_t i = 0; i < projection.terrainShadowBits.size(); ++i) {
            if ((i % 4096) == 0 && cancellation.stop_requested())
                Fail("TERRAIN_SHADOW_CANCELLED", "阴影编码已取消，未返回部分输出。");
            shadow.intensities.push_back(projection.terrainShadowBits[i] ? 0xff : 0);
        }
        owner->bytes_ = Take(MapData::SerializeMapShadow(shadow));
        if (cancellation.stop_requested()) Fail("TERRAIN_SHADOW_CANCELLED", "阴影计算已取消，未返回部分输出。");
        return Output::Success(std::move(owner));
    } catch (Failure& failure) { Output result; result.diagnostics = std::move(failure.diagnostics); return result; }
    catch (const std::bad_alloc&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_ALLOCATION", "阴影计算分配失败，原文件和编辑保留。"); }
    catch (const std::length_error&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_ALLOCATION", "阴影计算容量异常，原文件和编辑保留。"); }
}

Status TerrainShadowBake::AdmitToPlan(MapData::TerrainSavePlan& plan, const MapData::MapDataEditSession& edits) const {
    if (sourceSha256_ != plan.SourceSha256() || terrainSha256_ != plan.Members()[0].outputSha256)
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SOURCE_STALE", "阴影计算与地形保存计划的来源或最终地形不同。");
    return plan.AdmitGeneratedShadow(edits, bytes_);
}
Status TerrainShadowBake::VerifyFrozenOutput(const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    if (source.SourceSha256() != sourceSha256_)
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SOURCE_STALE", "阴影任务与当前归档来源不同。");
    const auto output = CheckOutput(replacements, removals, bytes_); if (!output.Succeeded()) return output;
    const auto direction = light_->VerifySources(light_->CaptureBudgetBytes()); if (!direction.Succeeded()) return direction;
    return casters_->VerifySources(source, replacements, removals);
}
Status TerrainShadowBake::VerifyReadback(const MapArchiveSession& saved) const {
    const auto direction = light_->VerifySources(light_->CaptureBudgetBytes()); if (!direction.Succeeded()) return direction;
    const auto dependencies = casters_->VerifyReadback(saved); if (!dependencies.Succeeded()) return dependencies;
    const std::array<std::string, 1> names{"war3map.shd"};
    ArchiveExactMemberLimits limits;
    limits.memberCount = 1; limits.memberBytes = limits.totalBytes = bytes_.size();
    const auto members = saved.CaptureExactMembers(names, limits);
    if (!members.Succeeded() || !members.value) return {members.diagnostics};
    if (!(*members.value)[0].present || (*members.value)[0].bytes != bytes_)
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_READBACK_STALE", "实际保存的SHD与本次计算结果不一致。");
    return Status::Ok();
}

Status PrepareTerrainShadowBake(MapSaveTask& task, const std::filesystem::path& gameRoot,
    const MapData::MapDataEditSession* edits, std::shared_ptr<const MapData::TerrainSavePlan>& plan) {
    if (!task.archive || (task.executed && !task.InFinalDerivedStage()) || task.terrainShadow)
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SAVE_STAGE", "阴影只能在同次最终保存阶段计算一次。");
    if (task.sourceSha256 != task.archive->SourceSha256())
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SOURCE_STALE", "阴影任务与冻结归档来源不同。");
    if (std::any_of(task.replacements.begin(), task.replacements.end(), [](const auto& item) { return IsShadow(item.name); })
        || std::any_of(task.removals.begin(), task.removals.end(), IsShadow))
        return Status::Failure(kSubsystem, "TERRAIN_SHADOW_OUTPUT_CONFLICT", "显式计算阴影不能和手动写入或删除SHD混用。");
    auto captured = TerrainShadowBake::Capture(*task.archive, gameRoot, task.replacements, task.removals);
    if (!captured.Succeeded() || !captured.value) return {std::move(captured.diagnostics)};
    auto nextPlan = plan;
    if (plan) {
        if (!edits) return Status::Failure(kSubsystem, "TERRAIN_SHADOW_SAVE_STAGE", "阴影保存计划缺少当前会话。");
        auto candidate = std::make_shared<MapData::TerrainSavePlan>(*plan);
        const auto admitted = (*captured.value)->AdmitToPlan(*candidate, *edits);
        if (!admitted.Succeeded()) return admitted;
        nextPlan = std::move(candidate);
    }
    task.replacements.push_back({"war3map.shd", (*captured.value)->Bytes()});
    task.terrainShadow = std::move(*captured.value); plan = std::move(nextPlan);
    return Status::Ok();
}

} // namespace War3Map::Services

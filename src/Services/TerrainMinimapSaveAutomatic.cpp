#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "TerrainMinimapSave";
constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kReadReserve = 8 * kMiB;
constexpr std::size_t kDecodedBytes = 256 * 256 * 4;
constexpr std::array<std::string_view, 13> kMinimapInputs{
    "war3map.w3e", "war3map.w3i", "war3map.doo", "war3mapunits.doo",
    "war3map.w3u", "war3map.w3t", "war3map.w3b", "war3map.w3d", "war3map.w3a", "war3map.w3h", "war3map.w3q",
    "terrainart/terrain.slk", "terrainart/clifftypes.slk"};
bool AffectsMinimap(std::string_view name) {
    return std::find(kMinimapInputs.begin(), kMinimapInputs.end(), name) != kMinimapInputs.end()
        || name == "terrainart/water.slk" || name == "ui/miscdata.txt" || name == "ui/worldeditdata.txt"
        || (name != "war3mapmap.blp" && name != "war3mapmap.tga"
            && (name.ends_with(".blp") || name.ends_with(".tga") || name.ends_with(".mdx")
                || name.ends_with(".mdl") || name.ends_with(".slk")));
}

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure(kSubsystem, code, message).diagnostics};
}
void Check(const Status& status) { if (!status.Succeeded()) throw Failure{status.diagnostics}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class Function> Status Guard(Function&& function) {
    try { function(); return Status::Ok(); }
    catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::exception&) {
        return Status::Failure(kSubsystem, "MINIMAP_SAVE_AUTOMATIC_EXCEPTION", "自动小地图复核未能完成，拒绝接受保存结果。");
    }
}
std::string Key(std::string_view name) {
    if (name.empty() || name.size() > 1024 || name.find('\0') != std::string_view::npos)
        Fail("MINIMAP_SAVE_DOMAIN", "小地图保存成员名必须非空且有界。");
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    return key;
}
std::filesystem::path Path(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || path.native().size() > 32767)
        Fail("MINIMAP_SAVE_SOURCE", "自动小地图要求有界的绝对源和目标路径。");
    std::error_code error;
    auto value = std::filesystem::weakly_canonical(path, error).native();
    if (error) Fail("MINIMAP_SAVE_SOURCE", "无法核对自动小地图来源或目标路径。");
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return std::filesystem::path(std::move(value));
}
void SourceTask(const MapSaveTask& task) {
    if (!task.archive || task.sourceSha256 != task.archive->SourceSha256()
        || Path(task.sourcePath) != Path(task.archive->SourcePath()))
        Fail("MINIMAP_SAVE_SOURCE", "自动小地图必须来自本次FreezeArchive拥有的真实归档。");
    if (task.inPlace != (Path(task.target) == Path(task.sourcePath)))
        Fail("MINIMAP_SAVE_TARGET", "自动小地图保存目标与原地保存标记不一致。");
    if (task.replacements.size() > 16384 || task.removals.size() > 16384)
        Fail("MINIMAP_SAVE_BUDGET", "自动小地图保存成员列表超过有界数量。");
}
const MemberReplacement* Replacement(const MapSaveTask& task, std::string_view name) {
    const MemberReplacement* found = nullptr;
    for (const auto& member : task.replacements) if (Key(member.name) == name) {
        if (found) Fail("MINIMAP_SAVE_DOMAIN", "最终保存列表重复覆盖小地图依赖成员。");
        found = &member;
    }
    return found;
}
void RequireNotRemoved(const MapSaveTask& task, std::string_view name) {
    for (const auto& removed : task.removals) if (Key(removed) == name)
        Fail("MINIMAP_SAVE_DEPENDENCY_REMOVED", "本次保存删除了自动小地图依赖成员。");
}
std::vector<ArchiveExactMember> Capture(const MapArchiveSession& source, std::span<const std::string> names,
    std::size_t maximum = kReadReserve) {
    ArchiveExactMemberLimits limits;
    limits.memberCount = names.size(); limits.memberBytes = maximum; limits.totalBytes = maximum;
    return Take(source.CaptureExactMembers(names, limits));
}
std::size_t Add(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        Fail("MINIMAP_SAVE_BUDGET", "自动小地图工作预算溢出。");
    return left + right;
}
std::size_t Available(const TerrainMinimapSaveLimits& limits, std::size_t retained) {
    if (!limits.maxWorkingBytes || limits.maxWorkingBytes > TerrainMinimapSaveLimits{}.maxWorkingBytes
        || retained >= limits.maxWorkingBytes)
        Fail("MINIMAP_SAVE_BUDGET", "自动小地图捕获、计划和输出超过共享工作预算。");
    return limits.maxWorkingBytes - retained;
}
std::size_t TaskCapacity(const MapSaveTask& task) {
    std::size_t bytes = task.replacements.capacity() * sizeof(MemberReplacement);
    for (const auto& member : task.replacements) bytes = Add(bytes, Add(member.name.capacity() + 1, member.data.capacity()));
    return bytes;
}
std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) Fail("MINIMAP_SAVE_ENCODING", "自动小地图BLP头不完整。");
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 4; ++shift) value |= std::to_integer<std::uint32_t>(bytes[offset + shift]) << (shift * 8);
    return value;
}
Render::TerrainMinimapMipFacts DecodeClassic(std::span<const std::byte> bytes) {
    if (bytes.size() < 160 || bytes.size() > Render::ClassicMinimapCandidateLimits{}.maxBlpBytes
        || U32(bytes, 0) != 0x31504c42 || U32(bytes, 4) != 0 || U32(bytes, 8) != 0
        || U32(bytes, 12) != 256 || U32(bytes, 16) != 256 || U32(bytes, 20) != 3 || U32(bytes, 24) != 0)
        Fail("MINIMAP_SAVE_ENCODING", "自动小地图必须是256² opaque BLP1 JPEG单mip。");
    const auto headerBytes = U32(bytes, 156), offset = U32(bytes, 28), size = U32(bytes, 92);
    if (offset != 160ull + headerBytes || offset > bytes.size() || size != bytes.size() - offset)
        Fail("MINIMAP_SAVE_ENCODING", "自动小地图JPEG头和单mip范围不一致。");
    for (std::size_t level = 1; level < 16; ++level)
        if (U32(bytes, 28 + level * 4) || U32(bytes, 92 + level * 4))
            Fail("MINIMAP_SAVE_ENCODING", "自动小地图不得携带额外mip。");
    auto decoded = Take(Image::DecodeBlp(bytes));
    if (!decoded.Valid() || decoded.width != 256 || decoded.height != 256 || decoded.rgba.capacity() > 2 * kDecodedBytes)
        Fail("MINIMAP_SAVE_ENCODING", "自动小地图真实解码尺寸或容量错误。");
    for (std::size_t index = 3; index < decoded.rgba.size(); index += 4)
        if (decoded.rgba[index] != std::byte{255}) Fail("MINIMAP_SAVE_ENCODING", "自动小地图真实解码必须完全不透明。");
    return {decoded.width, decoded.height, decoded.rgba.size(), decoded.rgba.capacity(), Take(Sha256(decoded.rgba))};
}
void MatchObjectMembers(const Render::ClassicMinimapObjectAssets& assets, const MapArchiveSession& archive,
    const MapSaveTask* task, bool saved) {
    const auto& facts = assets.CapturedMembers();
    std::array<std::string, 2> names{facts[0].memberName, facts[1].memberName};
    const auto actual = Capture(archive, names, Render::ClassicMinimapObjectAssetLimits{}.maxObjectMemberBytes * 2);
    for (std::size_t index = 0; index < facts.size(); ++index) {
        const auto& expected = saved ? facts[index].currentSha256 : facts[index].sourceSha256;
        if (actual[index].present != !expected.empty()
            || (actual[index].present && Take(Sha256(actual[index].bytes)) != expected))
            Fail("MINIMAP_SAVE_OBJECT_SOURCE", "自动小地图对象成员来源或回读与真实捕获不一致。");
        if (!task) continue;
        const auto key = Key(names[index]);
        RequireNotRemoved(*task, key);
        const auto* replacement = Replacement(*task, key);
        const auto finalSha = replacement ? Take(Sha256(replacement->data)) : expected;
        if (finalSha != facts[index].currentSha256)
            Fail("MINIMAP_SAVE_OBJECT_STALE", "最终DOO成员与自动小地图本次捕获的对象不一致。");
    }
}
void NoTga(const MapSaveTask& task, const MapArchiveSession& archive) {
    const std::array<std::string, 1> names{"war3mapMap.tga"};
    // 只问真实存在性，不读取或解析作者可能很大的TGA正文。
    ArchiveExactMemberLimits limits; limits.memberCount = 1;
    const auto identity = Take(archive.DescribeExactMemberIdentities(archive.SourceSha256(), names, limits));
    if (identity.members[0].present || Replacement(task, "war3mapmap.tga"))
        Fail("MINIMAP_SAVE_TGA_PRESENT", "已有TGA小地图必须精确保留，不允许同时补写BLP。");
}
std::string ManifestIdentity(const Render::ClassicMinimapManifest& manifest) {
    // 长度前缀避免字符串边界歧义；摘要绑定本次保存的来源、范围、对象、
    // 像素和编码结果，不把BLP摘要改名为“manifest摘要”。
    std::string text = "classic-minimap-save-manifest-v1\n";
    for (const auto* value : {&manifest.sourceSha256, &manifest.terrainSha256, &manifest.objectDomainSha256,
        &manifest.objectDomainPolicy, &manifest.objectCaptureIdentitySha256, &manifest.rawRgbaSha256,
        &manifest.preNoiseRgbaSha256, &manifest.finalRgbaSha256, &manifest.blpSha256, &manifest.noiseSamplesSha256})
        text += std::to_string(value->size()) + ":" + *value + "\n";
    for (const auto value : manifest.sourceRectangle) text += std::to_string(value) + ",";
    text += "\n" + std::to_string(manifest.postprocessFacts.medianRadius) + ","
        + std::to_string(manifest.postprocessFacts.blurRadius) + ","
        + std::to_string(manifest.postprocessFacts.noisePercent) + ","
        + (manifest.postprocessFacts.noiseSeed ? std::to_string(*manifest.postprocessFacts.noiseSeed) : "none");
    return Take(Sha256(std::as_bytes(std::span(text))));
}
} // namespace

Result<bool> TerrainMinimapSave::RequiresAutomaticGeneration(const MapSaveTask& task) {
    try {
        SourceTask(task);
        const std::array<std::string, 2> names{"war3mapMap.tga", "war3mapMap.blp"};
        ArchiveExactMemberLimits limits; limits.memberCount = 2;
        const auto identities = Take(task.archive->DescribeExactMemberIdentities(task.sourceSha256, names, limits));
        if (identities.members[0].present || Replacement(task, "war3mapmap.tga")) return Result<bool>::Success(false);
        if (!identities.members[1].present) return Result<bool>::Success(true);
        for (const auto& replacement : task.replacements) {
            const auto key = Key(replacement.name);
            if (AffectsMinimap(key))
                return Result<bool>::Success(true);
        }
        for (const auto& removed : task.removals)
            if (AffectsMinimap(Key(removed))) return Result<bool>::Success(true);
        return Result<bool>::Success(false);
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::exception&) {
        return Result<bool>::Failure(kSubsystem, "MINIMAP_SAVE_SOURCE", "无法准确确定小地图源成员状态，拒绝保存。");
    }
}

Result<std::shared_ptr<const TerrainMinimapSave>> TerrainMinimapSave::PrepareAutomatic(
    const MapSaveTask& task, const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const TerrainMinimapSaveLimits& limits) {
    using Output = Result<std::shared_ptr<const TerrainMinimapSave>>;
    try {
        if (!Take(RequiresAutomaticGeneration(task))) return Output::Success({});
        if (task.executed) Fail("MINIMAP_SAVE_SOURCE", "已执行任务不能重新生成小地图。");
        if (Replacement(task, "war3mapmap.blp"))
            Fail("MINIMAP_SAVE_OUTPUT_CONFLICT", "保存列表已经手动提供MapBLP，不能和自动重建混用。");
        RequireNotRemoved(task, "war3mapmap.blp");
        NoTga(task, *task.archive);
        auto available = Available(limits, Add(TaskCapacity(task), kReadReserve));
        auto planLimits = limits.plan; planLimits.maximumBytes = std::min(planLimits.maximumBytes, available);
        auto plan = std::make_shared<MapData::TerrainSavePlan>(Take(MapData::TerrainSavePlan::Prepare(*task.archive, edits, planLimits)));
        Check(plan->VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals));
        auto terrain = Take(Render::CaptureTerrainMinimapAssets(*task.archive, edits, gameRoot,
            limits.assets, Render::TerrainMinimapRangePolicy::ClassicPlayable));
        auto objectLimits = limits.objects;
        objectLimits.maxWorkingBytes = std::min(objectLimits.maxWorkingBytes, Available(limits, Add(TaskCapacity(task), plan->OwnedCapacityBytes())));
        auto assets = Take(Render::CaptureClassicMinimapObjectAssets(std::move(terrain), *task.archive, edits,
            objectLimits, task.replacements, task.removals));
        auto candidateLimits = limits.classic;
        candidateLimits.maxWorkingBytes = std::min(candidateLimits.maxWorkingBytes,
            Available(limits, Add(Add(TaskCapacity(task), plan->OwnedCapacityBytes()), kReadReserve)));
        Render::TerrainMinimapPostprocessSettings postprocess;
        // 原版的进程级rand状态不是地图内容。每次保存使用新的显式种子，
        // 既保持10%噪点，也在本次manifest中保留复现依据。
        postprocess.noiseSeed = std::random_device{}();
        auto candidate = Take(Render::BuildClassicMinimapCandidate(assets, postprocess, candidateLimits));
        if (candidate.manifest.blpSha256 != Take(Sha256(candidate.encoded.bytes))
            || candidate.manifest.terrainSha256 != assets.TerrainAssets().Binding().currentTerrainSha256)
            Fail("MINIMAP_SAVE_ENCODING", "自动小地图生成结果未绑定实际地形和编码字节。");
        auto decoded = DecodeClassic(candidate.encoded.bytes);
        Check(plan->AdmitGeneratedMinimap(edits, std::move(candidate.encoded.bytes)));
        auto output = std::shared_ptr<TerrainMinimapSave>(new TerrainMinimapSave);
        output->automatic_ = true; output->limits_ = limits; output->limits_.objects = objectLimits;
        output->classicAssets_ = std::make_unique<const Render::ClassicMinimapObjectAssets>(std::move(assets));
        output->classicManifest_ = std::move(candidate.manifest); output->plan_ = std::move(plan);
        output->decodedMip0_ = std::move(decoded);
        output->sourcePath_ = Path(task.sourcePath); output->targetPath_ = Path(task.target);
        output->taskIdentity_ = &task; output->archiveIdentity_ = task.archive.get();
        // 旧回执字段保留为实际编码的通用事实；ClassicManifest才是本次算法完整清单。
        output->manifest_.rangePolicy = Render::TerrainMinimapRangePolicy::ClassicPlayable;
        output->manifest_.currentW3eSha256 = output->classicManifest_->terrainSha256;
        output->manifest_.outputBlpSha256 = output->classicManifest_->blpSha256;
        output->manifest_.outputRgbaSha256 = output->classicManifest_->finalRgbaSha256;
        output->manifest_.encodedMipCount = 1; output->manifest_.encodedTotalMipPixels = 256 * 256;
        output->manifest_.encodedBlpBytes = output->MinimapReplacement().data.size();
        output->manifest_.manifestSha256 = ManifestIdentity(*output->classicManifest_);
        return Output::Success(std::move(output));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::exception&) {
        return Output::Failure(kSubsystem, "MINIMAP_SAVE_AUTOMATIC_EXCEPTION", "自动小地图生成未能完成；不返回部分保存资格。");
    }
}

Status TerrainMinimapSave::VerifyAutomaticCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot) const {
    return Guard([&] {
        Check(VerifyAutomaticFrozenOutput(task));
        Check(plan_->VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals));
        Check(Render::VerifyTerrainMinimapAssetsCurrent(classicAssets_->TerrainAssets(), *task.archive, edits, gameRoot,
            limits_.assets));
        MatchObjectMembers(*classicAssets_, *task.archive, &task, false);
        // 当前编辑身份同时由Terrain资产和plan复核；对象当前字节还要来自该会话，
        // 不能只比较源DOO或最终task哈希。
        for (std::size_t index = 0; index < 2; ++index) {
            const auto document = Take(edits.Snapshot(index == 0 ? MapDataKind::Units : MapDataKind::Doodads,
                limits_.objects.maxCurrentSnapshotBytes));
            const auto& member = document.Member(index == 0 ? MapDataKind::Units : MapDataKind::Doodads);
            const auto& expected = classicAssets_->CapturedMembers()[index].currentSha256;
            if (member.state == MapDataState::Missing) {
                if (!expected.empty()) Fail("MINIMAP_SAVE_OBJECT_STALE", "当前对象成员已不再存在。");
            } else if (member.sources.size() != 1 || Take(Sha256(member.sources[0].data)) != expected)
                Fail("MINIMAP_SAVE_OBJECT_STALE", "当前对象成员与自动小地图冻结输入不同。");
        }
    });
}

Status TerrainMinimapSave::VerifyAutomaticFrozenOutput(const MapSaveTask& task) const {
    return Guard([&] {
        SourceTask(task);
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || Path(task.sourcePath) != sourcePath_
            || Path(task.target) != targetPath_ || task.sourceSha256 != plan_->SourceSha256())
            Fail("MINIMAP_SAVE_SOURCE", "自动小地图资格不属于本次冻结任务或目标。");
        Check(plan_->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals));
        NoTga(task, *task.archive);
        MatchObjectMembers(*classicAssets_, *task.archive, &task, false);
        Check(Render::VerifyTerrainMinimapAssetsFrozenOutput(classicAssets_->TerrainAssets(), *task.archive,
            task.replacements, task.removals, limits_.assets));
    });
}

Status TerrainMinimapSave::VerifyAutomaticReadback(const MapSaveTask& task, const MapArchiveSession& saved) const {
    return Guard([&] {
        // 原地保存后原路径已经是新图，不能再重读旧source；owned archive和任务
        // 身份仍应相同，saved则必须是实际目标。
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || task.sourceSha256 != plan_->SourceSha256()
            || Path(task.target) != targetPath_ || Path(saved.SourcePath()) != targetPath_)
            Fail("MINIMAP_SAVE_TARGET", "自动小地图回读不是本次真实保存目标。");
        Check(plan_->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals));
        Check(plan_->VerifyReadback(saved, limits_.plan));
        Check(Render::VerifyTerrainMinimapAssetsReadback(classicAssets_->TerrainAssets(), saved, limits_.assets));
        MatchObjectMembers(*classicAssets_, saved, nullptr, true);
        NoTga(task, saved);
        const std::array<std::string, 1> names{"war3mapMap.blp"};
        const auto members = Capture(saved, names, limits_.classic.maxBlpBytes);
        if (!members[0].present || members[0].bytes != MinimapReplacement().data)
            Fail("MINIMAP_SAVE_ENCODING", "保存后的MapBLP与本次生成字节不一致。");
        const auto decoded = DecodeClassic(members[0].bytes);
        if (decoded.decodedRgbaSha256 != decodedMip0_.decodedRgbaSha256)
            Fail("MINIMAP_SAVE_ENCODING", "保存后MapBLP真实解码与生成阶段不一致。");
    });
}
} // namespace War3Map::Services

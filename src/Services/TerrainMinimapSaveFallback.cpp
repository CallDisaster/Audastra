#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <iterator>
#include <new>
#include <string_view>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "TerrainMinimapSave";
const std::array<std::string, 2> kMinimapMembers{"war3mapMap.blp", "war3mapMap.tga"};
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure(kSubsystem, code, message).diagnostics};
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class Function> Status Guard(Function&& function) {
    try { function(); return Status::Ok(); }
    catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "MINIMAP_PRESERVE_ALLOCATION", "保留小地图的来源复核分配失败。");
    }
}
std::filesystem::path FixedPath(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || path.native().size() > 32767)
        Fail("MINIMAP_PRESERVE_SOURCE", "保留小地图需要有界的绝对来源和目标路径。");
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error).native();
    if (error) Fail("MINIMAP_PRESERVE_SOURCE", "无法固定保留小地图的来源或目标路径。");
    std::transform(canonical.begin(), canonical.end(), canonical.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return std::filesystem::path(std::move(canonical));
}
void TaskSource(const MapSaveTask& task) {
    if (!task.archive || task.sourceSha256 != task.archive->SourceSha256()
        || FixedPath(task.sourcePath) != FixedPath(task.archive->SourcePath()))
        Fail("MINIMAP_PRESERVE_SOURCE", "保留小地图必须来自本次 FreezeArchive 的真实归档。");
    if (task.inPlace != (FixedPath(task.sourcePath) == FixedPath(task.target)))
        Fail("MINIMAP_PRESERVE_TARGET", "保留小地图的目标与原地保存标记不一致。");
    if (task.replacements.size() > 16384 || task.removals.size() > 16384)
        Fail("MINIMAP_PRESERVE_DOMAIN", "最终保存成员数量超过小地图来源复核上限。");
}
void Preparing(const MapSaveTask& task) {
    if (task.executed && !task.InFinalDerivedStage())
        Fail("MINIMAP_PRESERVE_ALREADY_EXECUTED", "已执行任务不能重新准备小地图保留资格。");
}
std::vector<ArchiveExactMemberIdentity> Identities(const MapArchiveSession& archive) {
    ArchiveExactMemberLimits limits;
    limits.memberCount = kMinimapMembers.size();
    return Take(archive.DescribeExactMemberIdentities(archive.SourceSha256(), kMinimapMembers, limits)).members;
}
void SameMembers(std::span<const ArchiveExactMemberIdentity> expected,
    std::span<const ArchiveExactMemberIdentity> actual) {
    if (expected.size() != kMinimapMembers.size() || actual.size() != expected.size())
        Fail("MINIMAP_PRESERVE_IDENTITY", "小地图来源身份数量不完整。");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& before = expected[index]; const auto& after = actual[index];
        if (before.name != after.name || before.present != after.present || before.byteSize != after.byteSize
            || before.sha256 != after.sha256)
            Fail("MINIMAP_PRESERVE_MEMBER_CHANGED", "原小地图的存在性或实际字节已改变，拒绝继续保存。");
    }
}
void FinalMembers(const MapSaveTask& task, std::span<const ArchiveExactMemberIdentity> members) {
    if (members.size() != kMinimapMembers.size())
        Fail("MINIMAP_PRESERVE_IDENTITY", "小地图来源身份不完整。");
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto wanted = Take(Resources::CanonicalImportPath(kMinimapMembers[index]));
        const MemberReplacement* replacement = nullptr;
        for (const auto& member : task.replacements) {
            if (Take(Resources::CanonicalImportPath(member.name)) != wanted) continue;
            if (replacement) Fail("MINIMAP_PRESERVE_CONFLICT", "最终列表重复替换受保护的小地图成员。");
            replacement = &member;
        }
        for (const auto& removed : task.removals)
            if (Take(Resources::CanonicalImportPath(removed)) == wanted)
                Fail("MINIMAP_PRESERVE_MEMBER_CHANGED", "最终列表删除了受保护的小地图成员。");
        if (!replacement) continue;
        const auto& original = members[index];
        if (!original.present || !original.byteSize || !original.sha256
            || replacement->data.size() != *original.byteSize || Take(Sha256(replacement->data)) != *original.sha256)
            Fail("MINIMAP_PRESERVE_MEMBER_CHANGED", "最终列表改写或新增了小地图，不能作为保留原图继续保存。");
    }
}

bool CanPreserveFor(const Diagnostic& diagnostic) {
    // 明确列出生成器的能力与有界资源不足。任何新增错误默认继续拒绝；
    // 不匹配 *_STALE/*IDENTITY，不将 source/I/O/opaque/内容不一致泛化降级。
    constexpr std::string_view allowed[]{
        std::string_view{"CLASSIC_OBJECT_CAPTURE_SPECIAL_UNRESOLVED"},
        "CLASSIC_OBJECT_CAPTURE_LAYOUT", "CLASSIC_MINIMAP_GENERATION_UNSUPPORTED",
        "CLASSIC_MINIMAP_PLAYABLE_RECT_UNRESOLVED", "CLASSIC_MINIMAP_CLIFF_UNRESOLVED",
        "CLASSIC_MINIMAP_UNKNOWN_FLAGS", "CLASSIC_MINIMAP_WATER", "CLASSIC_MINIMAP_WATER_MISSING",
        "CLASSIC_MINIMAP_CLIFF_MISSING", "TERRAIN_ASSETS_UNKNOWN_GROUND", "RESOURCE_NOT_FOUND",
        "CLASSIC_MINIMAP_CONFIG_MISSING", "CLASSIC_MINIMAP_OBJECT_CATALOG_EMPTY",
        "CLASSIC_MINIMAP_OBJECT_INVALID_MODEL_CONFIG", "CLASSIC_OBJECT_LAYER_INVALID_MODEL_CONFIG_MISSING",
        "TGA_FORMAT_UNSUPPORTED", "BLP_MIP_ALPHA_UNSUPPORTED", "MDX_VERSION_UNSUPPORTED",
        "PLACEMENT_VERSION_UNSUPPORTED", "TERRAIN_VERSION_UNSUPPORTED", "MAP_TERRAIN_READ_ONLY", "W3I_UNSUPPORTED_VERSION",
        "PATHING_VERSION_UNSUPPORTED", "PATHING_BYTE_LIMIT", "PATHING_GRID_LIMIT", "CLASSIC_MINIMAP_W3I_LAYOUT",
        "IMPORT_VERSION_UNSUPPORTED", "IMPORT_BUDGET_EXCEEDED",
        "MINIMAP_SAVE_BUDGET", "TERRAIN_SAVE_PLAN_BUDGET", "TERRAIN_ASSETS_BUDGET",
        "EXACT_MEMBER_LIMIT", "FILE_READ_BUDGET_EXCEEDED",
        "CLASSIC_OBJECT_CAPTURE_BUDGET", "CLASSIC_OBJECT_LAYER_DEPENDENCY_BUDGET",
        "CLASSIC_OBJECT_LAYER_RESOURCE_BUDGET", "CLASSIC_MINIMAP_OBJECT_FOOTPRINT_BUDGET",
        "CLASSIC_MINIMAP_OBJECT_MODEL_BUDGET", "CLASSIC_MINIMAP_OBJECT_PATHING_BUDGET",
        "CLASSIC_MINIMAP_OBJECT_PIXEL_BUDGET", "CLASSIC_MINIMAP_BUDGET", "CLASSIC_MINIMAP_W3E_BUDGET",
        "TERRAIN_MINIMAP_BUDGET", "TERRAIN_MINIMAP_POSTPROCESS_BUDGET",
        "TERRAIN_MINIMAP_POSTPROCESS_ASPECT_UNREPRESENTABLE", "TERRAIN_MINIMAP_POSTPROCESS_FILTER_UNSUPPORTED",
        "TERRAIN_TABLE_BUDGET", "TERRAIN_MATERIAL_TABLE_BUDGET", "TERRAIN_MATERIAL_TEXTURE_BUDGET", "TERRAIN_MATERIAL_TILESET_BUDGET",
        "TERRAIN_ATLAS_INPUT_BUDGET", "TERRAIN_ATLAS_RGBA_BUDGET", "TERRAIN_ATLAS_MIP_BUDGET",
        "TERRAIN_ATLAS_RETAINED_BUDGET", "TERRAIN_ATLAS_WORKING_BUDGET", "TERRAIN_ATLAS_JPEG_BUDGET",
        "TERRAIN_ATLAS_ALPHA_UNSUPPORTED", "MDX_LIMIT",
        "MAP_SNAPSHOT_BUDGET", "TERRAIN_MEMORY_LIMIT", "TERRAIN_TILESET_LIMIT", "TERRAIN_BYTE_LIMIT",
        "TERRAIN_DIMENSION_LIMIT", "TERRAIN_VERTEX_LIMIT", "TERRAIN_TILE_LIMIT",
        "BLP_MIP_INPUT_BUDGET", "BLP_MIP_RGBA_BUDGET", "BLP_MIP_MIP_BUDGET",
        "BLP_MIP_RETAINED_BUDGET", "BLP_MIP_JPEG_BUDGET", "BLP_MIP_WORKING_BUDGET",
        "BLP_JPEG_WORKING_BUDGET", "BLP_JPEG_OUTPUT_BUDGET"};
    return std::find(std::begin(allowed), std::end(allowed), diagnostic.code) != std::end(allowed);
}
} // namespace

Status TerrainMinimapPreserveGuard::VerifyCurrent(const MapSaveTask& task,
    const MapData::MapDataEditSession& edits) const {
    return Guard([&] {
        if (edits.SessionIdentity() != sessionIdentity_ || edits.Revision() != revision_
            || edits.TerrainSourceRevision() != terrainSourceRevision_)
            Fail("MINIMAP_PRESERVE_STALE", "保留小地图的编辑会话或修订已变化。");
        Check(VerifyFrozenOutput(task));
    });
}

Status TerrainMinimapPreserveGuard::VerifyFrozenOutput(const MapSaveTask& task) const {
    return Guard([&] {
        TaskSource(task);
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_
            || task.sourceSha256 != sourceSha256_ || FixedPath(task.sourcePath) != sourcePath_
            || FixedPath(task.target) != targetPath_)
            Fail("MINIMAP_PRESERVE_IDENTITY", "保留小地图的资格不属于本次冻结任务或目标。");
        FinalMembers(task, members_);
        SameMembers(members_, Identities(*task.archive));
    });
}

Status TerrainMinimapPreserveGuard::VerifyReadback(const MapSaveTask& task, const MapArchiveSession& saved) const {
    return Guard([&] {
        // 原地保存完成后旧路径已是新归档；此处只核 owned task 身份与实际
        // 输出，不通过重读旧 source 的方式把正常原地保存误判为过期。
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || task.sourceSha256 != sourceSha256_
            || FixedPath(task.target) != targetPath_ || FixedPath(saved.SourcePath()) != targetPath_)
            Fail("MINIMAP_PRESERVE_IDENTITY", "回读的小地图不属于本次真实保存目标。");
        FinalMembers(task, members_);
        SameMembers(members_, Identities(saved));
    });
}

Result<TerrainMinimapAutomaticPreparation> TerrainMinimapSave::PrepareAutomaticOrPreserve(
    const MapSaveTask& task, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot, const TerrainMinimapSaveLimits& limits) {
    using Output = Result<TerrainMinimapAutomaticPreparation>;
    try {
        Preparing(task);
        TaskSource(task);
        auto preserve = std::shared_ptr<TerrainMinimapPreserveGuard>(new TerrainMinimapPreserveGuard);
        preserve->taskIdentity_ = &task; preserve->archiveIdentity_ = task.archive.get();
        preserve->sourcePath_ = FixedPath(task.sourcePath); preserve->targetPath_ = FixedPath(task.target);
        preserve->sourceSha256_ = task.sourceSha256; preserve->sessionIdentity_ = edits.SessionIdentity();
        preserve->revision_ = edits.Revision(); preserve->terrainSourceRevision_ = edits.TerrainSourceRevision();
        preserve->members_ = Identities(*task.archive);

        auto generated = PrepareAutomatic(task, edits, gameRoot, limits);
        if (generated.Succeeded() && generated.value) {
            TerrainMinimapAutomaticPreparation result{std::move(*generated.value), {}};
            if (!result.generated && preserve->members_[1].present) {
                Check(preserve->VerifyCurrent(task, edits));
                result.preserved = std::move(preserve);
            }
            auto success = Output::Success(std::move(result)); success.diagnostics = std::move(generated.diagnostics);
            return success;
        }
        Check(preserve->VerifyCurrent(task, edits));
        return PreserveUnsupportedAutomatic(task, edits, std::move(generated.diagnostics));
    } catch (Failure& failure) { return {std::nullopt, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Output::Failure(kSubsystem, "MINIMAP_PRESERVE_ALLOCATION", "无法取得小地图保留来源，不允许降级继续保存。");
    }
}

Result<TerrainMinimapAutomaticPreparation> TerrainMinimapSave::PreserveUnsupportedAutomatic(
    const MapSaveTask& task, const MapData::MapDataEditSession& edits,
    std::vector<Diagnostic> diagnostics) {
    using Output = Result<TerrainMinimapAutomaticPreparation>;
    try {
        bool sawError = false;
        for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) {
            sawError = true;
            if (!CanPreserveFor(diagnostic)) return {std::nullopt, std::move(diagnostics)};
        }
        if (!sawError)
            return Output::Failure(kSubsystem, "MINIMAP_PRESERVE_REASON_REQUIRED", "保留小地图的降级需要明确的生成能力或预算错误。");
        Preparing(task);
        TaskSource(task);
        auto preserve = std::shared_ptr<TerrainMinimapPreserveGuard>(new TerrainMinimapPreserveGuard);
        preserve->taskIdentity_ = &task; preserve->archiveIdentity_ = task.archive.get();
        preserve->sourcePath_ = FixedPath(task.sourcePath); preserve->targetPath_ = FixedPath(task.target);
        preserve->sourceSha256_ = task.sourceSha256; preserve->sessionIdentity_ = edits.SessionIdentity();
        preserve->revision_ = edits.Revision(); preserve->terrainSourceRevision_ = edits.TerrainSourceRevision();
        preserve->members_ = Identities(*task.archive);
        Check(preserve->VerifyCurrent(task, edits));
        for (auto& diagnostic : diagnostics)
            if (diagnostic.severity == DiagnosticSeverity::Error) diagnostic.severity = DiagnosticSeverity::Warning;
        diagnostics.push_back({DiagnosticSeverity::Warning, kSubsystem, "MINIMAP_GENERATION_SKIPPED",
            preserve->members_[0].present || preserve->members_[1].present
                ? "自动小地图暂不支持当前内容或超出生成预算；本次保留原小地图，继续保存其它已通过检查的编辑。"
                : "自动小地图暂不支持当前内容或超出生成预算；原图缺少小地图，本次不补写，继续保存其它已通过检查的编辑。"});
        auto success = Output::Success({{}, std::move(preserve)});
        success.diagnostics = std::move(diagnostics); return success;
    } catch (Failure& failure) { return {std::nullopt, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Output::Failure(kSubsystem, "MINIMAP_PRESERVE_ALLOCATION", "无法取得小地图保留来源，不允许降级继续保存。");
    }
}
} // namespace War3Map::Services

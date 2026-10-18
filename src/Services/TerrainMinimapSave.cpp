#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {

constexpr const char* kSubsystem = "TerrainMinimapSave";
constexpr std::size_t kMiB = 1024 * 1024, kHashReserve = kMiB, kMetadataReserve = 2 * kMiB;
constexpr std::size_t kEncodedReserve = Image::BlpOpaqueQuantizeLimits{}.maxOutputBytes;
constexpr std::size_t kDecodedBytes = 256 * 256 * 4;
constexpr std::array<std::string_view, 9> kOriginalOnly{
    "war3map.j", "war3map.lua", "war3map.w3u", "war3map.w3t", "war3map.w3b",
    "war3map.w3d", "war3map.w3a", "war3map.w3h", "war3map.w3q"};

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure(kSubsystem, code, message).diagnostics};
}
void Check(const Status& status) { if (!status.Succeeded()) throw Failure{status.diagnostics}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class Function> Status GuardStatus(Function&& function) {
    try { function(); return Status::Ok(); }
    catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "MINIMAP_SAVE_ALLOCATION", "小地图保存复核分配失败，拒绝发布。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "MINIMAP_SAVE_ALLOCATION", "小地图保存复核容量异常，拒绝发布。"); }
}
void Charge(std::size_t& total, std::size_t count, std::size_t element = 1) {
    if (element && count > (std::numeric_limits<std::size_t>::max() - total) / element)
        Fail("MINIMAP_SAVE_BUDGET", "小地图保存容量乘加溢出。");
    total += count * element;
}
std::size_t Remaining(std::size_t maximum, std::size_t used) {
    if (used >= maximum) Fail("MINIMAP_SAVE_BUDGET", "小地图保存重叠容器或临时工作超过共享预算。");
    return maximum - used;
}
void Within(std::size_t maximum, std::size_t used) {
    if (used > maximum) Fail("MINIMAP_SAVE_BUDGET", "小地图保存可观察capacity超过共享预算。");
}
std::size_t Sum(std::initializer_list<std::size_t> values) {
    std::size_t result = 0; for (const auto value : values) Charge(result, value); return result;
}
void TextCapacity(std::size_t& total, const std::string& text) { Charge(total, text.capacity()); Charge(total, 1); }
void PathCapacity(std::size_t& total, const std::filesystem::path& path) {
    Charge(total, path.native().capacity() + 1, sizeof(std::filesystem::path::value_type));
}
void LocationCapacity(std::size_t& total, const Resources::ResourceLocation& location) {
    TextCapacity(total, location.canonicalPath); TextCapacity(total, location.sourceId); TextCapacity(total, location.sourcePath);
}
void BindingCapacity(std::size_t& total, const Render::TerrainInspectionBinding& binding) {
    TextCapacity(total, binding.documentId); TextCapacity(total, binding.sourceSha256); TextCapacity(total, binding.terrainSha256);
}
void MipCapacity(std::size_t& total, const Render::TerrainMinimapMipFacts& mip) { TextCapacity(total, mip.decodedRgbaSha256); }

std::size_t SnapshotCapacity(const Render::TerrainMinimapAssetSnapshot& snapshot) {
    std::size_t total = sizeof(snapshot);
    const auto& input = snapshot.Input();
    BindingCapacity(total, input.binding); Charge(total, input.currentW3e.capacity());
    if (input.sourceW3i) Charge(total, input.sourceW3i->capacity());
    Charge(total, input.groundAtlases.capacity(), sizeof(input.groundAtlases.front()));
    for (std::size_t index = 0; index < input.groundAtlases.size(); ++index) {
        const auto& pointer = input.groundAtlases[index];
        if (std::find(input.groundAtlases.begin(), input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(index), pointer)
            != input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(index)) continue;
        const auto& atlas = *pointer;
        Charge(total, sizeof(atlas)); LocationCapacity(total, atlas.source); TextCapacity(total, atlas.sourceSha256);
        Charge(total, atlas.image.rgba.capacity()); Charge(total, atlas.additionalMips.capacity(), sizeof(Image::DecodedImage));
        for (const auto& mip : atlas.additionalMips) Charge(total, mip.rgba.capacity());
        Charge(total, atlas.cells.capacity(), sizeof(Render::TerrainAtlasCell));
    }
    const auto& binding = snapshot.Binding();
    TextCapacity(total, binding.archiveSha256); TextCapacity(total, binding.originalTerrainSha256);
    TextCapacity(total, binding.currentTerrainSha256); PathCapacity(total, binding.gameRoot);
    Charge(total, snapshot.Dependencies().capacity(), sizeof(Render::TerrainMinimapAssetDependency));
    for (const auto& dependency : snapshot.Dependencies()) {
        LocationCapacity(total, dependency.winner); TextCapacity(total, dependency.containerIdentity);
        TextCapacity(total, dependency.encodedSha256);
        Charge(total, dependency.decodedMips.capacity(), sizeof(Render::TerrainMinimapMipFacts));
        for (const auto& mip : dependency.decodedMips) MipCapacity(total, mip);
    }
    Charge(total, snapshot.Tables().capacity(), sizeof(Render::TerrainMinimapTableFacts));
    for (const auto& table : snapshot.Tables()) { TextCapacity(total, table.idColumn); TextCapacity(total, table.identitySha256); }
    Charge(total, snapshot.GroundAssets().capacity(), sizeof(Render::TerrainMinimapGroundAsset));
    for (const auto& ground : snapshot.GroundAssets()) { TextCapacity(total, ground.rawcode); TextCapacity(total, ground.canonicalTexturePath); }
    TextCapacity(total, snapshot.IdentitySha256());
    return total;
}
std::size_t ManifestCapacity(const Render::TerrainMinimapManifest& manifest) {
    std::size_t total = sizeof(manifest); BindingCapacity(total, manifest.binding);
    for (const auto* text : {&manifest.currentW3eSha256, &manifest.reserializedW3eSha256, &manifest.meshSha256,
        &manifest.outputRgbaSha256, &manifest.outputBlpSha256, &manifest.manifestSha256,
        &manifest.w3i.state, &manifest.w3i.stage, &manifest.w3i.sha256,
        &manifest.mapEdge.state, &manifest.mapEdge.maskSha256}) TextCapacity(total, *text);
    Charge(total, manifest.atlases.capacity(), sizeof(Render::TerrainMinimapAtlasFacts));
    for (const auto& atlas : manifest.atlases) {
        LocationCapacity(total, atlas.declaredSource); TextCapacity(total, atlas.declaredEncodedSha256);
        Charge(total, atlas.mips.capacity(), sizeof(Render::TerrainMinimapMipFacts));
        for (const auto& mip : atlas.mips) MipCapacity(total, mip);
        Charge(total, atlas.cells.capacity(), sizeof(Render::TerrainAtlasCell));
    }
    return total;
}
std::size_t TaskReplacementCapacity(const MapSaveTask& task) {
    std::size_t total = sizeof(MemberReplacement) * 2;
    Charge(total, task.replacements.capacity(), sizeof(MemberReplacement));
    for (const auto& replacement : task.replacements) { TextCapacity(total, replacement.name); Charge(total, replacement.data.capacity()); }
    return total;
}
std::string Canonical(std::string_view text) {
    if (text.empty() || text.size() > 1024 || text.find('\0') != std::string_view::npos)
        Fail("MINIMAP_SAVE_DOMAIN", "保存列表的成员名必须非空且有界。");
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    return result;
}
std::filesystem::path Path(const std::filesystem::path& path, const char* code) {
    if (path.empty() || !path.is_absolute() || path.native().size() > 32767)
        Fail(code, "小地图保存要求有界的绝对源/目标路径。");
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) Fail(code, "小地图保存无法冻结路径的真实父目录身份。");
    auto native = result.native();
    std::transform(native.begin(), native.end(), native.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return std::filesystem::path(std::move(native));
}
void Limits(const TerrainMinimapSaveLimits& limits) {
    const TerrainMinimapSaveLimits hard;
    const auto cap = [](std::size_t value, std::size_t maximum) {
        if (!value || value > maximum) Fail("MINIMAP_SAVE_BUDGET", "小地图保存及其子预算必须为正且只能收紧硬上限。");
    };
    cap(limits.maxWorkingBytes, hard.maxWorkingBytes); cap(limits.plan.maximumBytes, hard.plan.maximumBytes);
    const auto& a = limits.assets; const auto& h = hard.assets;
    cap(a.maxSourceBytes, h.maxSourceBytes); cap(a.maxCurrentW3eBytes, h.maxCurrentW3eBytes);
    cap(a.maxTiles, h.maxTiles); cap(a.maxRetainedAtlasCapacityBytes, h.maxRetainedAtlasCapacityBytes);
    cap(a.tables.maxTableBytes, h.tables.maxTableBytes); cap(a.tables.maxRows, h.tables.maxRows);
    cap(a.tables.maxCells, h.tables.maxCells); cap(a.tables.maxCellBytes, h.tables.maxCellBytes);
    cap(a.atlas.maxInputBytes, h.atlas.maxInputBytes); cap(a.atlas.maxDimension, h.atlas.maxDimension);
    cap(a.atlas.maxRgbaBytes, h.atlas.maxRgbaBytes); cap(a.atlas.maxRetainedRgbaBytes, h.atlas.maxRetainedRgbaBytes);
    cap(a.atlas.maxTotalMipPixels, h.atlas.maxTotalMipPixels); cap(a.atlas.maxWorkingBytes, h.atlas.maxWorkingBytes);
    cap(a.atlas.maxJpegSegments, h.atlas.maxJpegSegments); cap(a.atlas.maxJpegScans, h.atlas.maxJpegScans);
    cap(limits.candidate.maxTiles, hard.candidate.maxTiles); cap(limits.candidate.maxW3eBytes, hard.candidate.maxW3eBytes);
    cap(limits.candidate.maxRetainedInputBytes, hard.candidate.maxRetainedInputBytes);
    cap(limits.candidate.maxWorkingBytes, hard.candidate.maxWorkingBytes);
}

// Capture的三表phase与atlas phase不重叠。表的编码副本/节点/重放/catalog预留
// 按每表16×原字+1024×cell+512×row计；atlas保留+当前decoder工作同时计。
// 通过收紧既有子预算预收，而不是让每个子系统各自使用128MiB。
std::size_t CaptureEnvelope(const Render::TerrainMinimapAssetCaptureLimits& limits,
    Render::TerrainMinimapRangePolicy rangePolicy) {
    std::size_t tables = kMetadataReserve + kHashReserve;
    Charge(tables, limits.maxSourceBytes, 3); Charge(tables, limits.maxCurrentW3eBytes);
    Charge(tables, limits.tables.maxTableBytes, 3 * 16);
    Charge(tables, limits.tables.maxCells, 3 * 1024); Charge(tables, limits.tables.maxRows, 3 * 512);
    std::size_t atlas = kMetadataReserve + kHashReserve;
    Charge(atlas, limits.maxSourceBytes, 3); Charge(atlas, limits.maxCurrentW3eBytes);
    Charge(atlas, limits.maxRetainedAtlasCapacityBytes); Charge(atlas, limits.atlas.maxWorkingBytes);
    if (rangePolicy == Render::TerrainMinimapRangePolicy::FullW3e) return std::max(tables, atlas);
    // W3I在表批次移入snapshot，直到atlas/candidate结束仍保活；不计入三张表的解析预留。
    // 成员读取cap约束size；capacity仍以Assets在解析前核过的maxSourceBytes为界。
    const auto retainedW3i = limits.maxSourceBytes;
    Charge(tables, retainedW3i); Charge(atlas, retainedW3i);
    // Assets在Snapshot/Settings解析前已将64KiB+64×实际W3I字节与原字/current容量
    // 共同预收在maxSourceBytes内。用这个内限界定独立Settings phase，再保守叠加
    // 原字副本及current；不是用64×总source cap挤占不重叠的atlas解码阶段。
    std::size_t settings = kMetadataReserve + kHashReserve;
    Charge(settings, limits.maxSourceBytes, 4); Charge(settings, limits.maxCurrentW3eBytes);
    Charge(settings, retainedW3i);
    return std::max({tables, atlas, settings});
}
Render::TerrainMinimapAssetCaptureLimits CaptureLimits(Render::TerrainMinimapAssetCaptureLimits limits,
    std::size_t available, Render::TerrainMinimapRangePolicy rangePolicy) {
    while (CaptureEnvelope(limits, rangePolicy) > available) {
        if (limits.maxSourceBytes <= 1 || limits.tables.maxCells <= 1 || limits.atlas.maxWorkingBytes <= 1)
            Fail("MINIMAP_SAVE_BUDGET", "剩余共享预算无法容纳最小资源捕获。");
        limits.maxSourceBytes = std::max<std::size_t>(1, limits.maxSourceBytes / 2);
        limits.maxCurrentW3eBytes = std::max<std::size_t>(1, limits.maxCurrentW3eBytes / 2);
        limits.maxRetainedAtlasCapacityBytes = std::max<std::size_t>(1, limits.maxRetainedAtlasCapacityBytes / 2);
        limits.tables.maxTableBytes = std::max<std::size_t>(1, limits.tables.maxTableBytes / 2);
        limits.tables.maxRows = std::max<std::size_t>(1, limits.tables.maxRows / 2);
        limits.tables.maxCells = std::max<std::size_t>(1, limits.tables.maxCells / 2);
        limits.atlas.maxInputBytes = std::max<std::size_t>(1, limits.atlas.maxInputBytes / 2);
        limits.atlas.maxWorkingBytes = std::max<std::size_t>(1, limits.atlas.maxWorkingBytes / 2);
    }
    return limits;
}

void SourceTask(const MapSaveTask& task) {
    if (!task.archive || task.sourceSha256 != task.archive->SourceSha256()
        || Path(task.sourcePath, "MINIMAP_SAVE_SOURCE") != Path(task.archive->SourcePath(), "MINIMAP_SAVE_SOURCE"))
        Fail("MINIMAP_SAVE_SOURCE", "小地图准备需要FreezeArchive真实owned来源，sourcePath/sourceSha必须与owned归档一致。");
    if (task.inPlace || Path(task.target, "MINIMAP_SAVE_TARGET") == Path(task.sourcePath, "MINIMAP_SAVE_SOURCE"))
        Fail("MINIMAP_SAVE_TARGET", "首次missing-only小地图只允许独立绝对SaveCopy目标。");
    std::error_code error;
    if (std::filesystem::exists(task.target, error) || error)
        Fail("MINIMAP_SAVE_TARGET", "missing-only SaveCopy目标已存在或无法准确确定其缺失。");
    if (task.triggerChanges || !task.objectTransactions.empty() || !task.removals.empty() || task.replacements.size() > 12)
        Fail("MINIMAP_SAVE_DOMAIN", "missing-only小地图拒绝触发器/物编混合事务、任意删除或额外成员领域。");
}

void FinalMembers(const MapSaveTask& task, bool beforeAdmission, std::size_t available) {
    std::set<std::string> seen;
    std::vector<std::string> names;
    std::vector<const MemberReplacement*> originals;
    for (const auto& replacement : task.replacements) {
        const auto name = Canonical(replacement.name);
        if (!seen.insert(name).second) Fail("MINIMAP_SAVE_DOMAIN", "保存列表存在大小写/分隔符等价的重复成员。");
        if (name == "war3map.w3e" || name == "war3map.wpm" || (!beforeAdmission && name == "war3mapmap.blp")) continue;
        if (beforeAdmission || std::find(kOriginalOnly.begin(), kOriginalOnly.end(), name) == kOriginalOnly.end())
            Fail("MINIMAP_SAVE_DOMAIN", "missing-only保存只允许计划地形成员及原脚本/七类native模板的原字重现；其它新增/替换拒绝。");
        names.push_back(replacement.name); originals.push_back(&replacement);
    }
    if (names.empty()) return;
    ArchiveExactMemberLimits capture;
    capture.memberCount = kOriginalOnly.size(); capture.totalPathBytes = 12 * capture.pathBytes;
    capture.memberBytes = std::min(capture.memberBytes, available / 3);
    capture.totalBytes = std::min(capture.totalBytes, available / 3);
    if (!capture.totalBytes) Fail("MINIMAP_SAVE_BUDGET", "没有原字模板复核缓冲预算。");
    const auto actual = Take(task.archive->CaptureExactMembers(names, capture));
    std::size_t capacity = sizeof(actual) + kMetadataReserve + kHashReserve;
    Charge(capacity, actual.capacity(), sizeof(ArchiveExactMember));
    for (std::size_t index = 0; index < actual.size(); ++index) {
        TextCapacity(capacity, actual[index].name); Charge(capacity, actual[index].bytes.capacity());
        if (!actual[index].present || actual[index].bytes != originals[index]->data)
            Fail("MINIMAP_SAVE_DOMAIN", "脚本/native模板仅允许准确Present源成员的逐字重现，禁止新增或修改。");
    }
    Within(available, capacity);
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 4; ++shift) value |= std::to_integer<std::uint32_t>(bytes[at + shift]) << (shift * 8);
    return value;
}
Render::TerrainMinimapMipFacts DecodeActual(std::span<const std::byte> encoded) {
    // 先有界检查本单元唯一允许的opaque BLP1全9级精确布局，再交唯一decoder。
    // 整编码SHA及exact字节readback覆盖全部mip；DecodedMip0只声明实际解码过的一级。
    if (encoded.size() != 1180 + 87381 || U32(encoded, 0) != 0x31504c42 || U32(encoded, 4) != 1
        || U32(encoded, 8) != 0 || U32(encoded, 12) != 256 || U32(encoded, 16) != 256
        || U32(encoded, 20) != 5 || U32(encoded, 24) != 1)
        Fail("MINIMAP_SAVE_ENCODING", "实际生成/回读MapBLP不是有界256² opaque BLP1完整链。");
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, dimension = 256; level < 16; ++level) {
        const auto pixels = level < 9 ? dimension * dimension : 0;
        if (U32(encoded, 28 + level * 4) != (pixels ? offset : 0) || U32(encoded, 92 + level * 4) != pixels)
            Fail("MINIMAP_SAVE_ENCODING", "MapBLP九级mip范围、连续性或总长度与实际编码政策不符。");
        offset += pixels;
        dimension = std::max(1u, dimension / 2);
    }
    auto decoded = Take(Image::DecodeBlp(encoded));
    if (!decoded.Valid() || decoded.width != 256 || decoded.height != 256
        || decoded.rgba.capacity() > kDecodedBytes * 2)
        Fail("MINIMAP_SAVE_ENCODING", "实际encoded MapBLP解码尺寸/容量不符。");
    for (std::size_t alpha = 3; alpha < decoded.rgba.size(); alpha += 4)
        if (decoded.rgba[alpha] != std::byte{255}) Fail("MINIMAP_SAVE_ENCODING", "MapBLP实际解码并非opaque。");
    return {decoded.width, decoded.height, decoded.rgba.size(), decoded.rgba.capacity(), Take(Sha256(decoded.rgba))};
}

} // namespace

std::size_t TerrainMinimapSave::RetainedCapacityBytes() const {
    std::size_t total = sizeof(*this);
    Charge(total, plan_->OwnedCapacityBytes()); Charge(total, SnapshotCapacity(*assets_));
    Charge(total, ManifestCapacity(manifest_)); MipCapacity(total, decodedMip0_);
    PathCapacity(total, sourcePath_); PathCapacity(total, targetPath_);
    return total;
}

Result<std::shared_ptr<const TerrainMinimapSave>> TerrainMinimapSave::Prepare(
    const MapSaveTask& task, const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    TerrainMinimapSaveChoice choice, const TerrainMinimapSaveLimits& limits) {
    using Output = Result<std::shared_ptr<const TerrainMinimapSave>>;
    if (choice == TerrainMinimapSaveChoice::Preserve) return Output::Success({});
    if (choice != TerrainMinimapSaveChoice::GenerateMissingGroundOnly
        && choice != TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly)
        return Output::Failure(kSubsystem, "MINIMAP_SAVE_CHOICE", "未知的小地图保存choice。");
    const auto rangePolicy = choice == TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly
        ? Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge
        : Render::TerrainMinimapRangePolicy::FullW3e;
    try {
        Limits(limits); SourceTask(task);
        if (task.executed) Fail("MINIMAP_SAVE_SOURCE", "已执行的保存任务不能重新准备小地图。");
        const auto taskBytes = TaskReplacementCapacity(task);
        // 最终脚本/native原字副本和MapBLP append也可能与Readback重叠。
        // 此上界在工厂冻结；后续最终列表必须落在同一预留内，回读不遗漏task旧副本。
        auto available = Remaining(limits.maxWorkingBytes, Sum({taskBytes, kMetadataReserve, kHashReserve}));
        const auto taskCeiling = Sum({taskBytes, kEncodedReserve, std::min(16 * kMiB, available / 4)});
        available = Remaining(limits.maxWorkingBytes, Sum({taskCeiling, kMetadataReserve, kHashReserve}));
        FinalMembers(task, true, available);
        auto planLimits = limits.plan;
        planLimits.maximumBytes = std::min(planLimits.maximumBytes, available);
        auto plan = std::make_shared<MapData::TerrainSavePlan>(Take(MapData::TerrainSavePlan::Prepare(*task.archive, edits, planLimits)));
        Check(plan->VerifyMissingMinimapEligibility(edits));
        Check(plan->VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals));
        available = Remaining(available, plan->OwnedCapacityBytes());
        auto captureLimits = CaptureLimits(limits.assets, available, rangePolicy);
        auto assets = Take(Render::CaptureTerrainMinimapAssets(*task.archive, edits, gameRoot, captureLimits,
            rangePolicy));
        // Candidate本身已计输入保活；此处再保守计一次，确保snapshot metadata、Plan和task副本
        // 与mesh/raster/encoder同时存在。另预留生成BLP的task copy及真实解码/散列。
        std::size_t outside = Sum({taskCeiling, plan->OwnedCapacityBytes(), SnapshotCapacity(assets), kMetadataReserve,
            kHashReserve, kEncodedReserve, 2 * kDecodedBytes});
        auto candidateLimits = limits.candidate;
        candidateLimits.maxWorkingBytes = std::min(candidateLimits.maxWorkingBytes, Remaining(limits.maxWorkingBytes, outside));
        auto candidate = Take(Render::BuildTerrainMinimapCandidate(assets.Input(), candidateLimits));
        const auto& manifest = candidate.manifest;
        if (manifest.rangePolicy != rangePolicy
            || manifest.encodedMipCount != 9 || manifest.encodedTotalMipPixels != 87381
            || manifest.encodedBlpBytes != candidate.encoded.bytes.size()
            || manifest.outputBlpSha256 != Take(Sha256(candidate.encoded.bytes))
            || manifest.formalSaveAuthorized || manifest.nativeEditorOrGameVerified)
            Fail("MINIMAP_SAVE_ENCODING", "生成清单必须绑定实际完整九级BLP与未取得native验证的事实。");
        Charge(outside, candidate.image.rgba.capacity()); Charge(outside, candidate.encoded.bytes.capacity());
        Charge(outside, ManifestCapacity(candidate.manifest)); Within(limits.maxWorkingBytes, outside);
        auto decoded = DecodeActual(candidate.encoded.bytes);
        Check(plan->AdmitMissingMinimap(edits, std::move(candidate.encoded.bytes)));
        auto prepared = std::shared_ptr<TerrainMinimapSave>(new TerrainMinimapSave);
        prepared->assets_ = std::make_unique<const Render::TerrainMinimapAssetSnapshot>(std::move(assets));
        prepared->plan_ = std::move(plan); prepared->manifest_ = std::move(candidate.manifest);
        prepared->decodedMip0_ = std::move(decoded); prepared->limits_ = limits;
        // 保留实际收紧后的读取限额，后续复核仍从同一政策继续收紧。
        prepared->limits_.assets = captureLimits;
        prepared->sourcePath_ = Path(task.sourcePath, "MINIMAP_SAVE_SOURCE");
        prepared->targetPath_ = Path(task.target, "MINIMAP_SAVE_TARGET");
        prepared->taskReplacementCapacityCeiling_ = taskCeiling;
        prepared->taskIdentity_ = &task; prepared->archiveIdentity_ = task.archive.get();
        std::size_t retained = prepared->RetainedCapacityBytes(); Charge(retained, taskCeiling);
        Charge(retained, kHashReserve + kMetadataReserve + 2 * kDecodedBytes);
        Within(limits.maxWorkingBytes, retained);
        // 生成期间仍须核会话修订；当前资源已由Capture同次租约/guard核验。
        if (edits.SessionIdentity() != prepared->plan_->SessionIdentity() || edits.Revision() != prepared->plan_->Revision()
            || edits.TerrainSourceRevision() != prepared->plan_->TerrainSourceRevision())
            Fail("TERRAIN_SAVE_PLAN_STALE", "小地图生成期间编辑会话已变化。");
        return Output::Success(std::move(prepared));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Output::Failure(kSubsystem, "MINIMAP_SAVE_ALLOCATION", "小地图准备分配失败，不返回部分资格。"); }
    catch (const std::length_error&) { return Output::Failure(kSubsystem, "MINIMAP_SAVE_ALLOCATION", "小地图准备容量异常，不返回部分资格。"); }
}

Status TerrainMinimapSave::VerifyTask(const MapSaveTask& task, bool beforeAdmission) const {
    return GuardStatus([&] {
        SourceTask(task);
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || task.sourceSha256 != plan_->SourceSha256()
            || Path(task.sourcePath, "MINIMAP_SAVE_SOURCE") != sourcePath_)
            Fail("MINIMAP_SAVE_SOURCE", "小地图资格不是此真实冻结任务/owned来源。");
        if (Path(task.target, "MINIMAP_SAVE_TARGET") != targetPath_)
            Fail("MINIMAP_SAVE_TARGET", "小地图资格的SaveCopy目标已变化。");
        Within(taskReplacementCapacityCeiling_, TaskReplacementCapacity(task));
        std::size_t used = RetainedCapacityBytes(); Charge(used, taskReplacementCapacityCeiling_);
        Charge(used, kMetadataReserve + kHashReserve + 2 * kDecodedBytes);
        FinalMembers(task, beforeAdmission, Remaining(limits_.maxWorkingBytes, used));
        Check(plan_->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals));
    });
}

Status TerrainMinimapSave::VerifyCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits,
    const std::filesystem::path& gameRoot) const {
    return GuardStatus([&] {
        Check(VerifyTask(task, false));
        Check(plan_->VerifyForSave(edits, task.sourceSha256, task.replacements, task.removals));
        if (gameRoot.empty() != assets_->Binding().gameRoot.empty()
            || (!gameRoot.empty() && Path(gameRoot, "MINIMAP_SAVE_SOURCE") != Path(assets_->Binding().gameRoot, "MINIMAP_SAVE_SOURCE")))
            Fail("MINIMAP_SAVE_SOURCE", "小地图资格的gameRoot已变化。");
        std::size_t used = RetainedCapacityBytes(); Charge(used, taskReplacementCapacityCeiling_);
        auto limits = CaptureLimits(limits_.assets, Remaining(limits_.maxWorkingBytes, used), manifest_.rangePolicy);
        Check(Render::VerifyTerrainMinimapAssetsCurrent(*assets_, *task.archive, edits, gameRoot, limits));
    });
}

Status TerrainMinimapSave::VerifyFrozenOutput(const MapSaveTask& task) const {
    return GuardStatus([&] {
        Check(VerifyTask(task, false));
        std::size_t used = RetainedCapacityBytes(); Charge(used, taskReplacementCapacityCeiling_);
        auto limits = CaptureLimits(limits_.assets, Remaining(limits_.maxWorkingBytes, used), manifest_.rangePolicy);
        Check(Render::VerifyTerrainMinimapAssetsFrozenOutput(*assets_, *task.archive, task.replacements, task.removals, limits));
    });
}

Status TerrainMinimapSave::VerifyReadback(const MapSaveTask& task, const MapArchiveSession& saved) const {
    return GuardStatus([&] {
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || task.sourceSha256 != plan_->SourceSha256()
            || task.sourceSha256 != task.archive->SourceSha256()
            || Path(task.sourcePath, "MINIMAP_SAVE_SOURCE") != sourcePath_
            || Path(task.archive->SourcePath(), "MINIMAP_SAVE_SOURCE") != sourcePath_)
            Fail("MINIMAP_SAVE_SOURCE", "回读必须提供同一真实冻结任务及其owned归档。");
        if (task.inPlace || Path(task.target, "MINIMAP_SAVE_TARGET") != targetPath_)
            Fail("MINIMAP_SAVE_TARGET", "回读task目标已脱离原SaveCopy绑定。");
        if (Path(saved.SourcePath(), "MINIMAP_SAVE_TARGET") != targetPath_)
            Fail("MINIMAP_SAVE_TARGET", "回读归档不是资格绑定的独立SaveCopy目标。");
        Within(taskReplacementCapacityCeiling_, TaskReplacementCapacity(task));
        std::size_t used = RetainedCapacityBytes();
        Charge(used, TaskReplacementCapacity(task)); // 实际最终脚本/native/W3E/MapBLP的全部正文/name/descriptor。
        Charge(used, kMetadataReserve + kHashReserve + 2 * kDecodedBytes);
        const auto available = Remaining(limits_.maxWorkingBytes, used);
        if (task.triggerChanges || !task.objectTransactions.empty() || !task.removals.empty() || task.replacements.size() > 12)
            Fail("MINIMAP_SAVE_DOMAIN", "回读任务出现未获准的混合领域或删除。");
        FinalMembers(task, false, available);
        Check(plan_->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals));
        const MapData::TerrainSavePlanLimits readLimits{std::min(limits_.plan.maximumBytes, available / 3)};
        Check(plan_->VerifyReadback(saved, readLimits));
        auto limits = CaptureLimits(limits_.assets, available, manifest_.rangePolicy);
        Check(Render::VerifyTerrainMinimapAssetsReadback(*assets_, saved, limits));
        const std::array<std::string, 1> names{"war3mapMap.blp"};
        ArchiveExactMemberLimits capture;
        capture.memberCount = 1; capture.memberBytes = kEncodedReserve;
        capture.totalBytes = std::min(kEncodedReserve, available / 3);
        const auto members = Take(saved.CaptureExactMembers(names, capture));
        if (!members[0].present || members[0].bytes != MinimapReplacement().data)
            Fail("MINIMAP_SAVE_ENCODING", "回读MapBLP必须逐字等于本次实际生成的完整九级编码。");
        const auto decoded = DecodeActual(members[0].bytes);
        if (decoded.width != decodedMip0_.width || decoded.height != decodedMip0_.height
            || decoded.rgbaBytes != decodedMip0_.rgbaBytes || decoded.decodedRgbaSha256 != decodedMip0_.decodedRgbaSha256)
            Fail("MINIMAP_SAVE_ENCODING", "回读实际encoded解码mip0与准备阶段的真实解码基线不符。");
    });
}

} // namespace War3Map::Services

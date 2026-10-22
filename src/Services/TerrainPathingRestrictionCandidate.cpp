#include "War3MapCore/Services/TerrainPathingRestrictionCandidate.h"

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace War3Map::Services {
namespace {

constexpr const char* kSubsystem = "TerrainPathingCandidate";
constexpr std::size_t kMetadataAndHashReserve = 3u * 1024u * 1024u;
constexpr std::array<std::string_view, 8> kNames{
    "war3map.w3e", "war3map.wpm", "war3map.doo", "war3mapUnits.doo", "war3map.w3i",
    "war3map.shd", "war3map.mmp", "war3mapMap.blp"};

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{Status::Failure(kSubsystem, code, message).diagnostics};
}
void Check(const Status& status) { if (!status.Succeeded()) throw Failure{status.diagnostics}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class F> Status Guard(F&& function) {
    try { function(); return Status::Ok(); }
    catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "PATHING_CANDIDATE_ALLOCATION", "离线通行候选分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "PATHING_CANDIDATE_ALLOCATION", "离线通行候选容量异常。"); }
}
void Charge(std::size_t& total, std::size_t count, std::size_t element = 1) {
    if (element && count > (std::numeric_limits<std::size_t>::max() - total) / element)
        Fail("PATHING_CANDIDATE_BUDGET", "通行候选工作字节预算乘加溢出。");
    total += count * element;
}
void Within(std::size_t maximum, std::size_t used) {
    if (used > maximum) Fail("PATHING_CANDIDATE_BUDGET", "通行候选工作字节超出保守预算。");
}
std::size_t TaskBytes(const MapSaveTask& task) {
    std::size_t bytes = 0; Charge(bytes, task.replacements.capacity(), sizeof(MemberReplacement));
    for (const auto& member : task.replacements) { Charge(bytes, member.name.capacity()); Charge(bytes, 1); Charge(bytes, member.data.capacity()); }
    return bytes;
}
std::filesystem::path Path(const std::filesystem::path& input) {
    if (input.empty() || !input.is_absolute() || input.native().size() > 4096)
        Fail("PATHING_CANDIDATE_TARGET", "候选来源和独立目标需要有界绝对路径。");
    std::error_code error; auto path = std::filesystem::weakly_canonical(input, error);
    if (error) Fail("PATHING_CANDIDATE_TARGET", "候选路径不能准确规范化。");
    auto value = path.native(); std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return std::filesystem::path(value);
}
bool SameName(std::string_view a, std::string_view b) {
    const auto lower = [](unsigned char c) { return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}
void Domain(const MapSaveTask& task) {
    if (!task.testCopyOnly || task.inPlace || task.executed || task.triggerChanges || task.script
        || !task.objectTransactions.empty() || !task.removals.empty() || task.beforePublish
        || task.mapObjectReferences || task.generatedLuaTestUnit || task.luaTestInstalledBindings)
        Fail("PATHING_CANDIDATE_DOMAIN", "离线通行候选只准独立测试副本，不运行脚本、混合事务或外部发布回调。");
    for (std::size_t i = 0; i < kMapSaveObjectTypeCount; ++i)
        if (task.nextBaselines[i] || task.generatedObjectBaselines[i])
            Fail("PATHING_CANDIDATE_DOMAIN", "离线通行候选不能夹带物编基线。");
}
void SourcePrefix(const MapArchiveSession& source) {
    if (source.Prefix().size() > 16u * 1024u * 1024u)
        Fail("PATHING_CANDIDATE_BUDGET", "原Prefix超过既有归档来源硬限。");
    struct Lease {
        HANDLE handle;
        ~Lease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } lease{CreateFileW(source.SourcePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (lease.handle == INVALID_HANDLE_VALUE) Fail("SOURCE_LEASE_FAILED", "原Prefix来源不可读或正被写入。");
    // 持有不共享write/delete的真实源句柄后核SHA/文件身份，路径不能在复核和读取之间替换。
    Check(source.VerifySourceUnchanged());
    Mpq::MpqOpenLimits limits;
    limits.maxArchivePrefixBytes = source.Prefix().size(); // 分配前限长；不读取整幅地图。
    limits.maxDeclaredArchiveBytes = 1024ull * 1024ull * 1024ull;
    limits.maxStrictMemberCount = 131072;
    auto opened = Take(Mpq::MpqArchive::OpenReadOnlyHandle(lease.handle, source.SourcePath(), limits));
    const auto prefix = Take(opened.ReadArchivePrefix());
    if (!std::equal(prefix.begin(), prefix.end(), source.Prefix().begin(), source.Prefix().end()))
        Fail("PATHING_CANDIDATE_SOURCE", "owned快照Prefix已改写，不是实际源原字。");
    Check(source.VerifySourceUnchanged());
}
void Source(const MapSaveTask& task, bool published) {
    Domain(task);
    if (!task.archive || task.sourceSha256 != task.archive->SourceSha256()
        || Path(task.sourcePath) != Path(task.archive->SourcePath()))
        Fail("PATHING_CANDIDATE_SOURCE", "需要FreezeArchive真实owned来源，不接受外部摘要或proof授予资格。");
    if (!task.archive->MetadataMembers().empty())
        Fail("PATHING_CANDIDATE_DOMAIN", "来源含MPQ元数据，既有普通writer会清理；本次零删除候选拒绝该来源。");
    if (Path(task.target) == Path(task.sourcePath)) Fail("PATHING_CANDIDATE_TARGET", "离线候选禁止覆盖来源。");
    std::error_code error; const bool exists = std::filesystem::exists(task.target, error);
    if (error || exists != published) Fail("PATHING_CANDIDATE_TARGET", "候选目标存在性与当前准备/回读阶段不符。");
    SourcePrefix(*task.archive);
}
std::array<std::string, 8> Names() {
    std::array<std::string, 8> names; std::copy(kNames.begin(), kNames.end(), names.begin()); return names;
}
ArchiveExactMemberLimits CaptureLimits(std::size_t bytes, std::size_t count = 8) {
    ArchiveExactMemberLimits limits; limits.memberCount = count; limits.memberBytes = limits.totalBytes = bytes;
    return limits;
}
std::size_t SourceBytes(const ArchiveExactIdentityResult& source) {
    std::size_t total = 0;
    for (const auto& member : source.members) if (member.present) {
        if (!member.byteSize || !member.sha256 || *member.byteSize > std::numeric_limits<std::size_t>::max())
            Fail("PATHING_CANDIDATE_SOURCE", "精确成员缺少可用长度或摘要。");
        Charge(total, static_cast<std::size_t>(*member.byteSize));
    }
    return total;
}
std::size_t Reservation(std::size_t sourceBytes, std::size_t w3eBytes, std::size_t taskBytes, std::size_t prefixBytes) {
    // MapTerrain先核Remaining()/recordBytes再reserve，v11每顶点最少7B（v12的8B更小）。
    // 表数量先受256项硬帽；短/巨大计数头在分配顶点前拒绝。没有自行解析W3E字段。
    std::size_t decoded = kMetadataAndHashReserve;
    Charge(decoded, w3eBytes, 2); Charge(decoded, w3eBytes / 7, sizeof(MapData::TerrainVertex) + sizeof(MapData::TerrainTile));
    // 精确捕获返回的8成员正文最多3×capacity；上游reader表/解码scratch单独受既有硬限。
    // inspection5成员原/现与WPM替换共存，proof的
    // 原/现WPM、共享A、APR编码/分层解码/剥离副本及task append预留合计12×source；两倍terrain
    // 顶点/tiles/原字临时上界覆盖唯一parser与proof阶段，不只计最终retained。
    std::size_t bytes = kMetadataAndHashReserve; Charge(bytes, sourceBytes, 12); Charge(bytes, decoded, 2); Charge(bytes, taskBytes); Charge(bytes, prefixBytes, 2);
    return bytes;
}
void CheckInspection(const MapData::TerrainEditInspection& inspection) {
    for (std::size_t i = 0; i < inspection.Members().size(); ++i) if (i != 1) {
        const auto& member = inspection.Members()[i];
        if (member.originalSha256 != member.currentSha256 || static_cast<bool>(member.original) != static_cast<bool>(member.current))
            Fail("PATHING_CANDIDATE_DOMAIN", "候选只允许WPM净差；W3E、DOO、Units和W3I必须原字不变。");
    }
    const auto& proof = inspection.PathingPreservation();
    if (!proof.Succeeded()) throw Failure{proof.diagnostics};
    if (proof.value->Kind() != MapData::PathingPreservationKind::ExplicitRestrictionCandidate || !proof.value->ChangedCells())
        Fail("PATHING_CANDIDATE_EMPTY", "候选要求APR解释的非空附加限制，PreserveExact不能冒充候选。");
}

} // namespace

std::size_t TerrainPathingRestrictionCandidate::RetainedCapacityBytes() const {
    // proof有界64格及固定SHA、8身份、两个4096字符路径和元数据由固定预留覆盖。
    std::size_t bytes = kMetadataAndHashReserve; Charge(bytes, replacement_.data.capacity()); return bytes;
}

Result<std::shared_ptr<const TerrainPathingRestrictionCandidate>> TerrainPathingRestrictionCandidate::Prepare(
    const MapSaveTask& task, const MapData::MapDataEditSession& edits, const TerrainPathingCandidateLimits& limits) {
    using Output = Result<std::shared_ptr<const TerrainPathingRestrictionCandidate>>;
    try {
        const TerrainPathingCandidateLimits hard;
        if (limits.maxWorkingBytes > hard.maxWorkingBytes || limits.maximumCaptureBytes > hard.maximumCaptureBytes
            || limits.maximumKnownMembers > hard.maximumKnownMembers || !limits.maximumKnownMembers
            || limits.maxWorkingBytes <= 3 * kMetadataAndHashReserve)
            Fail("PATHING_CANDIDATE_BUDGET", "候选只能收紧工作字节、捕获与成员数硬预算。");
        std::size_t initial = kMetadataAndHashReserve; Charge(initial, TaskBytes(task));
        if (task.archive) Charge(initial, task.archive->Prefix().size(), 2);
        Within(limits.maxWorkingBytes, initial); Source(task, false);
        if (!task.replacements.empty()) Fail("PATHING_CANDIDATE_DOMAIN", "准备时task成员列表必须为空。");
        const auto captureBytes = std::min(limits.maximumCaptureBytes,
            (limits.maxWorkingBytes - initial) / 12);
        if (task.archive->Members().size() > limits.maximumKnownMembers || !captureBytes)
            Fail("PATHING_CANDIDATE_BUDGET", "候选成员或捕获工作超过预算。");
        for (const auto& member : task.archive->Members())
            if (member.name.size() > 1024 || member.data.size() > captureBytes)
                Fail("PATHING_CANDIDATE_BUDGET", "已命名成员超出逐项回读工作预算。");
        auto identities = Take(task.archive->DescribeExactMemberIdentities(task.sourceSha256, Names(), CaptureLimits(captureBytes)));
        if (identities.members.size() != 8 || !identities.members[0].present || !identities.members[1].present)
            Fail("PATHING_CANDIDATE_SOURCE", "候选需要准确存在的源W3E/WPM。");
        const auto reserved = Reservation(SourceBytes(identities), static_cast<std::size_t>(*identities.members[0].byteSize), TaskBytes(task), task.archive->Prefix().size());
        Within(limits.maxWorkingBytes, reserved);
        // 在inspection与密封副本分配前准入后续重捕获阶段；源WPM长度即唯一允许的输出
        // 长度，副本各预留2×正文capacity及有界name/对象。分配后还会核实际capacity。
        std::size_t admitted = reserved; Charge(admitted, kMetadataAndHashReserve); Charge(admitted, TaskBytes(task));
        Charge(admitted, static_cast<std::size_t>(*identities.members[1].byteSize), 4);
        Charge(admitted, 1, sizeof(MemberReplacement)); Charge(admitted, 1024);
        Within(limits.maxWorkingBytes, admitted);
        auto inspection = Take(MapData::CaptureTerrainEditInspection(*task.archive, edits, {captureBytes}));
        CheckInspection(inspection);
        auto candidate = std::shared_ptr<TerrainPathingRestrictionCandidate>(new TerrainPathingRestrictionCandidate);
        candidate->proof_ = *inspection.PathingPreservation().value;
        candidate->replacement_ = {"war3map.wpm", *inspection.Members()[1].current};
        candidate->inspectionSha256_ = inspection.IdentitySha256();
        std::move(identities.members.begin(), identities.members.end(), candidate->members_.begin());
        candidate->sourcePath_ = Path(task.sourcePath); candidate->targetPath_ = Path(task.target);
        candidate->taskIdentity_ = &task; candidate->archiveIdentity_ = task.archive.get();
        candidate->limits_ = limits; candidate->captureBytes_ = captureBytes; candidate->inspectionReservationBytes_ = reserved;
        // 准备阶段就保守容纳后续VerifyCurrent：inspection/proof暂态与retained和唯一
        // WPM task副本共存。已预留的空vector容量仍保留；真实append后再核其实际capacity。
        std::size_t ready = reserved; Charge(ready, candidate->RetainedCapacityBytes()); Charge(ready, TaskBytes(task));
        if (!task.replacements.capacity()) Charge(ready, 1, sizeof(MemberReplacement));
        Charge(ready, candidate->replacement_.name.capacity()); Charge(ready, 1); Charge(ready, candidate->replacement_.data.capacity());
        Within(admitted, ready); candidate->workingReservationBytes_ = admitted;
        std::size_t actual = candidate->RetainedCapacityBytes(); Charge(actual, TaskBytes(task));
        // 已拥有inspection原/现唯一vector容量也核实；相同shared owner只计一次。
        for (std::size_t i = 0; i < inspection.Members().size(); ++i) {
            const auto& member = inspection.Members()[i];
            if (member.original) Charge(actual, member.original->capacity());
            if (member.current && member.current != member.original) Charge(actual, member.current->capacity());
        }
        if (inspection.PathingRestrictionIntent()) Charge(actual, inspection.PathingRestrictionIntent()->capacity());
        Within(limits.maxWorkingBytes, actual);
        if (edits.SessionIdentity() != candidate->proof_->Binding().sessionIdentity || edits.Revision() != candidate->proof_->Binding().revision
            || edits.TerrainSourceRevision() != candidate->proof_->Binding().terrainSourceRevision)
            Fail("PATHING_CANDIDATE_STALE", "准备期间会话修订或来源代际变化。");
        Source(task, false);
        auto result = Output::Success(std::move(candidate));
        result.diagnostics.push_back({DiagnosticSeverity::Info, kSubsystem, "PATHING_CANDIDATE_OFFLINE_ONLY",
            "本结果仅为离线WPM限制候选；正式保存仍须通过共享保存计划，未取得原版或游戏验证。工作字节预算不含归档reader内部表或进程RSS。", std::nullopt});
        return result;
    } catch (Failure& failure) { return {std::nullopt, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Output::Failure(kSubsystem, "PATHING_CANDIDATE_ALLOCATION", "准备分配失败，不返回部分候选。"); }
    catch (const std::length_error&) { return Output::Failure(kSubsystem, "PATHING_CANDIDATE_ALLOCATION", "准备容量异常，不返回部分候选。"); }
}

Status TerrainPathingRestrictionCandidate::VerifyTask(const MapSaveTask& task, bool published) const {
    return Guard([&] {
        std::size_t bytes = RetainedCapacityBytes(); Charge(bytes, TaskBytes(task)); Charge(bytes, captureBytes_, 3);
        if (task.archive) Charge(bytes, task.archive->Prefix().size(), 2);
        Within(limits_.maxWorkingBytes, bytes);
        Source(task, published);
        if (&task != taskIdentity_ || task.archive.get() != archiveIdentity_ || task.sourceSha256 != proof_->Binding().archiveSha256
            || Path(task.sourcePath) != sourcePath_ || Path(task.target) != targetPath_)
            Fail("PATHING_CANDIDATE_SOURCE", "候选不属于同一实际task、owned来源或独立目标。");
        if (task.replacements.size() != 1 || !SameName(task.replacements[0].name, replacement_.name)
            || task.replacements[0].data != replacement_.data)
            Fail("PATHING_CANDIDATE_OUTPUT", "最终列表只能含本次密封WPM原字输出，不能夹带或重复成员。");
    });
}

Status TerrainPathingRestrictionCandidate::VerifyCurrent(const MapSaveTask& task, const MapData::MapDataEditSession& edits) const {
    return Guard([&] {
        Check(VerifyFrozenOutput(task));
        const auto& binding = proof_->Binding();
        if (edits.SessionIdentity() != binding.sessionIdentity || edits.Revision() != binding.revision
            || edits.TerrainSourceRevision() != binding.terrainSourceRevision)
            Fail("PATHING_CANDIDATE_STALE", "候选的会话、修订或来源代际已过期，即使当前字节相同也不复活。");
        std::size_t phase = inspectionReservationBytes_; Charge(phase, RetainedCapacityBytes()); Charge(phase, TaskBytes(task));
        Within(limits_.maxWorkingBytes, phase); // 原inspection/proof临时上界与retained/task副本同时存活。
        auto inspection = Take(MapData::CaptureTerrainEditInspection(*task.archive, edits, {captureBytes_}));
        CheckInspection(inspection);
        if (inspection.IdentitySha256() != inspectionSha256_ || *inspection.Members()[1].current != replacement_.data)
            Fail("PATHING_CANDIDATE_STALE", "当前inspection/APR与准备时不符。");
    });
}

Status TerrainPathingRestrictionCandidate::VerifyFrozenOutput(const MapSaveTask& task) const { return VerifyTask(task, false); }

Status TerrainPathingRestrictionCandidate::VerifyReadback(const MapSaveTask& task, const MapArchiveSession& saved) const {
    return Guard([&] {
        Check(VerifyTask(task, true));
        if (Path(saved.SourcePath()) != targetPath_) Fail("PATHING_CANDIDATE_TARGET", "回读不是密封候选的实际独立目标。");
        Check(saved.VerifySourceUnchanged());
        if (!std::equal(saved.Prefix().begin(), saved.Prefix().end(), task.archive->Prefix().begin(), task.archive->Prefix().end()))
            Fail("PATHING_CANDIDATE_READBACK", "候选Prefix与来源不符。");
        SourcePrefix(saved); // saved也可能经WithLeadingHeader改快照，必须核实际目标原字。
        const auto actual = Take(saved.DescribeExactMemberIdentities(saved.SourceSha256(), Names(), CaptureLimits(captureBytes_)));
        for (std::size_t i = 0; i < members_.size(); ++i) {
            const auto& expected = members_[i]; const auto& got = actual.members[i];
            const auto sha = i == 1 ? std::optional<std::string>(proof_->OutputPathingSha256()) : expected.sha256;
            const auto size = i == 1 ? std::optional<std::uint64_t>(replacement_.data.size()) : expected.byteSize;
            if (got.present != expected.present || got.sha256 != sha || got.byteSize != size)
                Fail("PATHING_CANDIDATE_READBACK", "回读WPM或保留派生成员的字节/存在性不符。");
        }
        const auto originals = task.archive->Members();
        const bool namedPathing = std::any_of(originals.begin(), originals.end(), [](const auto& member) { return SameName(member.name, "war3map.wpm"); });
        if (saved.Members().size() != originals.size() + (namedPathing ? 0 : 1))
            Fail("PATHING_CANDIDATE_READBACK", "候选已命名成员清单含额外新增或删除。");
        for (const auto& member : originals) {
            if (SameName(member.name, "war3map.wpm")) continue;
            const std::array<std::string, 1> name{member.name};
            const auto read = Take(saved.CaptureExactMembers(name, CaptureLimits(captureBytes_, 1)));
            if (!read[0].present || read[0].bytes != member.data)
                Fail("PATHING_CANDIDATE_READBACK", "候选改动了WPM以外的已命名原成员。");
        }
        Check(saved.VerifySourceUnchanged());
    });
}

} // namespace War3Map::Services

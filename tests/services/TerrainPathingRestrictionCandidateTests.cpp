#include "War3MapCore/Services/TerrainPathingRestrictionCandidate.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "../../src/Core/Mpq/MpqCrypt.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string text = "fixture失败"; for (const auto& d : result.diagnostics) text += "|" + d.code + ":" + d.message;
        throw std::runtime_error(text);
    }
    return std::move(*result.value);
}
bool Code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
template<class T> void Refused(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "失败不返回部分candidate");
    if (!code.empty()) Require(Code(result.diagnostics, code), "应命中预期拒绝点");
}
void Refused(const Status& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.diagnostics.empty(), "复核必须拒绝");
    if (!code.empty()) Require(Code(result.diagnostics, code), "复核应命中预期拒绝点");
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255);
}
Bytes Text(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size())); return {bytes.begin(), bytes.end()};
}
// 与既有TerrainSavePlanTests相同的原创未压缩MPQ夹具，明确hash与listfile互相独立。
// 只构造小型测试输入；正式候选仍必须经过SaveCandidate/AfterSave链。
Bytes OpaqueArchiveBytes(const std::vector<MapArchiveMember>& files, bool unsupportedShadow) {
    const auto count = static_cast<std::uint32_t>(files.size()); std::uint32_t hashCount = 8, dataBytes = 0;
    while (hashCount < count * 2) hashCount *= 2;
    for (const auto& file : files) dataBytes += static_cast<std::uint32_t>(file.data.size());
    const auto hashes = 32u + dataBytes, blocks = hashes + hashCount * 16;
    Bytes bytes(blocks + count * 16); Put32(bytes, 0, 0x1a51504d); Put32(bytes, 4, 32); Put32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    Put32(bytes, 12, 0x00030000); Put32(bytes, 16, hashes); Put32(bytes, 20, blocks); Put32(bytes, 24, hashCount); Put32(bytes, 28, count);
    std::fill(bytes.begin() + hashes, bytes.begin() + blocks, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance(); std::uint32_t position = 32;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& file = files[index]; std::copy(file.data.begin(), file.data.end(), bytes.begin() + position);
        auto bucket = crypt.HashString(file.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        for (;; bucket = (bucket + 1) & (hashCount - 1)) {
            const auto at = hashes + bucket * 16;
            if (bytes[at + 12] != std::byte{0xff}) continue;
            Put32(bytes, at, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameA));
            Put32(bytes, at + 4, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameB));
            Put32(bytes, at + 8, 0); Put32(bytes, at + 12, index); break;
        }
        const auto size = static_cast<std::uint32_t>(file.data.size());
        Put32(bytes, blocks + index * 16, position); Put32(bytes, blocks + index * 16 + 4, size); Put32(bytes, blocks + index * 16 + 8, size);
        Put32(bytes, blocks + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit
            | (unsupportedShadow && file.name == "war3map.shd" ? Mpq::kFilePatch : 0u)); position += size;
    }
    crypt.EncryptBlock(bytes.data() + hashes, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blocks, count * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey)); return bytes;
}
class Directory {
public:
    Directory() {
        static std::atomic<unsigned> next{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-pathing-candidate-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(path_), "创建独占合成目录");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const auto& Path() const { return path_; }
private:
    std::filesystem::path path_;
};
struct Fixture {
    Directory directory;
    Bytes w3e, wpm, prefix;
    std::vector<MapArchiveMember> members;
    MapArchiveSession source;
    std::unique_ptr<MapDataEditSession> edits;
    explicit Fixture(int derived = 1, std::optional<Bytes> terrainOverride = {}, bool unlisted = false,
        bool unsupportedShadow = false, bool metadata = false) {
        TerrainGridSnapshot grid; grid.header.width = grid.header.height = 5; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Ldrt"}; grid.header.cliffTilesets = {"CLdi"}; grid.vertices.resize(25);
        for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(grid, 0, 0, 5, 5); const auto baseline = Take(SerializeMapTerrain(grid));
        w3e = terrainOverride ? *terrainOverride : baseline;
        wpm = Text("MP3W"); wpm.resize(16 + 16 * 16, std::byte{0x91}); Put32(wpm, 4, 0); Put32(wpm, 8, 16); Put32(wpm, 12, 16);
        wpm[16] = std::byte{0xb1}; wpm[18] = std::byte{0x9f}; // 原bit5、未知位与已有全部限制均须保留。
        members = {{"war3map.w3e", w3e}, {"war3map.wpm", wpm}, {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
            {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}, {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
            {"war3map.j", Text("function main takes nothing returns nothing\nendfunction\n")}, {"unrelated.bin", Text("unknown author asset bytes")}};
        if (derived) {
            Bytes shadow; if (derived == 1) for (unsigned i = 0; i < 256; ++i) shadow.push_back(static_cast<std::byte>(i));
            Bytes markers(24); Put32(markers, 4, 1); Put32(markers, 8, 0x76543210); Put32(markers, 12, 0x80000000);
            Put32(markers, 16, 0x7fffffff); Put32(markers, 20, 0x9a123456);
            members.push_back({"war3map.shd", shadow}); members.push_back({"war3map.mmp", markers}); members.push_back({"war3mapMap.blp", {}});
        }
        prefix = Text("HM3W"); prefix.resize(512, std::byte{0x55});
        if (unlisted) {
            prefix.clear(); auto files = members;
            if (metadata) files.push_back({"(attributes)", Text("opaque attributes bytes")});
            files.push_back({"(listfile)", Text("war3map.w3e\nwar3map.wpm\nwar3map.w3i\nwar3map.doo\nwar3mapUnits.doo\nwar3map.j\nunrelated.bin\n")});
            Require(IO::WriteFileBytes(directory.Path() / "source.w3x", OpaqueArchiveBytes(files, unsupportedShadow)).Succeeded(), "未列名派生成员原始夹具");
        } else Take(WriteVerifiedArchive(directory.Path() / "source.w3x", members, prefix));
        source = Take(MapArchiveSession::Open(directory.Path() / "source.w3x"));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromArchive(source));
        // malformed归档仍附上合法上下文，使服务真正进入唯一source W3E parser，
        // 不能让阴性仅在PATHING_POLICY_CONTEXT_MISSING提前通过。
        Require(edits->AttachTerrain(baseline).Succeeded() && edits->AttachPathing(wpm).Succeeded(), "附入原生源对");
    }
    void Stroke() {
        TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction; brush.size = 1; brush.pathingRestrictions = 0x0e;
        const TerrainStrokePoint point{48, 48}; Require(edits->EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "真实APR笔划");
    }
    void Freeze(MapSaveTask& task) {
        Require(task.FreezeArchive(source).Succeeded(), "真实FreezeArchive"); task.testCopyOnly = true; task.target = directory.Path() / "candidate.w3x";
    }
    auto Prepare(MapSaveTask& task) { return Take(TerrainPathingRestrictionCandidate::Prepare(task, *edits)); }
};

void OriginalMembersSurviveOnlyWpmChanges() {
    for (const int derived : {0, 1, 2}) {
        Fixture f(derived); f.Stroke();
        if (derived == 2) Require(f.edits->SetSerialized(MapDataKind::Strings, Text("STRING 1\n{\npending independent text\n}\n")).Succeeded(), "其它待编辑不属于WPM输出域");
        MapSaveTask task; f.Freeze(task); const auto revision = f.edits->Revision();
        const auto prepared = f.Prepare(task);
        Require(!prepared->FormalSaveAuthorized() && !prepared->NativeValidated() && prepared->Proof().RequiresNativeVerification(), "候选不冒充正式/native资格");
        Require(prepared->Proof().Kind() == PathingPreservationKind::ExplicitRestrictionCandidate && prepared->Proof().ChangedCells() == 1, "消费真实proof");
        for (std::size_t i = 5; i < 8; ++i) Require(prepared->SourceMembers()[i].present == (derived != 0), "empty与Missing存在性不同");
        if (derived == 2) Require(prepared->SourceMembers()[5].byteSize == 0, "真实空SHD保有长度身份");
        Require(f.edits->VerifyTerrainPathingForSave().Succeeded(), "完整声明也可进入普通保存");
        auto ordinary = Take(TerrainSavePlan::Prepare(f.source, *f.edits));
        Require(ordinary.SaveGate().Succeeded(), "普通保存计划独立验证声明，不借离线候选授权");
        task.replacements.push_back(prepared->Replacement()); Require(prepared->VerifyCurrent(task, *f.edits).Succeeded(), "来源和当前完整inspection复核");
        const auto written = Take(task.archive->SaveCandidate(task.target, task.replacements));
        const auto saved = Take(task.archive->AfterSave(written, task.replacements));
        Require(prepared->VerifyReadback(task, saved).Succeeded(), "复用真实归档写出和严格回读");
        Require(std::equal(saved.Prefix().begin(), saved.Prefix().end(), f.prefix.begin(), f.prefix.end()), "原Prefix准确保留");
        const auto got = Take(saved.ReadMember("war3map.wpm")); Require(got[16 + 17] == std::byte{0x9f}, "只追加指定格限制组合");
        for (std::size_t i = 0; i < got.size(); ++i) if (i != 16 + 17) Require(got[i] == f.wpm[i], "其余WPM头/位原字保持");
        for (const auto& member : f.members) if (member.name != "war3map.wpm")
            Require(Take(saved.ReadMember(member.name)) == member.data, "SHD非二值/未知MMP/空MapBLP/其它asset原字保持");
        Require(Take(Sha256File(f.source.SourcePath())) == f.source.SourceSha256() && f.edits->Revision() == revision && f.edits->CanUndo(), "源图与Undo历史不动");
        if (derived == 2) Require(!saved.HasMember("war3map.wts") && f.edits->Replacements().size() == 2, "不保存或rebase整会话其它未保存编辑");
    }
}

void TaskAndRevisionCannotBorrowQualification() {
    Fixture f; f.Stroke(); MapSaveTask task; f.Freeze(task); const auto prepared = f.Prepare(task);
    task.replacements.push_back(prepared->Replacement()); Require(prepared->VerifyFrozenOutput(task).Succeeded(), "正确冻结输出");
    MapSaveTask foreign; f.Freeze(foreign); foreign.target = task.target; foreign.replacements = task.replacements;
    Refused(prepared->VerifyFrozenOutput(foreign), "PATHING_CANDIDATE_SOURCE");
    const auto target = task.target; task.target = f.directory.Path() / "other.w3x"; Refused(prepared->VerifyFrozenOutput(task), "PATHING_CANDIDATE_SOURCE"); task.target = target;
    task.replacements[0].data.back() ^= std::byte{4}; Refused(prepared->VerifyFrozenOutput(task), "PATHING_CANDIDATE_OUTPUT"); task.replacements[0] = prepared->Replacement();
    task.replacements.push_back(prepared->Replacement()); Refused(prepared->VerifyFrozenOutput(task), "PATHING_CANDIDATE_OUTPUT"); task.replacements.pop_back();
    task.replacements.push_back({"war3map.shd", {}}); Refused(prepared->VerifyFrozenOutput(task), "PATHING_CANDIDATE_OUTPUT"); task.replacements.pop_back();
    task.removals.push_back("WAR3MAP.MMP"); Refused(prepared->VerifyFrozenOutput(task), "PATHING_CANDIDATE_DOMAIN"); task.removals.clear();
    Fixture other; other.Stroke(); Refused(prepared->VerifyCurrent(task, *other.edits), "PATHING_CANDIDATE_STALE");
    Require(f.edits->Undo() && f.edits->Redo(), "同字节Undo/Redo"); Refused(prepared->VerifyCurrent(task, *f.edits), "PATHING_CANDIDATE_STALE");
    MapSaveTask fresh; f.Freeze(fresh); const auto beforeRebase = f.Prepare(fresh); fresh.replacements.push_back(beforeRebase->Replacement());
    const auto revision = f.edits->Revision(); f.edits->CommitSaved(MapDocument::FromArchive(f.source));
    Require(f.edits->Revision() == revision, "来源代际变化不依赖普通Revision");
    Refused(beforeRebase->VerifyCurrent(fresh, *f.edits), "PATHING_CANDIDATE_STALE");
    Require(!std::filesystem::exists(task.target), "复核失败从不写候选");
}

void DomainAndMalformedInputsFailWithoutPartialValue() {
    Fixture unchanged; MapSaveTask empty; unchanged.Freeze(empty);
    Refused(TerrainPathingRestrictionCandidate::Prepare(empty, *unchanged.edits), "PATHING_CANDIDATE_EMPTY");
    Fixture height; height.Stroke(); TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 1;
    const TerrainStrokePoint point{256, 256}; Require(height.edits->EditTerrain(raise, std::span(&point, 1), false).Succeeded(), "混合高度变化");
    MapSaveTask mixed; height.Freeze(mixed); Refused(TerrainPathingRestrictionCandidate::Prepare(mixed, *height.edits), "PATHING_CANDIDATE_DOMAIN");
    Fixture f; f.Stroke(); MapSaveTask task; f.Freeze(task);
    task.testCopyOnly = false; Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits), "PATHING_CANDIDATE_DOMAIN"); task.testCopyOnly = true;
    task.replacements.push_back({"unrelated.bin", Text("unauthorized")}); Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits), "PATHING_CANDIDATE_DOMAIN"); task.replacements.clear();
    task.sourceSha256 = std::string(64, 'a'); Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits), "PATHING_CANDIDATE_SOURCE"); task.sourceSha256 = f.source.SourceSha256();
    task.target = f.source.SourcePath(); Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits), "PATHING_CANDIDATE_TARGET");
    auto alteredPrefix = f.prefix; alteredPrefix.back() ^= std::byte{1};
    const auto altered = Take(f.source.WithLeadingHeader(alteredPrefix)); MapSaveTask headerTask;
    Require(headerTask.FreezeArchive(altered).Succeeded(), "同源SHA但Prefix被改的真实FreezeArchive");
    headerTask.testCopyOnly = true; headerTask.target = f.directory.Path() / "wrong-header.w3x";
    Refused(TerrainPathingRestrictionCandidate::Prepare(headerTask, *f.edits), "PATHING_CANDIDATE_SOURCE");
    for (Bytes malformed : {Bytes{}, Text("W3E!"), f.w3e}) {
        // 两个单项贴图表后width/height分别在29/33；2049合法但正文不足，
        // 应由Remaining()/7的分配前门拒绝，不能误伤表ID后在其它字段提前退出。
        if (malformed.size() > 37) { Put32(malformed, 29, 2049); Put32(malformed, 33, 2049); }
        Fixture bad(0, malformed); MapSaveTask badTask; bad.Freeze(badTask);
        Refused(TerrainPathingRestrictionCandidate::Prepare(badTask, *bad.edits), "TERRAIN_TRUNCATED");
        Require(!std::filesystem::exists(badTask.target), "空/短/巨大声明头不得分配大网格后漏出候选");
    }
}

void NativeMixedEditsRemainOutsideCandidateDomain() {
    for (const auto kind : {MapDataKind::Doodads, MapDataKind::Units, MapDataKind::Settings}) {
        Fixture f; f.Stroke(); auto document = f.edits->Snapshot();
        if (kind == MapDataKind::Doodads) {
            auto value = *document.Doodads(); DoodadPlacement doodad;
            doodad.rawcode = "LTlt"; doodad.scale = {1, 1, 1}; value.doodads.push_back(doodad);
            Require(f.edits->EditDoodads(value).Succeeded(), "混入真实DOO变化");
        } else if (kind == MapDataKind::Units) {
            auto value = *document.Units(); UnitPlacement unit;
            unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; value.units.push_back(unit);
            Require(f.edits->EditUnits(value).Succeeded(), "混入真实Units变化");
        } else {
            auto value = *document.Settings(); value.author = "mixed settings edit";
            Require(f.edits->EditSettings(value).Succeeded(), "混入真实W3I变化");
        }
        MapSaveTask task; f.Freeze(task);
        Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits), "PATHING_CANDIDATE_DOMAIN");
        Require(!std::filesystem::exists(task.target), "混合原生数据不产生部分候选");
    }
}

void WorkingBudgetsAndSourceFailuresAreClosed() {
    Fixture f; f.Stroke(); MapSaveTask task; f.Freeze(task);
    TerrainPathingCandidateLimits limits; limits.maxWorkingBytes = 1;
    Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits), "PATHING_CANDIDATE_BUDGET");
    limits = {}; limits.maximumCaptureBytes = 1; Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits));
    limits = {}; limits.maxWorkingBytes += 1; Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits), "PATHING_CANDIDATE_BUDGET");
    limits = {}; limits.maximumKnownMembers = 1; Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits), "PATHING_CANDIDATE_BUDGET");
    const auto bounded = f.Prepare(task); limits = {}; limits.maxWorkingBytes = bounded->WorkingReservationBytes() - 1;
    Refused(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits), "PATHING_CANDIDATE_BUDGET");
    limits.maxWorkingBytes += 1; const auto exact = Take(TerrainPathingRestrictionCandidate::Prepare(task, *f.edits, limits));
    task.replacements.push_back(exact->Replacement()); Require(exact->VerifyCurrent(task, *f.edits).Succeeded(), "Prepare精确准入预算足以重捕获和单WPM副本");
    task.replacements.clear();
    const auto writer = CreateFileW(f.source.SourcePath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(writer != INVALID_HANDLE_VALUE, "只持私有fixture写句柄，不写源");
    const auto locked = TerrainPathingRestrictionCandidate::Prepare(task, *f.edits); CloseHandle(writer); Refused(locked, "SOURCE_LEASE_FAILED");
    const auto prepared = f.Prepare(task); task.replacements.push_back(prepared->Replacement());
    auto changed = Take(IO::ReadFileBytes(f.source.SourcePath())); changed[32] ^= std::byte{1};
    Require(IO::WriteFileBytes(f.source.SourcePath(), changed).Succeeded(), "模拟独立源已替换");
    Refused(prepared->VerifyFrozenOutput(task)); Require(!std::filesystem::exists(task.target), "过期来源没有候选");
}

void ReadbackRejectsWrongPreservedMembersAndWpm() {
    for (const auto* name : {"war3map.wpm", "war3map.shd", "war3map.mmp", "unrelated.bin"}) {
        Fixture f; f.Stroke(); MapSaveTask task; f.Freeze(task); const auto prepared = f.Prepare(task);
        task.replacements.push_back(prepared->Replacement()); Require(prepared->VerifyCurrent(task, *f.edits).Succeeded(), "冻结正确输出");
        auto invalid = task.replacements;
        if (std::string_view(name) == "war3map.wpm") invalid[0].data.back() ^= std::byte{4};
        else invalid.push_back({name, Text("wrong preserved bytes")});
        const auto written = Take(task.archive->SaveCandidate(task.target, invalid));
        const auto saved = Take(task.archive->AfterSave(written, invalid));
        Refused(prepared->VerifyReadback(task, saved), "PATHING_CANDIDATE_READBACK");
    }
    for (const int mode : {0, 1, 2}) {
        Fixture f(mode == 1 ? 0 : 1); f.Stroke(); MapSaveTask task; f.Freeze(task); const auto prepared = f.Prepare(task);
        task.replacements.push_back(prepared->Replacement()); auto output = task.replacements;
        std::vector<std::string> removals;
        if (mode == 0) removals.push_back("war3map.shd");
        if (mode == 1) output.push_back({"war3map.mmp", {}});
        auto writer = *task.archive;
        if (mode == 2) { auto header = f.prefix; header.back() ^= std::byte{1}; writer = Take(writer.WithLeadingHeader(header)); }
        const auto written = Take(writer.SaveCandidate(task.target, output, removals));
        const auto saved = Take(writer.AfterSave(written, output, removals));
        Refused(prepared->VerifyReadback(task, saved), "PATHING_CANDIDATE_READBACK");
        if (mode == 2) {
            const auto disguised = Take(saved.WithLeadingHeader(f.prefix));
            Refused(prepared->VerifyReadback(task, disguised), "PATHING_CANDIDATE_SOURCE");
        }
    }
}

void UnlistedDerivedMembersAndUnsupportedSourceRemainExact() {
    Fixture f(1, {}, true); f.Stroke(); MapSaveTask task; f.Freeze(task);
    Require(f.source.HasOpaqueMembers() && !f.source.HasMember("war3map.shd"), "派生成员未列名不能猜Missing");
    const auto prepared = f.Prepare(task);
    for (std::size_t i = 5; i < 8; ++i) Require(prepared->SourceMembers()[i].present, "按明确名字捕获未列名派生成员");
    task.replacements.push_back(prepared->Replacement()); Require(prepared->VerifyCurrent(task, *f.edits).Succeeded(), "未列名来源仍核同一修订");
    const auto written = Take(task.archive->SaveCandidate(task.target, task.replacements));
    const auto saved = Take(task.archive->AfterSave(written, task.replacements));
    Require(prepared->VerifyReadback(task, saved).Succeeded() && !saved.HasMember("war3map.shd"), "匿名原块经既有追加链与精确回读保持");
    Fixture bad(1, {}, true, true); bad.Stroke(); MapSaveTask badTask; bad.Freeze(badTask);
    Refused(TerrainPathingRestrictionCandidate::Prepare(badTask, *bad.edits));
    Require(!std::filesystem::exists(badTask.target), "unsupported来源不能降格为空或Missing");
    Fixture metadata(1, {}, true, false, true); metadata.Stroke(); MapSaveTask metadataTask; metadata.Freeze(metadataTask);
    Require(!metadata.source.MetadataMembers().empty(), "真实来源记录MPQ元数据而非普通member");
    Refused(TerrainPathingRestrictionCandidate::Prepare(metadataTask, *metadata.edits), "PATHING_CANDIDATE_DOMAIN");
    Require(!std::filesystem::exists(metadataTask.target), "不通过writer清理元数据越过零删除域");
}

} // namespace
int main() {
    try {
        OriginalMembersSurviveOnlyWpmChanges(); TaskAndRevisionCannotBorrowQualification();
        DomainAndMalformedInputsFailWithoutPartialValue(); WorkingBudgetsAndSourceFailuresAreClosed();
        NativeMixedEditsRemainOutsideCandidateDomain();
        ReadbackRejectsWrongPreservedMembersAndWpm();
        UnlistedDerivedMembersAndUnsupportedSourceRemainExact();
        std::cout << "offline pathing candidate: source, APR, task, budgets, preserved derived bytes and readback passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "War3MapCore/MapData/TerrainSavePlan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "fixture operation failed";
        for (const auto& item : result.diagnostics) message += " | " + item.code + ": " + item.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) { return item.code == code; });
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-terrain-save-plan-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create private fixture directory");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

struct Fixture {
    Bytes w3e, wpm;
    MapDataEditSession edits{MapDocument::FromMembers({})};
    WorkspaceSnapshot source;
    TerrainStrokePoint point{512, 512};

    Fixture() {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9;
        terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"};
        terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192;
            vertex.waterHeightAndFlagsRaw = 7936;
            vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        w3e = Take(SerializeMapTerrain(terrain));
        wpm = {std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        wpm.resize(16 + 32 * 32, std::byte{0x82}); // 原限制与未知位必须保留。
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "attach source pair");
        source.aggregateSha256 = Take(Sha256Text("original synthetic workspace"));
        AddSource("map/war3map.w3e", w3e);
        AddSource("map/war3map.wpm", wpm);
    }
    void AddSource(std::string name, const Bytes& bytes) {
        source.files.push_back({std::filesystem::path(name), bytes.size(), 0, Take(Sha256(bytes))});
    }
    void Stroke(TerrainBrushKind kind, float strength = 10.0f) {
        TerrainBrush brush;
        brush.kind = kind; brush.radius = 0; brush.strength = strength;
        Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "apply synthetic terrain edit");
    }
    std::vector<MemberReplacement> Output() const { return edits.Replacements(); }
};

void DryHeightHasExplicitDerivedRisks() {
    Fixture fixture;
    const Bytes shadow{std::byte{7}, std::byte{11}}; // 不解释、归一化或伪造影图格式。
    fixture.AddSource("map/war3map.shd", shadow);
    fixture.AddSource("map/war3mapMap.blp", {}); // 真实空成员不等于缺失。
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto revision = fixture.edits.Revision();
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    Require(plan.SaveGate().Succeeded() && plan.Replacements().size() == 1, "bounded dry height keeps old gate");
    Require(plan.Members()[0].action == TerrainSaveMemberAction::WriteEdited, "W3E is explicit edited output");
    Require(plan.Members()[1].action == TerrainSaveMemberAction::Preserve, "height does not rebuild WPM");
    Require(plan.Members()[2].sourceState == TerrainSaveSourceState::Present
        && plan.Members()[2].outputSha256 == plan.Members()[2].sourceSha256, "SHD bytes preserved without interpretation");
    Require(plan.Members()[3].sourceState == TerrainSaveSourceState::Missing && !plan.Members()[3].outputSha256,
        "absent markers remain absent");
    Require(plan.Members()[4].sourceState == TerrainSaveSourceState::Present
        && plan.Members()[4].sourceBytes == 0 && plan.Members()[4].sourceSha256.has_value(), "empty minimap has an identity");
    Require(plan.Diagnostics().size() == 3 && plan.RequiresNativeVerification(), "derived gaps remain explicit");
    Require(HasCode(plan.Diagnostics(), "TERRAIN_SAVE_SHADOW_NOT_REBUILT")
        && HasCode(plan.Diagnostics(), "TERRAIN_SAVE_MARKERS_NOT_REBUILT")
        && HasCode(plan.Diagnostics(), "TERRAIN_SAVE_MINIMAP_NOT_REBUILT"), "one reason for each missing derivation");
    Require(plan.VerifyForSave(fixture.edits, fixture.source.aggregateSha256, fixture.Output()).Succeeded(), "exact current output validates");
    Require(fixture.edits.Revision() == revision, "planning does not edit or consume history");
}

void BlightOutputCannotBeChangedAfterPlanning() {
    Fixture fixture;
    fixture.Stroke(TerrainBrushKind::Blight);
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    auto output = fixture.Output();
    Require(plan.SaveGate().Succeeded() && plan.Replacements().size() == 2, "blight writes paired members");
    Require(plan.Members()[1].action == TerrainSaveMemberAction::WriteEdited, "WPM blight delta is explicit");
    Require(plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "paired output matches");
    const auto pathing = std::find_if(output.begin(), output.end(), [](const auto& item) { return item.name == "war3map.wpm"; });
    Require(pathing != output.end(), "paired pathing output exists");
    for (std::size_t index = 16; index < pathing->data.size(); ++index)
        Require((std::to_integer<unsigned>(pathing->data[index]) & ~0x20u) == 0x82u, "all non-blight pathing bits survive");
    pathing->data[16] ^= std::byte{4};
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "later pathing mutation refuses publication");
    output = fixture.Output(); output.pop_back();
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "missing paired output refuses publication");
    output = fixture.Output(); output.push_back({"WAR3MAP.W3E", output.front().data});
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "case-folded duplicate refuses publication");
    output = fixture.Output();
    const std::array removals{std::string("War3Map.WPM")};
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output, removals).Succeeded(), "paired removal refuses publication");
    Require(!plan.VerifyFrozenOutput(std::string(64, 'a'), output).Succeeded(), "different source refuses publication");
}

void OriginalSaveGateRemainsAuthoritative() {
    Fixture height;
    height.Stroke(TerrainBrushKind::Raise, 40.0f);
    const auto plan = Take(TerrainSavePlan::Prepare(height.source, height.edits));
    Require(plan.SaveGate().Succeeded() && plan.Members()[0].action == TerrainSaveMemberAction::WriteEdited,
        "超过历史原版单点实验幅度的干地高度按已证通行不变规则放行");
    Require(plan.Members()[1].action == TerrainSaveMemberAction::Preserve
        && plan.VerifyForSave(height.edits, height.source.aggregateSha256, height.Output()).Succeeded(),
        "干地高度保存保留原WPM而非清理作者未知/手画限制");
    auto wet = *height.edits.Terrain(); wet.vertices[4 * 9 + 4].textureAndFlagsRaw |= 0x40;
    Require(height.edits.SetTerrain(Take(SerializeMapTerrain(wet))).Succeeded(), "涉水高度仍可保留为工程草稿");
    const auto rejected = Take(TerrainSavePlan::Prepare(height.source, height.edits));
    Require(!rejected.SaveGate().Succeeded() && rejected.Members()[0].action == TerrainSaveMemberAction::Reject
        && HasCode(rejected.SaveGate().diagnostics, "MAP_TERRAIN_PATHING_UNVERIFIED"), "水语义未知时不借干地新规则放行");

    Fixture texture;
    Require(texture.edits.EditTerrainTextureSlot(TerrainTextureTable::Ground, 0, "Tg02").Succeeded(), "edit texture header draft");
    const auto textured = Take(TerrainSavePlan::Prepare(texture.source, texture.edits));
    Require(!textured.SaveGate().Succeeded(), "texture changes do not gain save approval");

    Fixture restrictions;
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::PathingRestriction; brush.radius = 0; brush.pathingRestrictions = 4;
    Require(restrictions.edits.EditTerrain(brush, std::span(&restrictions.point, 1), false).Succeeded(), "edit pathing restriction draft");
    const auto restricted = Take(TerrainSavePlan::Prepare(restrictions.source, restrictions.edits));
    Require(!restricted.SaveGate().Succeeded() && restricted.Members()[1].action == TerrainSaveMemberAction::Reject,
        "WPM-only restriction draft stays blocked");
}

void DryHeightFreezesRangePlacementsDefinitionsAndResources() {
    Fixture fixture;
    Bytes objects(12); objects[0] = std::byte{1}; // classic v1，空original/custom表
    fixture.AddSource("war3map.w3b", objects);
    fixture.Stroke(TerrainBrushKind::Raise, 512.0f);
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    Require(plan.SaveGate().Succeeded(), "高幅度干地修改须走实际共用保存计划");
    auto output = fixture.Output(); output.push_back({"war3map.w3b", objects});
    Require(plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(),
        "脚本模板可以重现源物编原字，不能凭成员名误报发生变化");
    output.back().data.push_back(std::byte{4});
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(),
        "改变静态对象定义不能借干地高度不变规则发布旧WPM");
    output = fixture.Output(); output.push_back({"war3map.w3i", {std::byte{25}}});
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "最终可玩范围变化必须拒绝");
    output = fixture.Output(); output.push_back({"PathTextures/changed.tga", {std::byte{1}}});
    const auto changedTexture = plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output);
    Require(!changedTexture.Succeeded() && HasCode(changedTexture.diagnostics, "TERRAIN_SAVE_PLAN_HEIGHT_RESOURCE_SCOPE"),
        "高度和导入占地资源混合保存不具备原通行不变证明");
    const std::array<std::string, 1> removals{"PathTextures/old.tga"};
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, fixture.Output(), removals).Succeeded(),
        "删除资源同样不能清理未知或作者手画通行位");
}

void SessionAndBaselineChangesInvalidatePlans() {
    Fixture fixture;
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    Require(fixture.edits.Undo() && fixture.edits.Redo(), "undo and redo exact terrain");
    Require(!plan.VerifyForSave(fixture.edits, fixture.source.aggregateSha256, fixture.Output()).Succeeded(),
        "identical bytes after undo-redo do not revive old revision");
    Fixture other;
    other.Stroke(TerrainBrushKind::Raise);
    Require(!plan.VerifyForSave(other.edits, fixture.source.aggregateSha256, other.Output()).Succeeded(), "foreign session rejected");

    const auto fresh = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    const auto revision = fixture.edits.Revision();
    const auto output = fixture.Output();
    fixture.edits.CommitSaved(MapDocument::FromMembers({}));
    Require(fixture.edits.Revision() == revision && fixture.edits.TerrainSourceRevision() != fresh.TerrainSourceRevision(),
        "source rebasing changes generation without ordinary edit revision");
    Require(!fresh.VerifyForSave(fixture.edits, fixture.source.aggregateSha256, output).Succeeded(), "saved baseline invalidates plan");
}

void PreservedMembersCannotBeSilentlyRebuilt() {
    Fixture fixture;
    const Bytes shadow{std::byte{5}, std::byte{13}};
    fixture.AddSource("map/war3map.shd", shadow);
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    auto output = fixture.Output();
    output.push_back({"war3map.shd", shadow});
    Require(plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "byte-identical preservation is allowed");
    output.back().data[0] = std::byte{255};
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "preview-normalized shadow is not a save source");
    output = fixture.Output(); output.push_back({"war3mapMap.blp", {std::byte{9}}});
    Require(!plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "unplanned minimap cannot be inserted");
    output = fixture.Output(); output.push_back({"scripts/user.lua", {std::byte{'x'}}});
    Require(plan.VerifyFrozenOutput(fixture.source.aggregateSha256, output).Succeeded(), "unrelated domains retain their own gates");
}

void DefaultMissingMinimapRemainsPreserved() {
    Fixture fixture;
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto plan = Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits));
    const auto& minimap = plan.Members()[4];
    Require(minimap.sourceState == TerrainSaveSourceState::Missing
        && minimap.action == TerrainSaveMemberAction::Preserve
        && !minimap.sourceBytes && !minimap.sourceSha256 && !minimap.outputBytes && !minimap.outputSha256,
        "普通Core Prepare对准确Missing MapBLP仍无新增资格");
    Require(plan.Replacements().size() == 1 && plan.Replacements()[0].name == "war3map.w3e"
        && HasCode(plan.Diagnostics(), "TERRAIN_SAVE_MINIMAP_NOT_REBUILT"),
        "新增Services私有准入不能改变Core默认Preserve计划或未重建告警");
}

void SourceErrorsNeverReturnPartialPlans() {
    Fixture fixture;
    fixture.Stroke(TerrainBrushKind::Raise);
    auto source = fixture.source;
    source.files.front().sha256 = Take(Sha256Text("different original terrain"));
    auto result = TerrainSavePlan::Prepare(source, fixture.edits);
    Require(!result.Succeeded() && !result.value, "different baseline fails with no partial plan");
    source = fixture.source;
    source.files.push_back({"WAR3MAP.W3E", fixture.w3e.size(), 0, Take(Sha256(fixture.w3e))});
    result = TerrainSavePlan::Prepare(source, fixture.edits);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "TERRAIN_SAVE_PLAN_SOURCE_AMBIGUOUS"), "root/map duplicate is ambiguous");
    source = fixture.source; source.aggregateSha256.clear();
    Require(!TerrainSavePlan::Prepare(source, fixture.edits).Succeeded(), "missing source identity fails");
    TerrainSavePlanLimits limits; limits.maximumBytes = 16;
    result = TerrainSavePlan::Prepare(fixture.source, fixture.edits, limits);
    Require(!result.Succeeded() && !result.value && HasCode(result.diagnostics, "TERRAIN_SAVE_PLAN_BUDGET"), "small budget refuses partial output");
    limits.maximumBytes = TerrainSavePlanLimits{}.maximumBytes + 1;
    Require(!TerrainSavePlan::Prepare(fixture.source, fixture.edits, limits).Succeeded(), "callers cannot enlarge hard budget");
}

void ArchiveRoundTripChecksPlannedAndPreservedBytes() {
    TempDirectory directory;
    Fixture fixture;
    const std::vector<MapArchiveMember> members{{"war3map.w3e", fixture.w3e}, {"war3map.wpm", fixture.wpm},
        {"war3map.shd", {std::byte{1}, std::byte{2}, std::byte{255}}}, {"war3mapMap.blp", {}}};
    const auto written = Take(WriteVerifiedArchive(directory.Path() / "source.w3x", members));
    const auto source = Take(MapArchiveSession::Open(written.path));
    fixture.Stroke(TerrainBrushKind::Blight);
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.VerifyForSave(fixture.edits, source.SourceSha256(), fixture.Output()).Succeeded(), "archive plan is source-bound");
    const auto candidate = Take(source.SaveCandidate(directory.Path() / "candidate.w3x", plan.Replacements()));
    const auto saved = Take(MapArchiveSession::Open(candidate.path));
    Require(plan.VerifyReadback(saved).Succeeded(), "reopen independently verifies edited and preserved identities");
    Require(!plan.VerifyReadback(source).Succeeded(), "old source is not edited readback");
    Require(Take(source.ReadMember("war3map.w3e")) == fixture.w3e, "source remains untouched");
}

void PutU32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// 原创未压缩 MPQ；清单与真实具名 hash 独立，以覆盖未列名正、负和坏编码。
Bytes OpaqueArchiveBytes(const std::vector<MapArchiveMember>& files, bool unsupportedShadow = false) {
    const auto blockCount = static_cast<std::uint32_t>(files.size());
    std::uint32_t hashCount = 8;
    while (hashCount < blockCount * 2) hashCount *= 2;
    std::uint32_t dataBytes = 0;
    for (const auto& file : files) dataBytes += static_cast<std::uint32_t>(file.data.size());
    const auto hashOffset = 32u + dataBytes, blockOffset = hashOffset + hashCount * 16;
    Bytes bytes(blockOffset + blockCount * 16);
    PutU32(bytes, 0, 0x1a51504d); PutU32(bytes, 4, 32); PutU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    PutU32(bytes, 12, 0x00030000); PutU32(bytes, 16, hashOffset); PutU32(bytes, 20, blockOffset);
    PutU32(bytes, 24, hashCount); PutU32(bytes, 28, blockCount);
    std::fill(bytes.begin() + hashOffset, bytes.begin() + blockOffset, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    std::uint32_t position = 32;
    for (std::uint32_t index = 0; index < blockCount; ++index) {
        const auto& file = files[index];
        std::copy(file.data.begin(), file.data.end(), bytes.begin() + position);
        auto bucket = crypt.HashString(file.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        for (;; bucket = (bucket + 1) & (hashCount - 1)) {
            const auto at = hashOffset + bucket * 16;
            std::uint32_t block = 0;
            std::memcpy(&block, bytes.data() + at + 12, sizeof(block));
            if (block != 0xffffffffu) continue;
            PutU32(bytes, at, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameA));
            PutU32(bytes, at + 4, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameB));
            PutU32(bytes, at + 8, 0); PutU32(bytes, at + 12, index); break;
        }
        const auto size = static_cast<std::uint32_t>(file.data.size());
        PutU32(bytes, blockOffset + index * 16, position); PutU32(bytes, blockOffset + index * 16 + 4, size);
        PutU32(bytes, blockOffset + index * 16 + 8, size);
        PutU32(bytes, blockOffset + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit
            | (unsupportedShadow && file.name == "war3map.shd" ? Mpq::kFilePatch : 0u));
        position += size;
    }
    crypt.EncryptBlock(bytes.data() + hashOffset, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockOffset, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    return bytes;
}

std::vector<MapArchiveMember> OpaqueFiles(const Fixture& fixture, std::string_view list) {
    const auto* first = reinterpret_cast<const std::byte*>(list.data());
    return {{"war3map.w3e", fixture.w3e}, {"war3map.wpm", fixture.wpm},
        {"opaque/keep.bin", {std::byte{0xc3}, std::byte{0}, std::byte{0xff}}},
        {"(listfile)", Bytes(first, first + list.size())}};
}

void UnlistedMembersAreFrozenAndReadBack() {
    TempDirectory directory;
    Fixture fixture;
    auto files = OpaqueFiles(fixture, "war3map.w3e\nwar3map.wpm\n");
    const Bytes shadow{std::byte{71}, std::byte{0}, std::byte{255}};
    const Bytes markers{std::byte{0x92}, std::byte{0}, std::byte{0x1b}};
    files.push_back({"war3map.shd", shadow});
    files.push_back({"war3map.mmp", markers}); // 不解释 opaque 内容，更不规范化。
    files.push_back({"war3mapMap.blp", {}}); // 未列名的真实空文件也是 Present。
    const auto path = directory.Path() / "opaque-source.w3x";
    const auto original = OpaqueArchiveBytes(files);
    Require(IO::WriteFileBytes(path, original).Succeeded(), "write private opaque fixture");
    const auto source = Take(MapArchiveSession::Open(path));
    Require(source.HasOpaqueMembers() && !source.HasMember("war3map.shd") && !source.HasMember("war3map.mmp"),
        "fixture has unlisted derived members");
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.SaveGate().Succeeded(), "source exact capture does not change the existing dry-height gate");
    Require(plan.Members()[2].sourceState == TerrainSaveSourceState::Present
        && plan.Members()[2].sourceSha256 == Take(Sha256(shadow)), "unlisted shadow has exact identity");
    Require(plan.Members()[3].sourceState == TerrainSaveSourceState::Present
        && plan.Members()[3].sourceSha256 == Take(Sha256(markers)), "opaque marker bytes have exact identity");
    Require(plan.Members()[4].sourceState == TerrainSaveSourceState::Present
        && plan.Members()[4].sourceBytes == 0 && plan.Members()[4].sourceSha256 == Take(Sha256(Bytes{})),
        "unlisted empty image is present and distinct from missing");
    auto output = fixture.Output(); output.push_back({"WAR3MAP.SHD", shadow});
    Require(plan.VerifyFrozenOutput(source.SourceSha256(), output).Succeeded(), "exact shadow preservation is verifiable");
    output.back().data[1] = std::byte{1};
    Require(!plan.VerifyFrozenOutput(source.SourceSha256(), output).Succeeded(), "unlisted shadow cannot be rewritten later");
    const auto candidate = Take(source.SaveCandidate(directory.Path() / "candidate.w3x", plan.Replacements()));
    const auto saved = Take(MapArchiveSession::Open(candidate.path));
    Require(!saved.HasMember("war3map.shd") && plan.VerifyReadback(saved).Succeeded(),
        "actual readback verifies hidden derived members without changing listfile semantics");
    auto alteredFiles = files;
    for (auto& file : alteredFiles) {
        if (file.name == "war3map.shd") file.data = {std::byte{8}};
        for (const auto& replacement : plan.Replacements())
            if (file.name == replacement.name) file.data = replacement.data;
    }
    const auto altered = directory.Path() / "altered.w3x";
    Require(IO::WriteFileBytes(altered, OpaqueArchiveBytes(alteredFiles)).Succeeded(), "write independent altered readback fixture");
    Require(!plan.VerifyReadback(Take(MapArchiveSession::Open(altered))).Succeeded(),
        "independent readback detects changed unlisted shadow");
    Require(Take(IO::ReadFileBytes(path)) == original, "planning and successful candidate preserve all source archive bytes");
}

void ExactMissingAndRequiredMembersDoNotDependOnListfile() {
    TempDirectory directory;
    Fixture fixture;
    const auto files = OpaqueFiles(fixture, "war3map.w3e\nwar3map.wpm\n");
    const auto path = directory.Path() / "missing-derived-source.w3x";
    Require(IO::WriteFileBytes(path, OpaqueArchiveBytes(files)).Succeeded(), "write hidden terrain fixture");
    const auto source = Take(MapArchiveSession::Open(path));
    Require(source.HasOpaqueMembers(), "unrelated source blocks remain opaque");
    fixture.Stroke(TerrainBrushKind::Blight);
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.SaveGate().Succeeded(), "exact required-member bytes match the attached baseline");
    for (std::size_t index = 2; index < plan.Members().size(); ++index)
        Require(plan.Members()[index].sourceState == TerrainSaveSourceState::Missing
            && !plan.Members()[index].sourceSha256, "exact negative lookup proves missing despite unrelated opaque blocks");
    const auto candidate = Take(source.SaveCandidate(directory.Path() / "hidden-candidate.w3x", plan.Replacements()));
    const auto saved = Take(MapArchiveSession::Open(candidate.path));
    Require(plan.VerifyReadback(saved).Succeeded(), "hidden required members and missing derivatives round trip");
    auto unexpected = files;
    unexpected.push_back({"war3map.mmp", {}});
    for (auto& file : unexpected)
        for (const auto& replacement : plan.Replacements())
            if (file.name == replacement.name) file.data = replacement.data;
    const auto changed = directory.Path() / "unexpected.w3x";
    Require(IO::WriteFileBytes(changed, OpaqueArchiveBytes(unexpected)).Succeeded(), "write independent unexpected-member readback fixture");
    Require(!plan.VerifyReadback(Take(MapArchiveSession::Open(changed))).Succeeded(),
        "readback rejects a newly inserted derived member even when it is empty");

    const auto hiddenPath = directory.Path() / "hidden-required-source.w3x";
    const auto hiddenOriginal = OpaqueArchiveBytes(OpaqueFiles(fixture, "opaque/keep.bin\n"));
    Require(IO::WriteFileBytes(hiddenPath, hiddenOriginal).Succeeded(), "write unlisted required-member fixture");
    const auto hidden = Take(MapArchiveSession::Open(hiddenPath));
    Require(!hidden.HasMember("war3map.w3e"), "fixture required terrain is unlisted");
    const auto hiddenPlan = Take(TerrainSavePlan::Prepare(hidden, fixture.edits));
    Require(hiddenPlan.Members()[0].sourceState == TerrainSaveSourceState::Present,
        "the plan binds hidden required-member bytes rather than guessing absence");
    const auto blocked = hidden.SaveCandidate(directory.Path() / "hidden-rejected.w3x", hiddenPlan.Replacements());
    Require(!blocked.Succeeded() && !blocked.value && HasCode(blocked.diagnostics, "OPAQUE_NEW_MEMBER_REJECTED"),
        "exact source capture does not bypass the archive's unlisted replacement restriction");
    Require(!std::filesystem::exists(directory.Path() / "hidden-rejected.w3x")
        && Take(IO::ReadFileBytes(hiddenPath)) == hiddenOriginal, "unsupported hidden-required save leaves no candidate or source change");
}

void ArchiveCaptureFailuresAreAtomicAndKeepHistory() {
    TempDirectory directory;
    Fixture fixture;
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto revision = fixture.edits.Revision();
    const auto output = fixture.Output();
    auto files = OpaqueFiles(fixture, "war3map.w3e\nwar3map.wpm\n");
    files.push_back({"war3map.shd", Bytes(128, std::byte{0x55})});
    const auto original = OpaqueArchiveBytes(files);
    const auto path = directory.Path() / "source.w3x";
    Require(IO::WriteFileBytes(path, original).Succeeded(), "write exact-source failure fixture");
    const auto source = Take(MapArchiveSession::Open(path));
    TerrainSavePlanLimits limits;
    limits.maximumBytes = fixture.w3e.size() + fixture.wpm.size() + 127;
    auto plan = TerrainSavePlan::Prepare(source, fixture.edits, limits);
    Require(!plan.Succeeded() && !plan.value, "unlisted derived bytes also consume the capture budget");
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "hold private source writer handle without writing");
    plan = TerrainSavePlan::Prepare(source, fixture.edits);
    CloseHandle(file);
    Require(!plan.Succeeded() && !plan.value && HasCode(plan.diagnostics, "SOURCE_LEASE_FAILED"),
        "active source writer refuses an exact plan, never an unknown-member fallback");
    const auto unsupportedPath = directory.Path() / "unsupported-shadow.w3x";
    const auto unsupported = OpaqueArchiveBytes(files, true);
    Require(IO::WriteFileBytes(unsupportedPath, unsupported).Succeeded(), "write private unsupported encoding fixture");
    const auto unreadable = Take(MapArchiveSession::Open(unsupportedPath));
    plan = TerrainSavePlan::Prepare(unreadable, fixture.edits);
    Require(!plan.Succeeded() && !plan.value && HasCode(plan.diagnostics, "EXACT_MEMBER_ENCODING_UNSUPPORTED"),
        "unreadable unlisted derivative refuses a plan instead of being called absent");
    Require(Take(IO::ReadFileBytes(unsupportedPath)) == unsupported && Take(IO::ReadFileBytes(path)) == original,
        "capture failures do not alter either source archive");
    const auto afterFailure = fixture.edits.Replacements();
    Require(fixture.edits.Revision() == revision && afterFailure.size() == output.size()
        && std::equal(afterFailure.begin(), afterFailure.end(), output.begin(), [](const auto& left, const auto& right) {
            return left.name == right.name && left.data == right.data;
        }) && fixture.edits.CanUndo(),
        "capture failures preserve edit revision, exact output and undo history");
    auto changed = original;
    changed[32] ^= std::byte{1};
    Require(IO::WriteFileBytes(path, changed).Succeeded(), "simulate a private source change after opening");
    plan = TerrainSavePlan::Prepare(source, fixture.edits);
    Require(!plan.Succeeded() && !plan.value && HasCode(plan.diagnostics, "SOURCE_CHANGED"),
        "source SHA is rechecked before using snapshot identities");
    Require(Take(IO::ReadFileBytes(path)) == changed && fixture.edits.Revision() == revision,
        "stale-source refusal does not restore or write the source or consume history");
}

} // namespace

int main() {
    try {
        DryHeightHasExplicitDerivedRisks();
        BlightOutputCannotBeChangedAfterPlanning();
        OriginalSaveGateRemainsAuthoritative();
        DryHeightFreezesRangePlacementsDefinitionsAndResources();
        SessionAndBaselineChangesInvalidatePlans();
        PreservedMembersCannotBeSilentlyRebuilt();
        DefaultMissingMinimapRemainsPreserved();
        SourceErrorsNeverReturnPartialPlans();
        ArchiveRoundTripChecksPlannedAndPreservedBytes();
        UnlistedMembersAreFrozenAndReadBack();
        ExactMissingAndRequiredMembersDoNotDependOnListfile();
        ArchiveCaptureFailuresAreAtomicAndKeepHistory();
        std::cout << "Terrain save plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

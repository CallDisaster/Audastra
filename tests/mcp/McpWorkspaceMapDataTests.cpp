#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataFieldPatchCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <tuple>

using namespace War3Map;
using namespace War3Map::MapData;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& row) { return row.code == code; });
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(stream), "write original fixture");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, std::as_bytes(std::span(text))); }
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("audastrafields-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<Mcp::McpWorkspace> workspace;
    std::vector<std::byte> original;
    Fixture(bool fiveMemberFields = false, bool soundFields = false, std::size_t soundCount = 1) {
        const auto ui = root / "ui";
        Write(ui / "config", "base\n");
        Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n");
        for (const auto* file : {"action.txt", "call.txt", "event.txt", "condition.txt"}) Write(ui / "base" / file, "");
        auto catalog = Trigger::UiCatalog::Load(ui); Require(catalog.Succeeded(), "fixture catalog");
        auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); Require(empty.Succeeded(), "fixture empty seal");
        auto pair = Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value); Require(pair.Succeeded(), "fixture pair");
        Write(root / "source/map/war3map.wtg", pair.value->wtg);
        Write(root / "source/map/war3map.wct", pair.value->wct);
        RegionsDocument regions;
        regions.entries.push_back({-128.f, -64.f, 128.f, 64.f, "Original region", 31, {}, "", {12, 34, 56, 78}});
        auto bytes = regions.Serialize(); Require(bytes.Succeeded(), "fixture native W3R"); original = *bytes.value;
        Write(root / "source/map/war3map.w3r", original);
        Write(root / "source/map/war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes);
        Write(root / "source/map/war3map.wts", "// retain\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\n");
        if (soundFields) {
            SoundsDocument sounds;
            for (std::size_t index = 0; index < soundCount; ++index) {
                SoundRecord sound{}; sound.name = "Original sound " + std::to_string(index); sound.filePath = "Sound\\Owned.wav";
                sound.eaxSetting = "DefaultEAXON"; sound.volume = 64; sound.pitch = 1; sound.fadeInRate = 20; sound.fadeOutRate = 30;
                sounds.entries.push_back(std::move(sound));
            }
            const auto soundBytes = sounds.Serialize(); Require(soundBytes.Succeeded(), "complete native W3S fixture");
            Write(root / "source/map/war3map.w3s", *soundBytes.value);
        } else Write(root / "source/map/war3map.w3s", "bad");
        if (fiveMemberFields) {
            UnitPlacements units;
            UnitPlacement unit{}; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; unit.creationId = 17;
            unit.unknownOwnerBytes = {0xa5, 0x5a}; unit.targetAcquisition = -1;
            units.units.push_back(unit);
            CamerasDocument cameras;
            CameraRecord camera{}; camera.name = "Original camera"; camera.targetDistance = 1600;
            camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = 100;
            cameras.entries.push_back(camera);
            const auto nativeUnits = units.Serialize(), nativeCameras = cameras.Serialize();
            Require(nativeUnits.Succeeded() && nativeCameras.Succeeded(), "five-member native fixtures");
            Write(root / "source/map/war3mapUnits.doo", *nativeUnits.value);
            Write(root / "source/map/war3map.w3c", *nativeCameras.value);
        }
        auto opened = Mcp::McpWorkspace::Open(root / "source", ui); Require(opened.Succeeded(), "actual workspace open");
        workspace = std::move(*opened.value);
    }
    ~Fixture() {
        workspace.reset();
        std::error_code error; std::filesystem::remove_all(root, error); // 固定自有临时夹具，无协议路径。
    }
    Mcp::McpWorkspaceMapDataContext Context() {
        auto context = workspace->MapDataContext("fixture-owned", 1); Require(context.Succeeded(), "current actual map data");
        return std::move(*context.value);
    }
    PreparedMapDataFields Plan(const char* fieldId, const char* value) {
        return Plan(MapDataKind::Regions, fieldId, value);
    }
    PreparedMapDataFields Plan(MapDataKind kind, const char* fieldId, const char* value, std::size_t row = 0) {
        auto context = Context(); const auto* field = FindMapField(kind, fieldId); Require(field != nullptr, "field catalog");
        auto old = ReadMapField(*context.document, kind, row, *field);
        auto next = ParseMapFieldInput(*field, value); auto key = MapFieldRowKey(*context.document, kind, row);
        const auto& source = context.document->Member(kind);
        Require(!source.sources.empty(), "typed field source is present");
        auto sha = Sha256(source.sources[0].data);
        const auto report = [kind, fieldId](const char* operation, const auto& result) {
            if (result.Succeeded() && result.value) return;
            std::cerr << "typed field input: kind=" << MapDataKindId(kind) << " field=" << fieldId
                << " operation=" << operation << '\n';
            for (const auto& row : result.diagnostics) std::cerr << row.code << ": " << row.message << '\n';
        };
        report("read", old); report("parse", next); report("row-key", key); report("sha", sha);
        Require(old.Succeeded() && old.value && next.Succeeded() && next.value
            && key.Succeeded() && key.value && sha.Succeeded() && sha.value, "typed field input");
        const std::array edits{MapFieldEdit{kind, row, *key.value, fieldId, *old.value, *next.value}};
        auto plan = PrepareMapDataFields(*context.original, *context.document, *sha.value, edits);
        Require(plan.Succeeded(), "pure prepared fields"); return std::move(*plan.value);
    }
};
void CumulativeOverlayRollbackAndDirectorySeed() {
    Fixture f;
    const auto initial = f.Context(); Require(initial.document->Member(MapDataKind::Sounds).state == MapDataState::Invalid, "invalid source preserved");
    Require(!f.workspace->ApplyMapDataPlan(f.Plan("left", "-127")).Succeeded(), "no implicit transaction");
    const auto revision = f.workspace->Revision();
    auto begun = f.workspace->BeginEdit(revision); Require(begun.Succeeded(), "explicit begin");
    auto first = f.workspace->ApplyMapDataPlan(f.Plan("left", "-127")); Require(first.Succeeded(), "first real overlay");
    const auto identity = f.workspace->ObjectRevision();
    const auto refused = f.workspace->ApplyMapDataPlan(f.Plan("right", "129"), [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "RECEIPT_REJECTED", "实际发布前回执拒绝");
    });
    Require(!refused.Succeeded() && f.workspace->ObjectRevision() == identity, "refusal keeps opaque CAS");
    Require(f.Context().document->Regions()->entries[0].right == 128.f, "refusal keeps original field bytes");
    Require(f.workspace->ApplyMapDataPlan(f.Plan("left", "-126")).Succeeded(), "same field repeated actual overlay");
    Require(f.workspace->ApplyMapDataPlan(f.Plan("right", "130")).Succeeded(), "second final field");
    auto current = f.Context(); auto frozen = FreezeOriginalMapDataFields(*current.original, *current.document);
    Require(frozen.Succeeded() && frozen.value->Operations().size() == 2, "repeated field freezes only final original difference");
    auto preview = f.workspace->PreviewEdit(*begun.value, revision);
    Require(preview.Succeeded() && preview.value->changes.size() == 1 && preview.value->changes[0].kind == "map-data", "only original native member overlay");
    auto validation = f.workspace->ValidateEdit(*begun.value, revision); Require(validation.satisfied, "actual original parsed validation");
    Require(f.workspace->ExportCandidate(*begun.value, revision, f.root / "output").Succeeded(), "original directory publish");
    Require(f.Context().document->Regions()->entries[0].left == -126.f, "directory commit keeps current typed overlay");
    auto secondBegin = f.workspace->BeginEdit(f.workspace->Revision()); Require(secondBegin.Succeeded(), "next explicit begin");
    auto seeded = f.workspace->PreviewEdit(*secondBegin.value, f.workspace->Revision());
    Require(seeded.Succeeded() && seeded.value->changes.size() == 1, "next original source transaction reseeds committed native member");
    Require(f.workspace->ApplyMapDataPlan(f.Plan("left", "-125")).Succeeded(), "post-directory field edit");
    Require(f.workspace->Rollback(*secondBegin.value, f.workspace->Revision()).Succeeded(), "original rollback");
    Require(f.Context().document->Regions()->entries[0].left == -126.f, "rollback returns committed typed revision");
    Require(f.Context().document->Member(MapDataKind::Sounds).state == MapDataState::Invalid, "invalid source survives every overlay");
}
void SourceAndSealedCodecRefuseBeforePublish() {
    Fixture f; const auto context = f.Context();
    auto source = DirectoryWorkspace::Open(f.root / "source"); Require(source.Succeeded(), "fixture directory snapshot");
    const std::array kinds{MapDataKind::Regions};
    auto forged = source.value->Snapshot(); forged.files[0].sha256 = std::string(64, 'f');
    for (auto& file : forged.files) if (file.relativePath.filename() == "war3map.w3r") file.sha256 = std::string(64, 'e');
    Require(!MapDataFieldPatchCodec::Seal(*context.original, *context.document, forged, kinds).Succeeded(), "unbound directory bytes refused");
    const auto revision = f.workspace->Revision(); auto begun = f.workspace->BeginEdit(revision); Require(begun.Succeeded(), "source test begin");
    auto plan = f.Plan("left", "-127"); const auto identity = f.workspace->ObjectRevision();
    auto changed = f.original; changed[8] ^= std::byte{1}; Write(f.root / "source/map/war3map.w3r", changed);
    Require(!f.workspace->ApplyMapDataPlan(plan).Succeeded() && f.workspace->ObjectRevision() == identity, "source drift refuses before overlay/revision");
    Write(f.root / "source/map/war3map.w3r", f.original);
    Require(f.workspace->Rollback(*begun.value, revision).Succeeded(), "restore allows owned transaction rollback");
}
void DSealedDirectorySaveReopenReseedAndRollback() {
    Fixture f;
    const auto initial = f.Context();
    const auto originalW3i = initial.document->Member(MapDataKind::Settings).sources.front().data;
    const auto originalWts = initial.document->Member(MapDataKind::Strings).sources.front().data;
    const auto revision = f.workspace->Revision(); const auto begun = f.workspace->BeginEdit(revision);
    Require(begun.Succeeded(), "D显式原事务");
    const auto appliedWts = f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Strings, "value", "共享描述"));
    if (!appliedWts.Succeeded())
        for (const auto& row : appliedWts.diagnostics) std::cerr << row.code << ": " << row.message << '\n';
    Require(appliedWts.Succeeded(), "WTS沿原密封overlay提交");
    const auto beforeDenied = f.workspace->ObjectRevision();
    const auto denied = f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Settings, "author", "denied"), [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "D_REJECTED", "D回执拒绝");
    });
    Require(!denied.Succeeded() && f.workspace->ObjectRevision() == beforeDenied
        && f.Context().document->Member(MapDataKind::Settings).sources.front().data == originalW3i,
        "晚失败不能发布W3I身份/字节，保留已获准WTS");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Settings, "author", "保存作者")).Succeeded(), "W3I与WTS共享原字段事务");
    auto current = f.Context(); const auto frozen = FreezeOriginalMapDataFields(*current.original, *current.document);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 2 && frozen.value->Operations().size() == 2, "D两成员完整冻结");
    auto preview = f.workspace->PreviewEdit(*begun.value, revision);
    Require(preview.Succeeded() && preview.value->changes.size() == 2, "D前后SHA进入原preview");
    Require(f.workspace->ValidateEdit(*begun.value, revision).satisfied, "D走原ValidateEdit");
    const auto output = f.root / "d-output";
    Require(f.workspace->ExportCandidate(*begun.value, revision, output).Succeeded(), "D正式目录候选发布");
    auto opened = DirectoryWorkspace::Open(output); Require(opened.Succeeded(), "独立重开真实目录候选");
    auto reopened = MapDocument::FromDirectory(*opened.value);
    Require(reopened.Succeeded() && reopened.value->Settings()->author == "保存作者"
        && reopened.value->ResolveString(reopened.value->Settings()->description).value == std::optional<std::string>("共享描述"),
        "D保存独立回读作者和WTS真实解析结果");
    auto second = f.workspace->BeginEdit(f.workspace->Revision()); Require(second.Succeeded(), "D保存后新原事务");
    auto seeded = f.workspace->PreviewEdit(*second.value, f.workspace->Revision());
    Require(seeded.Succeeded() && seeded.value->changes.size() == 2, "D已提交成员通过同一codec完整reseal，不遗漏Settings/Strings");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Strings, "value", "临时描述")).Succeeded(), "D保存后继续编辑");
    Require(f.workspace->Rollback(*second.value, f.workspace->Revision()).Succeeded()
        && f.Context().document->ResolveString("TRIGSTR_42").value == std::optional<std::string>("共享描述"), "原Rollback恢复最近已提交D修订");
    auto untouched = DirectoryWorkspace::Open(f.root / "source"); auto source = MapDocument::FromDirectory(*untouched.value);
    Require(source.Succeeded() && source.value->Member(MapDataKind::Settings).sources.front().data == originalW3i
        && source.value->Member(MapDataKind::Strings).sources.front().data == originalWts, "来源目录原W3I/WTS未被候选保存改写");
}
void FiveMemberReseedRefusalsCommitAndRollback() {
    Fixture f(true);
    const auto initial = f.Context();
    const auto revision = f.workspace->Revision();
    const auto begun = f.workspace->BeginEdit(revision); Require(begun.Succeeded(), "five-member original transaction");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Settings, "author", "Five member author")).Succeeded(), "five-member W3I");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Strings, "value", "Five member description")).Succeeded(), "five-member WTS");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Units, "x", "125.5")).Succeeded(), "five-member native unit");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Regions, "left", "-127")).Succeeded(), "five-member region");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Cameras, "distance", "1700")).Succeeded(), "five-member camera");
    const auto current = f.Context();
    const auto frozen = FreezeOriginalMapDataFields(*current.original, *current.document);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 5 && frozen.value->Operations().size() == 5,
        "five independently edited native members freeze exactly five operations");
    const auto beforeRefusal = f.workspace->ObjectRevision();
    const auto previewBefore = f.workspace->PreviewEdit(*begun.value, revision);
    Require(previewBefore.Succeeded() && previewBefore.value->changes.size() == 5, "five original overlay entries");
    const auto source = DirectoryWorkspace::Open(f.root / "source"); Require(source.Succeeded(), "five-member exact original directory snapshot");
    const std::array allFields{MapDataKind::Settings, MapDataKind::Strings, MapDataKind::Units, MapDataKind::Regions, MapDataKind::Cameras};
    const auto valid = MapDataFieldPatchCodec::Seal(*current.original, *current.document, source.value->Snapshot(), allFields);
    Require(valid.Succeeded(), "five-member codec seal uses exact original directory source");
    const std::array tooMany{MapDataKind::Settings, MapDataKind::Strings, MapDataKind::Units,
        MapDataKind::Regions, MapDataKind::Cameras, MapDataKind::Doodads};
    const std::array sound{MapDataKind::Sounds};
    const std::array unknown{static_cast<MapDataKind>(127)};
    const std::array duplicate{MapDataKind::Settings, MapDataKind::Settings};
    for (const auto kinds : {std::span<const MapDataKind>(tooMany), std::span<const MapDataKind>(sound),
            std::span<const MapDataKind>(unknown), std::span<const MapDataKind>(duplicate)})
        Require(HasCode(MapDataFieldPatchCodec::Seal(*current.original, *current.document, source.value->Snapshot(), kinds), "MAP_FIELD_PATCH_SCOPE"),
            "over-budget, sound, unknown and duplicate member scopes refuse before sealed patch exists");
    auto forgedSource = source.value->Snapshot();
    for (auto& file : forgedSource.files)
        if (file.relativePath.filename() == "war3map.w3c") file.sha256 = std::string(64, 'e');
    Require(HasCode(MapDataFieldPatchCodec::Seal(*current.original, *current.document, forgedSource, allFields), "MAP_FIELD_PATCH_SOURCE"),
        "last member unbound SHA refuses complete five-member batch");
    const auto previewAfter = f.workspace->PreviewEdit(*begun.value, revision);
    Require(f.workspace->ObjectRevision() == beforeRefusal && f.workspace->Revision() == revision
        && previewAfter.Succeeded() && previewAfter.value->changes.size() == previewBefore.value->changes.size(),
        "all rejected scopes leave original revision and complete overlay count unchanged");
    for (std::size_t i = 0; i < previewBefore.value->changes.size(); ++i) {
        const auto& before = previewBefore.value->changes[i]; const auto& after = previewAfter.value->changes[i];
        Require(before.path == after.path && before.kind == after.kind && before.beforeSha256 == after.beforeSha256
            && before.afterSha256 == after.afterSha256, "refused complete batch retains every overlay member identity and SHA");
    }
    Require(f.workspace->ValidateEdit(*begun.value, revision).satisfied, "five-member original ValidateEdit");
    const auto output = f.root / "five-output";
    Require(f.workspace->ExportCandidate(*begun.value, revision, output).Succeeded(), "five-member actual directory candidate commit");
    auto reopenedDirectory = DirectoryWorkspace::Open(output); Require(reopenedDirectory.Succeeded(), "independent five-member candidate open");
    const auto reopened = MapDocument::FromDirectory(*reopenedDirectory.value); Require(reopened.Succeeded(), "independent five-member candidate native parse");
    for (const auto kind : allFields)
        Require(reopened.value->Member(kind).sources.front().data == current.document->Member(kind).sources.front().data,
            "independent candidate retains the whole actual native member, including unedited and opaque fields");
    auto nextRevision = f.workspace->Revision();
    auto second = f.workspace->BeginEdit(nextRevision); Require(second.Succeeded(), "five-member next original transaction reseeds all five sealed members");
    auto seeded = f.workspace->PreviewEdit(*second.value, nextRevision);
    Require(seeded.Succeeded() && seeded.value->changes.size() == 5, "reseed never silently drops a committed native member");
    Require(f.workspace->ApplyMapDataPlan(f.Plan(MapDataKind::Strings, "value", "temporary")).Succeeded(), "temporary edit after five-member reseed");
    Require(f.workspace->Rollback(*second.value, nextRevision).Succeeded(), "five-member original rollback");
    const auto rolledBack = f.Context();
    for (const auto kind : allFields)
        Require(rolledBack.document->Member(kind).sources.front().data == current.document->Member(kind).sources.front().data,
            "rollback restores every last committed native byte, not the initial source");
    const auto originalDirectory = DirectoryWorkspace::Open(f.root / "source"); Require(originalDirectory.Succeeded(), "five-member original directory reopens unchanged");
    const auto original = MapDocument::FromDirectory(*originalDirectory.value); Require(original.Succeeded(), "five-member source native parse");
    for (const auto kind : allFields)
        Require(original.value->Member(kind).sources.front().data == initial.document->Member(kind).sources.front().data,
            "candidate commit and rollback never rewrite the initial directory members");
    Require(rolledBack.document->Member(MapDataKind::Sounds).sources.front().data == initial.document->Member(MapDataKind::Sounds).sources.front().data
        && rolledBack.document->Member(MapDataKind::Sounds).state == MapDataState::Invalid,
        "adding W3I/WTS permission preserves unrelated invalid sound bytes and state");
}
void SixMemberSoundsRetainCumulativeBudgetAndOriginalTransaction() {
    Fixture fixture(true, true, 10);
    const auto initial = fixture.Context(); const auto revision = fixture.workspace->Revision();
    const auto begun = fixture.workspace->BeginEdit(revision); Require(begun.Succeeded(), "six-member original explicit transaction");
    for (const auto& request : std::array{
        std::tuple{MapDataKind::Settings, "author", "Six member author"}, std::tuple{MapDataKind::Strings, "value", "Six member description"},
        std::tuple{MapDataKind::Units, "x", "125.5"}, std::tuple{MapDataKind::Regions, "left", "-127"}, std::tuple{MapDataKind::Cameras, "distance", "1700"}})
        Require(fixture.workspace->ApplyMapDataPlan(fixture.Plan(std::get<0>(request), std::get<1>(request), std::get<2>(request))).Succeeded(),
            "existing five domains share the original transaction with sounds");
    for (std::size_t row = 0; row < 9; ++row) {
        Require(fixture.workspace->ApplyMapDataPlan(fixture.Plan(MapDataKind::Sounds, "volume", "80", row)).Succeeded(), "sound numeric native overlay");
        Require(fixture.workspace->ApplyMapDataPlan(fixture.Plan(MapDataKind::Sounds, "fade_in", "40", row)).Succeeded(), "sound fade-in native overlay");
        Require(fixture.workspace->ApplyMapDataPlan(fixture.Plan(MapDataKind::Sounds, "fade_out", "50", row)).Succeeded(), "sound fade-out native overlay");
    }
    const auto current = fixture.Context(); const auto frozen = FreezeOriginalMapDataFields(*current.original, *current.document);
    Require(frozen.Succeeded() && frozen.value->Members().size() == 6 && frozen.value->Operations().size() == 32,
        "six actual native members share the exact 32 cumulative original-field budget");
    const auto identity = fixture.workspace->ObjectRevision(); const auto preview = fixture.workspace->PreviewEdit(*begun.value, revision);
    Require(preview.Succeeded() && preview.value->changes.size() == 6, "complete six-member overlay has original before/after SHA");
    const auto refused = fixture.workspace->ApplyMapDataPlan(fixture.Plan(MapDataKind::Sounds, "volume", "90", 9));
    Require(HasCode(refused, "MAP_FIELD_CUMULATIVE_COUNT") && fixture.workspace->ObjectRevision() == identity
        && fixture.Context().document->Member(MapDataKind::Sounds).sources.front().data == current.document->Member(MapDataKind::Sounds).sources.front().data,
        "33rd cumulative field refuses before publishing bytes or CAS identity");
    const std::array kinds{MapDataKind::Settings, MapDataKind::Strings, MapDataKind::Units, MapDataKind::Regions, MapDataKind::Cameras, MapDataKind::Sounds};
    const auto directory = DirectoryWorkspace::Open(fixture.root / "source"); Require(directory.Succeeded(), "owned six-member source snapshot");
    Require(MapDataFieldPatchCodec::Seal(*current.original, *current.document, directory.value->Snapshot(), kinds).Succeeded(),
        "six-member field codec seals actual source-bound native records");
    const std::array overScope{MapDataKind::Settings, MapDataKind::Strings, MapDataKind::Units, MapDataKind::Regions,
        MapDataKind::Cameras, MapDataKind::Sounds, MapDataKind::Doodads};
    Require(HasCode(MapDataFieldPatchCodec::Seal(*current.original, *current.document, directory.value->Snapshot(), overScope), "MAP_FIELD_PATCH_SCOPE"),
        "seventh member cannot expand the native field codec to placement-only domain");
    auto forged = directory.value->Snapshot();
    for (auto& file : forged.files) if (file.relativePath.filename() == "war3map.w3s") file.sha256 = std::string(64, 'e');
    Require(HasCode(MapDataFieldPatchCodec::Seal(*current.original, *current.document, forged, kinds), "MAP_FIELD_PATCH_SOURCE"),
        "unbound last W3S source rejects whole six-member patch");
    const auto afterRefusal = fixture.workspace->PreviewEdit(*begun.value, revision);
    Require(afterRefusal.Succeeded() && afterRefusal.value->changes.size() == preview.value->changes.size()
        && fixture.workspace->ObjectRevision() == identity && fixture.workspace->Revision() == revision, "refusals retain full transaction identity and member count");
    for (std::size_t index = 0; index < preview.value->changes.size(); ++index) {
        const auto& before = preview.value->changes[index]; const auto& after = afterRefusal.value->changes[index];
        Require(before.path == after.path && before.kind == after.kind && before.beforeSha256 == after.beforeSha256 && before.afterSha256 == after.afterSha256,
            "six-member refusal retains every original overlay SHA and path");
    }
    Require(fixture.workspace->ValidateEdit(*begun.value, revision).satisfied, "six-member original ValidateEdit");
    const auto output = fixture.root / "six-output";
    Require(fixture.workspace->ExportCandidate(*begun.value, revision, output).Succeeded(), "actual six-member original directory candidate commit");
    const auto reopenedDirectory = DirectoryWorkspace::Open(output); Require(reopenedDirectory.Succeeded(), "independent six-member candidate opens");
    const auto reopened = MapDocument::FromDirectory(*reopenedDirectory.value); Require(reopened.Succeeded(), "independent six-member native parse");
    for (const auto kind : kinds) Require(reopened.value->Member(kind).sources.front().data == current.document->Member(kind).sources.front().data,
        "candidate independently retains each complete native member and unedited record");
    const auto nextRevision = fixture.workspace->Revision(); const auto second = fixture.workspace->BeginEdit(nextRevision);
    Require(second.Succeeded(), "next original transaction reseeds all six members");
    const auto seeded = fixture.workspace->PreviewEdit(*second.value, nextRevision);
    Require(seeded.Succeeded() && seeded.value->changes.size() == 6, "reseed never silently drops the new sound member");
    Require(fixture.workspace->ApplyMapDataPlan(fixture.Plan(MapDataKind::Sounds, "volume", "81")).Succeeded(), "net 32-field budget still permits changing an existing difference");
    Require(fixture.workspace->Rollback(*second.value, nextRevision).Succeeded(), "original rollback restores latest six-member committed revision");
    const auto rolledBack = fixture.Context();
    for (const auto kind : kinds) Require(rolledBack.document->Member(kind).sources.front().data == current.document->Member(kind).sources.front().data,
        "rollback restores all committed original bytes, rather than initial source");
    const auto untouchedDirectory = DirectoryWorkspace::Open(fixture.root / "source"); Require(untouchedDirectory.Succeeded(), "original six-member source still opens");
    const auto untouched = MapDocument::FromDirectory(*untouchedDirectory.value); Require(untouched.Succeeded(), "original source independent parse");
    for (const auto kind : kinds) Require(untouched.value->Member(kind).sources.front().data == initial.document->Member(kind).sources.front().data,
        "candidate publishing/reseed/rollback never rewrites original source members");
}
}
int main() {
    try { CumulativeOverlayRollbackAndDirectorySeed(); SourceAndSealedCodecRefuseBeforePublish(); DSealedDirectorySaveReopenReseedAndRollback();
        FiveMemberReseedRefusalsCommitAndRollback(); SixMemberSoundsRetainCumulativeBudgetAndOriginalTransaction(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Actual map-data workspace overlay, prepublish refusal, directory reseed and source gates passed.\n";
    return 0;
}

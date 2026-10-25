#pragma once

#include "support/TerrainResizeFixture.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"
#include "War3MapCore/Services/TerrainShadowBake.h"
#include "War3MapCore/Services/MapSaveService.h"

namespace AudastraTest::TerrainResizeShadowAbsenceCases {
namespace R = TerrainResize;
using namespace War3Map;
using namespace War3Map::MapData;
using R::Bytes;
using R::Fixture;

inline bool IsShadow(std::string_view name) { return name == "war3map.shd"; }

inline void Prepare(Fixture& fixture) {
    const std::vector<MapArchiveMember> game{{"synthetic-marker.txt", R::Text("Audastra shadow absence fixture\n")}};
    R::Take(WriteVerifiedArchive(fixture.gameRoot / "war3.mpq", game));
    R::Check(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", R::Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=480\n"
        "[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n[Light]\nDirection=3,0,-4\n")));
    fixture.PutMember("war3mapImported/retained.shd", R::Text("unrelated authored shadow resource"));
    fixture.Seal();
}

inline void Resize(Fixture& fixture) {
    const auto resources = R::Take(Services::TerrainResizeEditService::Capture(
        *fixture.archive, *fixture.edits, fixture.gameRoot));
    const auto prepared = R::Take(fixture.edits->PrepareTerrainResizeEdit(resources, {1, {-32, 0, 32, 32}}));
    R::Check(fixture.edits->ApplyPreparedTerrainResizeEdit(prepared));
}

inline ArchiveExactMember Shadow(const MapArchiveSession& archive) {
    const std::array<std::string, 1> names{"war3map.shd"};
    return R::Take(archive.CaptureExactMembers(names)).front();
}

inline void Save(Fixture& fixture, std::string_view filename, const ArchiveExactMember& expected,
    bool expectedRemoval, bool bake = false) {
    auto output = fixture.edits->Replacements();
    const auto removals = fixture.edits->Removals();
    R::Require(std::count_if(removals.begin(), removals.end(), IsShadow) == (expectedRemoval ? 1 : 0),
        "only typed transition from saved present SHD to current absence emits a single removal");
    auto plan = R::Take(Services::TerrainPathingSave::PrepareForSave(
        *fixture.archive, *fixture.edits, fixture.gameRoot, {}, output, removals));
    R::Check(plan.SaveGate()); R::Check(plan.InstallPathingOutput(*fixture.edits, output));
    std::optional<MapDataEditSession::TerrainHistorySaveLease> history;
    std::shared_ptr<const Services::TerrainShadowBake> baked;
    if (bake) {
        history = R::Take(fixture.edits->PrepareTerrainHistoryForSave());
        baked = R::Take(Services::TerrainShadowBake::Capture(*fixture.archive, fixture.gameRoot, output, removals));
        R::Require(baked->Bytes() == expected.bytes, "flat synthetic terrain Bake has independently expected complete zero SHD");
        R::Check(baked->AdmitToPlan(plan, *fixture.edits));
        std::erase_if(output, [](const auto& member) { return IsShadow(member.name); });
        output.push_back({"war3map.shd", baked->Bytes()});
    }
    const auto& shadowPlan = plan.Members()[2];
    R::Require(shadowPlan.outputSha256.has_value() == expected.present,
        "frozen SHD output identity represents actual presence rather than an empty file");
    if (expectedRemoval) {
        R::Require(shadowPlan.action == TerrainSaveMemberAction::WriteEdited && !shadowPlan.outputBytes
            && shadowPlan.reasonCode == "TERRAIN_SAVE_REMOVE_RESIZE_SHADOW",
            "typed SHD deletion is explicit and has no pretend output bytes");
        R::Require(std::none_of(output.begin(), output.end(), [](const auto& member) { return IsShadow(member.name); }),
            "typed absence never installs an empty SHD replacement");
        R::Require(R::HasCode(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output),
            "TERRAIN_SAVE_PLAN_OUTPUT_MISSING"), "omitted SHD deletion is rejected before publish");
        auto duplicate = removals; duplicate.push_back("WAR3MAP.SHD");
        R::Require(R::HasCode(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output, duplicate),
            "TERRAIN_SAVE_PLAN_DUPLICATE"), "aliased duplicate SHD deletion is rejected");
        auto conflicting = output; conflicting.push_back({"war3map.shd", {}});
        R::Require(R::HasCode(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), conflicting, removals),
            "TERRAIN_SAVE_PLAN_DUPLICATE"), "SHD deletion cannot also write a zero-byte member");
        auto retained = R::Take(MapArchiveSession::Open(R::Take(fixture.archive->SaveCandidate(
            fixture.directory / "retained-shadow-must-fail-readback.w3x", output)).path));
        R::Require(!plan.VerifyReadback(retained).Succeeded(),
            "readback checks true member absence independently of the requested deletion list");
        R::Require(std::none_of(plan.Diagnostics().begin(), plan.Diagnostics().end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_SAVE_SHADOW_NOT_REBUILT";
        }), "typed deletion never also claims the SHD was preserved");
    }
    R::Check(plan.VerifySourceInputs(*fixture.archive));
    R::Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output, removals));
    auto saved = R::Take(MapArchiveSession::Open(R::Take(fixture.archive->SaveCandidate(
        fixture.directory / std::filesystem::path(filename), output, removals)).path));
    R::Check(plan.VerifyReadback(saved));
    const auto actual = Shadow(saved);
    R::Require(actual.present == expected.present && actual.bytes == expected.bytes,
        "actual archive preserves exact expected SHD presence and every output byte");
    R::Require(R::Take(saved.ReadMember("war3mapImported/retained.shd")) == R::Text("unrelated authored shadow resource"),
        "typed SHD removal never deletes unrelated authored shadow resources");
    if (baked) R::Check(baked->VerifyReadback(saved));
    R::Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(saved)));
    if (history) {
        R::Require(R::HasCode(fixture.edits->CommitTerrainHistorySave(*history), "TERRAIN_HISTORY_EPOCH_LIMIT"),
            "one-off Bake that changed presence does not install fictitious cold typed history");
        R::Require(!fixture.edits->ExportTerrainHistoryJournal().Succeeded(),
            "optional cold history remains unavailable when Bake is not a typed command");
    }
    const auto effective = R::Take(fixture.edits->TerrainResizeShadowDraft());
    R::Require(!fixture.edits->IsDirty() && effective.present == actual.present && effective.bytes == actual.bytes,
        "successful save adopts exactly the actual written SHD presence and bytes");
    fixture.archive = std::make_unique<MapArchiveSession>(std::move(saved));
}

inline void BakedMissingShadowCanUndoSaveAndRedoSave() {
    Fixture fixture(false, false); Prepare(fixture); Resize(fixture);
    Save(fixture, "absent-source-baked.w3x", {"war3map.shd", true, Bytes(256u * 128u)}, false, true);
    R::Require(fixture.edits->Undo(), "resize can Undo after actual Bake save");
    const auto restored = R::Take(fixture.edits->TerrainResizeShadowDraft());
    R::Require(!restored.present && restored.bytes.empty(), "hot Undo restores exact original SHD absence");
    {
        const auto known = fixture.archive->Members();
        std::vector<MapArchiveMember> members(known.begin(), known.end());
        const auto shadow = std::find_if(members.begin(), members.end(), [](const auto& member) { return IsShadow(member.name); });
        R::Require(shadow != members.end() && !shadow->data.empty(), "actual Bake member is available for source-change rejection");
        shadow->data.front() = std::byte{1};
        auto changed = R::Take(MapArchiveSession::Open(R::Take(WriteVerifiedArchive(
            fixture.directory / "changed-source-shadow.w3x", members)).path));
        const auto rejected = Services::TerrainPathingSave::PrepareForSave(changed, *fixture.edits, fixture.gameRoot, {},
            fixture.edits->Replacements(), fixture.edits->Removals());
        R::Require(!rejected.Succeeded() && R::HasCode({rejected.diagnostics}, "TERRAIN_RESIZE_SHADOW_SOURCE"),
            "target absence never bypasses complete saved physical SHD identity");
    }
    {
        const auto original = MapDocument::FromArchive(*fixture.archive);
        MapDataEditSession raw(original);
        R::Check(raw.AttachTerrain(R::Take(fixture.archive->ReadMember("war3map.w3e"))));
        R::Check(raw.AttachPathing(R::Take(fixture.archive->ReadMember("war3map.wpm"))));
        const auto terrain = R::Take(SerializeMapTerrain(*fixture.edits->Terrain()));
        const auto replacements = fixture.edits->Replacements();
        const auto wpm = std::find_if(replacements.begin(), replacements.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
        R::Require(wpm != replacements.end(), "Undo after resized save carries a complete original-shape WPM");
        R::Check(raw.SetTerrainResizeDraft(terrain, wpm->data, R::Take(fixture.edits->ExportTerrainResizeDraft())));
        R::Check(raw.SetSerialized(MapDataKind::Settings, fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data));
        const auto rawRemovals = raw.Removals();
        R::Require(std::none_of(rawRemovals.begin(), rawRemovals.end(), IsShadow), "TRD1 absence does not create SHD deletion authority");
        R::Require(!raw.VerifyTerrainPathingForSave().Succeeded(), "raw SHD absence still cannot pass the native save gate");
    }
    Save(fixture, "absent-source-undo-saved.w3x", {"war3map.shd", false, {}}, true);
    R::Require(fixture.edits->Redo(), "resize can Redo after Undo was actually saved");
    // Bake是单次保存派生，不是原typed命令。再次保存后Redo恢复原未Bake结果，
    // 不伪造该次显式计算已成为可恢复的命令或冷历史。
    const auto redone = R::Take(fixture.edits->TerrainResizeShadowDraft());
    R::Require(!redone.present && redone.bytes.empty(), "Redo restores the original unbaked resize result after the second save");
    Save(fixture, "absent-source-redo-saved.w3x", {"war3map.shd", false, {}}, false);
}

inline void PresentShadowPreservesHotRawAndNormalizesOnlyWrittenBytes() {
    Fixture fixture; Prepare(fixture); Resize(fixture);
    Save(fixture, "present-source-resized.w3x", {"war3map.shd", true, R::ShadowLiteral(-32, 0, 32, 32, true)}, false);
    R::Require(fixture.edits->Undo(), "present-SHD resize can Undo after actual save");
    R::Require(R::Take(fixture.edits->TerrainResizeShadowDraft()).bytes == R::ShadowLiteral(),
        "hot Undo preserves every original noncanonical SHD byte including 02 and 80");
    Save(fixture, "present-source-undo-saved.w3x", {"war3map.shd", true, R::ShadowLiteral(0, 0, 32, 32, true)}, false);
    R::Require(fixture.edits->Redo(), "present-SHD resize can Redo after Undo save");
    Save(fixture, "present-source-redo-saved.w3x", {"war3map.shd", true, R::ShadowLiteral(-32, 0, 32, 32, true)}, false);
}

inline void BakedMissingShadowCanUndoAndBakeAgainSave() {
    Fixture fixture(false, false); Prepare(fixture); Resize(fixture);
    Save(fixture, "before-undo-and-rebake.w3x", {"war3map.shd", true, Bytes(256u * 128u)}, false, true);
    R::Require(fixture.edits->Undo(), "Undo before explicitly baking the restored terrain again");
    auto output = fixture.edits->Replacements();
    const auto removed = fixture.edits->Removals();
    auto base = R::Take(Services::TerrainPathingSave::PrepareForSave(
        *fixture.archive, *fixture.edits, fixture.gameRoot, {}, output, removed));
    R::Check(base.SaveGate()); R::Check(base.InstallPathingOutput(*fixture.edits, output));
    auto baseline = std::make_shared<const TerrainSavePlan>(std::move(base));
    R::Require(baseline->Members()[2].reasonCode == "TERRAIN_SAVE_REMOVE_RESIZE_SHADOW",
        "rebake starts from the exact typed Undo deletion plan");
    const auto freeze = [&](Services::MapSaveTask& task) {
        R::Check(task.FreezeArchive(*fixture.archive));
        task.replacements = output; task.removals = removed;
        task.target = fixture.directory / "undo-then-explicitly-rebaked.w3x";
    };
    const auto otherSession = fixture.FreshSession();
    for (unsigned fault = 0; fault < 5; ++fault) {
        Services::MapSaveTask denied; freeze(denied);
        auto plan = baseline;
        if (fault == 0) denied.removals.push_back("WAR3MAP.SHD");
        if (fault == 1) denied.replacements.push_back({"war3map.shd", {}});
        if (fault == 2) plan.reset();
        const auto originalPlan = plan;
        const auto* edits = fault == 3 ? otherSession.get() : fixture.edits.get();
        const auto status = Services::PrepareTerrainShadowBake(denied,
            fault == 4 ? fixture.gameRoot / "unavailable-profile" : fixture.gameRoot, edits, plan);
        R::Require(!status.Succeeded(), "duplicate/conflicting/unbound/foreign/failed Bake cannot replace typed deletion");
        if (fault != 4) R::Require(R::HasCode(status, "TERRAIN_SHADOW_OUTPUT_CONFLICT"),
            "only a unique bound typed SHD deletion can be superseded by an explicit Bake");
        R::Require(!denied.terrainShadow && plan == originalPlan
            && denied.removals == (fault == 0 ? std::vector<std::string>{"war3map.shd", "WAR3MAP.SHD"} : removed)
            && denied.replacements.size() == output.size() + (fault == 1 ? 1 : 0)
            && !std::filesystem::exists(denied.target), "failed Bake leaves the original plan and deletion task intact");
    }
    Services::MapSaveTask task; freeze(task); auto plan = baseline;
    task.prepareFinalDerived = [&](Services::MapSaveTask& current) {
        return Services::PrepareTerrainShadowBake(current, fixture.gameRoot, fixture.edits.get(), plan);
    };
    task.beforePublish = [&] {
        return plan->VerifyForSave(*fixture.edits, task.sourceSha256, task.replacements, task.removals);
    };
    Services::MapSaveService::Run(task);
    if (!task.succeeded) R::Check({task.diagnostics});
    R::Require(task.succeeded && task.nextSession && task.terrainShadow && plan != baseline,
        "same ordinary save service publishes the complete explicitly rebaked Undo result");
    R::Require(std::none_of(task.removals.begin(), task.removals.end(), IsShadow)
        && plan->Members()[2].reasonCode == "TERRAIN_SAVE_WRITE_REBUILT_SHADOW"
        && baseline->Members()[2].reasonCode == "TERRAIN_SAVE_REMOVE_RESIZE_SHADOW",
        "successful Bake alone replaces typed deletion, without mutating the caller's old plan");
    R::Check(plan->VerifyReadback(*task.nextSession));
    R::Check(task.terrainShadow->VerifyReadback(*task.nextSession));
    const auto actual = Shadow(*task.nextSession);
    R::Require(actual.present && actual.bytes == Bytes(128u * 128u),
        "actual archive contains the complete independently expected flat-terrain SHD at the restored dimensions");
    R::Require(R::Take(task.nextSession->ReadMember("war3mapImported/retained.shd"))
        == R::Text("unrelated authored shadow resource"), "rebake preserves unrelated imported SHD data");
    R::Check(plan->CommitSaved(*fixture.edits, MapDocument::FromArchive(*task.nextSession)));
    R::Require(!fixture.edits->IsDirty() && R::Take(fixture.edits->TerrainResizeShadowDraft()).bytes == actual.bytes,
        "successful explicit rebake adopts the actual saved SHD baseline");
}

inline void OrdinaryShadowRemovalRemainsRefused() {
    Fixture fixture;
    R::Require(fixture.edits->Removals().empty(), "ordinary source has no implicit SHD removal");
    const auto plan = R::Take(TerrainSavePlan::Prepare(*fixture.archive, *fixture.edits));
    R::Check(plan.SaveGate());
    const std::array<std::string, 1> removals{"war3map.shd"};
    R::Require(R::HasCode(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), {}, removals),
        "TERRAIN_SAVE_PLAN_REMOVAL"), "manual SHD deletion without typed absence remains refused");
    Services::MapSaveTask task; R::Check(task.FreezeArchive(*fixture.archive));
    task.removals.assign(removals.begin(), removals.end());
    auto bakePlan = std::make_shared<const TerrainSavePlan>(plan);
    R::Require(R::HasCode(Services::PrepareTerrainShadowBake(task, fixture.gameRoot, fixture.edits.get(), bakePlan),
        "TERRAIN_SHADOW_OUTPUT_CONFLICT"), "explicit Bake cannot grant manual removal the typed Undo exception");
}

inline void TestTerrainResizeShadowAbsenceCases() {
    BakedMissingShadowCanUndoSaveAndRedoSave();
    PresentShadowPreservesHotRawAndNormalizesOnlyWrittenBytes();
    BakedMissingShadowCanUndoAndBakeAgainSave();
    OrdinaryShadowRemovalRemainsRefused();
}
} // namespace AudastraTest::TerrainResizeShadowAbsenceCases

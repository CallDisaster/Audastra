#include "EditorProject.h"
#include "PreparedAssetState.h"
#include "PendingImportPayload.h"
#include "MapSaveJob.h"
#include "Commands/TriggerCommands.h"
#include "War3MapCore/Services/ImportPayloadPolicy.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "../AuxiliarySaveFixtures.h"
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace War3Map;
using namespace War3Map::QtProject;
namespace {
struct Fixture {
    QTemporaryDir temporary;
    QUndoStack triggers;
    AuxiliarySaveFixtures::OriginalInputs inputs;
    std::unique_ptr<EditorProject> project;
    std::string sourceSha;
    Fixture() {
        using namespace AuxiliarySaveFixtures;
        Require(temporary.isValid(), "owned temporary fixture");
        inputs = CreateOriginalInputs(std::filesystem::path(temporary.path().toStdWString()));
        auto table = Resources::ImportTable::CreateEmpty(); Require(table.Upsert("unchanged.bin").Succeeded(), "original import record");
        auto members = inputs.members; members.push_back({"war3map.imp", Take(table.Serialize())});
        inputs.source = inputs.root / "import-source.w3x"; sourceSha = Take(WriteVerifiedArchive(inputs.source, members)).sha256;
        project = std::make_unique<EditorProject>();
        project->sourceArchivePath_ = inputs.source; project->projectRoot_ = inputs.root;
        project->archiveSession_ = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(inputs.source)));
        project->LoadMapDocument();
        project->uiCatalog_ = std::make_unique<Trigger::UiCatalog>(Take(Trigger::UiCatalog::Load(inputs.ui)));
        project->triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(Take(Trigger::WtgWctCodec{}.Parse(inputs.pair.wtg, inputs.pair.wct, *project->uiCatalog_)));
        project->uiDataRoot_ = project->activeUiDataRoot_ = inputs.ui; project->compilerDataRoot_ = inputs.tool; project->war3GameRoot_ = inputs.game;
        project->triggerUndoStack_ = &triggers; project->ResetEditTracking();
        Require(project->editTrackingReady_ && !project->editTrackingFailed_, "fixture revision initialized");
    }
    ~Fixture() { if (project) project->triggerUndoStack_ = nullptr; }
};
std::shared_ptr<std::vector<std::byte>> Binary(std::size_t count) {
    auto bytes = std::make_shared<std::vector<std::byte>>(count); std::uint32_t seed = 0x47a9b31c;
    for (auto& byte : *bytes) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; byte = static_cast<std::byte>(seed & 255); }
    return bytes;
}
std::string Problems(const std::vector<Diagnostic>& diagnostics) {
    std::string text; for (const auto& row : diagnostics) text += row.code + ":" + row.message + '\n'; return text;
}
}
class ImportAssetTransactionTests final : public QObject {
    Q_OBJECT
private slots:
    void FourMiBReplaceEditSaveUndoRedo() {
        using namespace AuxiliarySaveFixtures;
        Fixture fixture; auto& project = *fixture.project;
        auto mutableBytes = Binary(4 * 1024 * 1024 + 17); const auto expectedSha = Take(Sha256(*mutableBytes));
        std::vector<EditorProject::ImportPayload> payloads{{"unchanged.bin", mutableBytes}};
        const auto oldRevision = project.editRevision_.Revision(); bool observerSawPair = false;
        QObject::connect(&project, &EditorProject::ImportsChanged, &project, [&] {
            const auto scopes = project.CaptureEditScopes();
            observerSawPair = project.editRevision_.Revision() > oldRevision && scopes && *scopes == project.editRevision_.Current();
        });
        const auto problem = project.AddImportPayloads(payloads, true, "owned binary replacement");
        QVERIFY2(problem.isEmpty(), qPrintable(problem)); QVERIFY(payloads.front().replaced); QVERIFY(observerSawPair);
        QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_); QCOMPARE(project.AssetUndoStack()->count(), 1);
        (*mutableBytes)[0] ^= std::byte{1};
        QCOMPARE(Take(FingerprintPendingImport(project.pendingImports_.front())), expectedSha);
        const auto trigger = project.triggerDocument_->triggers.front().id;
        auto edited = PublishTriggerGraphOperations(project, {Trigger::GraphReplaceTriggerText{{Trigger::GraphRecordKind::Trigger, trigger, {}},
            "actions:\n    S6TemplateMarker(\"Edited owned T\")\n"}}, "edit owned T after binary import");
        QVERIFY2(edited.Succeeded(), Problems(edited.diagnostics).c_str()); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
        const auto verify = [&](const std::filesystem::path& path) {
            auto reopened = Take(MapArchiveSession::Open(path));
            Require(Take(Sha256(Take(reopened.ReadMember("unchanged.bin")))) == expectedSha, "saved exact binary SHA");
            const auto pair = Take(Trigger::WtgWctCodec{}.Parse(Take(reopened.ReadMember("war3map.wtg")), Take(reopened.ReadMember("war3map.wct")), *project.uiCatalog_));
            Require(Take(Trigger::RenderTriggerText(pair, *project.uiCatalog_, pair.triggers.front().id)).find("Edited owned T") != std::string::npos, "saved editable T");
            const auto script = Take(reopened.ReadMember("war3map.j"));
            Require(std::string(reinterpret_cast<const char*>(script.data()), script.size()).find("Edited owned T") != std::string::npos, "production rebuilt script carries T change");
        };
        const auto first = fixture.inputs.root / "test-candidate.w3x";
        auto testCopy = project.PrepareMapTestCopy(first); QVERIFY(testCopy);
        EditorProject::RunMapSave(*testCopy); QVERIFY2(testCopy->succeeded, Problems(testCopy->diagnostics).c_str()); verify(first);
        // 验收副本走同一保存执行链，保持当前源和收据；正式另存的基线提交在最后验证。
        fixture.triggers.undo(); project.AssetUndoStack()->undo(); QVERIFY(project.pendingImports_.empty());
        QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
        project.AssetUndoStack()->redo(); fixture.triggers.redo();
        QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_); QCOMPARE(Take(FingerprintPendingImport(project.pendingImports_.front())), expectedSha);
        const auto second = fixture.inputs.root / "normal-candidate.w3x";
        auto normal = project.PrepareMapSave(second, false); QVERIFY(normal);
        EditorProject::RunMapSave(*normal); QVERIFY2(normal->succeeded, Problems(normal->diagnostics).c_str()); verify(second);
        QVERIFY(project.CommitMapSave(*normal).has_value());
        QCOMPARE(project.sourceArchivePath_, second); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
        QCOMPARE(Take(Sha256File(fixture.inputs.source)), fixture.sourceSha);
    }
    void FailedPreparationAndStaleProofKeepStateAndHistory() {
        Fixture fixture; auto& project = *fixture.project;
        const auto before = project.CaptureAssets(); const auto scopes = project.editRevision_.Current(); const auto revision = project.editRevision_.Revision();
        auto candidate = before;
        PendingImport invalid{"broken.bin", {}, 1}; invalid.ownedPayload = Binary(16); candidate.imports.push_back(invalid);
        const auto bad = project.PrepareAssetStateEdit(candidate, project.editRevision_.Generation(), revision, "invalid candidate");
        QVERIFY(!bad.Succeeded()); QVERIFY(project.CaptureAssets() == before); QCOMPARE(project.editRevision_.Current(), scopes);
        QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
        QVERIFY(!project.RecordAssetEdit("cancelled", [](AssetState&) { return QStringLiteral("cancelled"); }).isEmpty());
        QCOMPARE(project.AssetUndoStack()->count(), 0); QCOMPARE(project.editRevision_.Revision(), revision);
        invalid.size = 16; candidate.imports = {invalid};
        auto prepared = project.PrepareAssetStateEdit(candidate, project.editRevision_.Generation(), revision, "prepared candidate"); QVERIFY(prepared.Succeeded());
        std::vector<EditorProject::ImportPayload> later{{"later.bin", Binary(32)}};
        QVERIFY(project.AddImportPayloads(later, false, "intervening edit").isEmpty());
        const auto current = project.CaptureAssets(); const auto currentScopes = project.editRevision_.Current();
        QVERIFY(!project.PublishPreparedAssetState(**prepared.value).Succeeded());
        QVERIFY(project.CaptureAssets() == current); QCOMPARE(project.editRevision_.Current(), currentScopes);
        QVERIFY(!project.PrepareAssetStateEdit(candidate, project.editRevision_.Generation(), revision, "old revision").Succeeded());
        invalid.runtimeLuaDraft = true; invalid.size = 4 * 1024 * 1024; invalid.ownedPayload = Binary(static_cast<std::size_t>(invalid.size));
        QVERIFY(!FingerprintPendingImport(invalid).Succeeded());
        QVERIFY(!Services::ImportPayloadPolicy::Check(Services::ImportPayloadRole::OwnedResource, 64ull * 1024 * 1024 + 1).Succeeded());
        QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    }
};
QTEST_MAIN(ImportAssetTransactionTests)
#include "ImportAssetTransactionTests.moc"

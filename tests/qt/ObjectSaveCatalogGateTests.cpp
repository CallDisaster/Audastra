#include "EditorProject.h"
#include "MapSaveJob.h"
#include "ProjectObjectCatalog.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "../MapDataFixtures.h"
#include "../services/ObjectCommandsFixture.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool HasCode(const auto& status, std::string_view code) {
    return std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& value) { return value.code == code; });
}
bool ProjectHasCode(const QtProject::EditorProject& project, QStringView code) {
    for (int row = 0; row < project.diagnosticsModel_->rowCount(); ++row)
        if (project.diagnosticsModel_->index(row, 2).data().toString() == code) return true;
    return false;
}
struct Fixture {
    QTemporaryDir temporary;
    std::filesystem::path source, game;
    std::unique_ptr<QtProject::EditorProject> project;
    std::string originalSha;
    Fixture() {
        Require(temporary.isValid(), "create unique original fixture");
        const auto root = std::filesystem::path(temporary.path().toStdWString());
        source = root / "source.w3x"; game = root / "game";
        std::filesystem::create_directory(game);
        const auto data = Tests::ObjectTable({
            {{"ID", "u001"}, {"Name", "Game base"}, {"HP", "100"}},
            {{"ID", "u002"}, {"Name", "Other game base"}, {"HP", "150"}}});
        const std::vector<Mpq::MpqBuildFile> gameFiles{{"Units/UnitData.slk", Tests::ObjectBytes(data), false}};
        Require(Mpq::WriteMpqArchive(game / "war3.mpq", gameFiles).Succeeded(), "write synthetic game table");
        auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit, 2);
        Require(native.Succeeded() && native.value->AddOriginalObject("u001").Succeeded(), "create owned native baseline");
        Require(native.value->AddModification({ObjectType::Unit, "u001"}, {"uhpm", {}, {}, {}},
            NativeValueType::Int32, std::int32_t{100}).Succeeded(), "populate original native health");
        const auto encoded = NativeObjectCodec(ObjectType::Unit).Serialize(*native.value);
        Require(encoded.Succeeded(), "serialize original native baseline");
        const std::vector<MapArchiveMember> members{
            {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3map.w3u", *encoded.value},
            {"war3map.j", Tests::ObjectBytes("function main takes nothing returns nothing\nendfunction\n")}};
        Require(WriteVerifiedArchive(source, members).Succeeded(), "create own source map");
        auto archive = MapArchiveSession::Open(source);
        Require(archive.Succeeded(), "capture own source map");
        originalSha = archive.value->SourceSha256();
        project = std::make_unique<QtProject::EditorProject>();
        project->archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
        project->sourceArchivePath_ = source; project->projectRoot_ = root; project->war3GameRoot_ = game;
        project->nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
        const auto text = ConvertNativeObjectDataToLni(ObjectType::Unit, *encoded.value);
        Require(text.Succeeded(), "project original native into editable LNI");
        auto parsed = LniObjectCodec(ObjectType::Unit).Parse(*text.value);
        const auto* editable = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
        Require(editable, "create original editable document");
        project->objectDocuments_[0] = std::make_unique<LniObjectDocument>(*editable);
        Tests::ObjectCommandsFixture metadata;
        project->objectFieldSchema_ = std::make_unique<ObjectFieldSchema>(std::move(metadata.schema));
        project->LoadMapDocument();
        Require(QtProject::EnsureProjectObjectCatalog(*project).Succeeded(), "capture actual game source before editing");
        project->ResetEditTracking();
        Require(project->ObserveEditState("original object-save fixture baseline"),
            "initialize a real save revision baseline before testing the object catalog gate");
    }
    void Dirty() {
        Require(project->objectDocuments_[0]->SetModification({ObjectType::Unit, "u001"}, {"uhpm", {}, {}, {}},
            std::int32_t{250}).Succeeded(), "make original dirty edit before changing source eligibility");
        project->objectDirty_[0] = true;
        project->ObserveObjectEdit(0);
        Require(project->objectDirty_[0] && !project->editTrackingFailed_,
            "dirty object edit is tracked before testing catalog write authority");
    }
    void ImportReadOnlyProfile(bool notify) {
        auto state = project->CaptureAssets();
        QtProject::PendingImport profile;
        profile.archivePath = "Units/HumanUnitStrings.txt";
        profile.ownedPayload = std::make_shared<const std::vector<std::byte>>(Tests::ObjectBytes("[u001]\nName=Map base\n"));
        profile.size = static_cast<qint64>(profile.ownedPayload->size());
        state.imports.push_back(std::move(profile));
        if (notify) project->RestoreAssets(state);
        else project->pendingImports_ = std::move(state.imports); // 恢复工程时已存在的owned状态；不依赖UI操作门。
    }
    std::unique_ptr<QtProject::MapSaveJob> Prepare() {
        return project->PrepareMapSave(source.parent_path() / "candidate.w3x", false);
    }
    void Unchanged() const {
        const auto current = Sha256File(source);
        Require(current.Succeeded() && *current.value == originalSha, "rejected save preparation preserves source bytes");
        Require(!std::filesystem::exists(source.parent_path() / "candidate.w3x"), "preparation never publishes a candidate");
    }
};

void DirtyGameEditsCannotAcquireReadOnlyMapWriteback() {
    for (const bool notifiedImport : {true, false}) {
        Fixture fixture; fixture.Dirty(); fixture.ImportReadOnlyProfile(notifiedImport);
        const auto job = fixture.Prepare();
        Require(!job && ProjectHasCode(*fixture.project, u"OBJECT_CATALOG_READ_ONLY"),
            "existing dirty edits and restored pending state are rejected by the save gate itself");
        Require(fixture.project->objectDirty_[0] && fixture.project->objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}),
            "rejection retains dirty editable state");
        fixture.Unchanged();
    }
}

void FrozenTaskOwnsSourcesAndChecksFinalTableChanges() {
    Fixture fixture; fixture.Dirty(); auto job = fixture.Prepare();
    Require(job && job->beforePublish, "ordinary game-based edit freezes an owned publication gate");
    fixture.project.reset();
    Require(job->beforePublish().Succeeded(), "frozen gate survives destruction of the originating project");
    job->replacements.push_back({"Units/UnitData.slk", Tests::ObjectBytes(Tests::ObjectTable({
        {{"ID", "u001"}, {"Name", "Template changed the base"}, {"HP", "100"}}}))});
    const auto shadowed = job->beforePublish();
    Require(!shadowed.Succeeded() && HasCode(shadowed, "OBJECT_CATALOG_READ_ONLY"),
        "final template-style map table cannot grant native writeback to the frozen dirty edit");
    job->replacements.pop_back();
    Require(job->beforePublish().Succeeded(), "removing only the rejected synthetic change restores original eligibility");
    fixture.Unchanged();
}

void FrozenTaskChecksEveryChangedNativeObject() {
    Fixture fixture; fixture.Dirty(); auto job = fixture.Prepare();
    Require(job && job->beforePublish, "freeze original game-based dirty edit");
    fixture.project.reset();
    job->replacements.push_back({"Units/HumanUnitStrings.txt", Tests::ObjectBytes("[u002]\nName=Map base\n")});
    Require(job->beforePublish().Succeeded(), "unrelated read-only base does not revoke the allowed u001 edit");
    const auto replacement = std::find_if(job->replacements.begin(), job->replacements.end(), [](const auto& input) {
        return input.name == NativeFileName(ObjectType::Unit);
    });
    Require(replacement != job->replacements.end(), "prepared task has one final native unit member");
    const auto allowedBytes = replacement->data;
    auto parsed = NativeObjectCodec(ObjectType::Unit).Parse(replacement->data);
    auto* native = parsed.Succeeded() ? dynamic_cast<NativeObjectDocument*>(parsed.document.get()) : nullptr;
    Require(native && native->AddOriginalObject("u002").Succeeded(), "append another original object's native override");
    Require(native->AddModification({ObjectType::Unit, "u002"}, {"uhpm", {}, {}, {}},
        NativeValueType::Int32, std::int32_t{200}).Succeeded(), "change the other read-only base after preparation");
    const auto encoded = NativeObjectCodec(ObjectType::Unit).Serialize(*native);
    Require(encoded.Succeeded(), "serialize the final template-style native change");
    replacement->data = *encoded.value;
    const auto rejected = job->beforePublish();
    Require(!rejected.Succeeded() && HasCode(rejected, "OBJECT_CATALOG_READ_ONLY"),
        "publication checks all changed native objects, including non-subject read-only objects");
    replacement->data = allowedBytes;
    Require(job->beforePublish().Succeeded(), "restoring only the extra native change restores eligibility");
    fixture.Unchanged();
}

void FrozenTaskRejectsSameBytesAtChangedGameSourceIdentity() {
    Fixture fixture; fixture.Dirty(); auto job = fixture.Prepare();
    Require(job && job->beforePublish, "freeze owned gate before external source replacement");
    fixture.project.reset();
    const auto old = fixture.game / "old-war3.mpq";
    std::filesystem::rename(fixture.game / "war3.mpq", old);
    std::filesystem::copy_file(old, fixture.game / "war3.mpq");
    Require(!job->beforePublish().Succeeded(), "same bytes at a new game file identity revoke frozen write authority");
    fixture.Unchanged();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir settings;
        Require(settings.isValid(), "isolate the original fixture settings");
        QCoreApplication::setOrganizationName("Audastra-ObjectSaveCatalogGate-Tests");
        QCoreApplication::setApplicationName("ObjectSaveCatalogGate");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
        qunsetenv("FORGE_SCRIPT_SOURCE_ENCODINGS_FILE"); // 仅清除此独立测试进程的继承设置。
        DirtyGameEditsCannotAcquireReadOnlyMapWriteback();
        FrozenTaskOwnsSourcesAndChecksFinalTableChanges();
        FrozenTaskChecksEveryChangedNativeObject();
        FrozenTaskRejectsSameBytesAtChangedGameSourceIdentity();
        std::cout << "object save catalog gates passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

#include "ObjectContextProvider.h"
#include "EditorProject.h"
#include "ProjectObjectCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"
#include "../services/ObjectCommandsFixture.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>

namespace {
using namespace War3Map;
using ObjectData::ObjectType;
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void Install(QtProject::EditorProject& project, const QTemporaryDir& directory) {
    Tests::ObjectCommandsFixture fixture;
    project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(fixture.schema));
    project.projectRoot_ = std::filesystem::path(directory.path().toStdWString());
    Require(QDir(directory.path()).mkpath("Units"), "create owned table folder");
    QFile table(directory.filePath("Units/UnitData.slk"));
    Require(table.open(QIODevice::WriteOnly), "open owned unit table");
    const auto body = Tests::ObjectTable({{{"ID", "u001"}, {"Name", "Original"}, {"HP", "100"}}});
    Require(table.write(body.data(), static_cast<qint64>(body.size())) == static_cast<qint64>(body.size()), "write owned unit table");
    table.close();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_);
    Require(opened.Succeeded(), "synthetic directory source");
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    Require(QtProject::EnsureProjectObjectCatalog(project).Succeeded(), "capture actual synthetic object source");
}
void EmptyAndLifetime() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary source");
    auto project = std::make_unique<QtProject::EditorProject>(); Install(*project, directory);
    auto provider = QtProject::MakeObjectContextProvider(*project);
    const auto context = provider(ObjectType::Unit);
    Require(context.Succeeded() && context.value->document && context.value->document->Objects().empty(), "known absent native/LNI creates borrowed empty view");
    Require(Services::ReadObjectFields(*context.value, "u001").Succeeded(), "standard type resolves through empty view");
    Require(!project->objectDocuments_[0], "query never mutates the project");
    const auto firstCatalog = project->standardCatalog_;
    Require(project->editRevision_.Observe({{"trigger", "changed"}}, "unrelated trigger").Succeeded(), "unrelated trigger edit");
    Require(QtProject::EnsureProjectObjectCatalog(*project).Succeeded() && project->standardCatalog_ == firstCatalog,
        "unrelated trigger edits reuse the captured table dependencies");
    Require(project->editRevision_.Reset().Succeeded(), "switch map generation");
    Require(!provider(ObjectType::Unit).Succeeded(), "provider refuses previous map generation");
    provider = QtProject::MakeObjectContextProvider(*project);
    Require(provider(ObjectType::Unit).Succeeded(), "fresh provider uses current generation");
    project.reset(); Require(!provider(ObjectType::Unit).Succeeded(), "closed project invalidates borrowed contexts");
}
void SourceAndNames() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary source");
    QtProject::EditorProject project; Install(project, directory);
    QFile file(directory.filePath("unit.ini")); Require(file.open(QIODevice::WriteOnly), "native source fixture");
    file.write("[U001]\n_parent=\"u001\"\n"); file.close();
    Require(!QtProject::MakeObjectContextProvider(project)(ObjectType::Unit).Succeeded(), "existing unloaded object file never replaced with empty table");
    Require(file.remove(), "remove own fixture");
    const std::vector<MapArchiveMember> members{{"war3map.wts", Tests::ObjectBytes("STRING 1\n{\n名称\n}\n")}};
    const auto source = MapDocument::FromMembers(members);
    project.mapDocument_ = std::make_unique<MapDocument>(source);
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(source);
    auto provider = QtProject::MakeObjectContextProvider(project);
    const auto context = provider(ObjectType::Unit);
    Require(context.Succeeded() && context.value->nameMap && context.value->nameMap->Strings(), "WTS borrowed from current map edit session");
    Require(context.value->nameMap->Member(MapDataKind::Units).state == MapDataState::Missing, "name context copies only WTS");
    Require(project.editRevision_.Observe({{"scope", "new"}}, "change").Succeeded(), "advance edit revision");
    Require(!provider(ObjectType::Unit).Succeeded(), "old context cannot cross a revision");
    project.standardCatalog_.reset();
    Require(!QtProject::MakeObjectContextProvider(project)(ObjectType::Unit).Succeeded(), "unknown standard source rejected");
}
void ImportsSwitchAndSourceIdentity() {
    QTemporaryDir first, second;
    Require(first.isValid() && second.isValid(), "owned map directories");
    QtProject::EditorProject project; Install(project, first);
    const auto original = project.standardCatalog_;
    const auto before = project.CaptureAssets();
    auto imported = before;
    QtProject::PendingImport profile;
    profile.archivePath = "Units/HumanUnitStrings.txt";
    profile.ownedPayload = std::make_shared<const std::vector<std::byte>>(Tests::ObjectBytes("[u001]\nName=Imported overlay\n"));
    profile.size = static_cast<qint64>(profile.ownedPayload->size());
    imported.imports.push_back(profile);
    project.RestoreAssets(imported);
    Require(project.standardCatalog_ && project.standardCatalog_->Find(ObjectType::Unit, "u001")->displayName == "Imported overlay",
        "owned pending profile immediately rebinds catalog");
    Require(!project.standardCatalog_->Find(ObjectType::Unit, "u001")->nativeOverridesSupported,
        "session profile contribution remains read only");
    Require(QtProject::VerifyProjectObjectCatalog(project).Succeeded(), "current owned overlay revalidates");
    project.RestoreAssets(before);
    Require(project.standardCatalog_ && project.standardCatalog_->Find(ObjectType::Unit, "u001")->displayName == "Original",
        "undo restores effective file selection");
    Require(original->Find(ObjectType::Unit, "u001")->displayName == "Original", "previous immutable catalog owner stays valid");
    Install(project, second);
    Require(project.standardCatalog_ && project.standardCatalog_ != original, "map switch publishes a new aliasing catalog owner");
    const QString tablePath = second.filePath("Units/UnitData.slk");
    Require(QFile::rename(tablePath, tablePath + ".old"), "replace fixture path while old lease lives");
    QFile replacement(tablePath); Require(replacement.open(QIODevice::WriteOnly), "new source identity");
    const auto body = Tests::ObjectTable({{{"ID", "u001"}, {"Name", "Original"}, {"HP", "100"}}});
    replacement.write(body.data(), static_cast<qint64>(body.size())); replacement.close();
    Require(!QtProject::VerifyProjectObjectCatalog(project).Succeeded(), "same bytes at a new file identity revoke write source");
    Require(!QtProject::EnsureProjectObjectCatalog(project).Succeeded() && !project.standardCatalog_, "invalid source clears old catalog");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try { EmptyAndLifetime(); SourceAndNames(); ImportsSwitchAndSourceIdentity(); std::cout << "ObjectContextProvider tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

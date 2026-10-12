#include "ObjectContextProvider.h"
#include "EditorProject.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"
#include "../services/ObjectCommandsFixture.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

namespace {
using namespace War3Map;
using ObjectData::ObjectType;
void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void Install(QtProject::EditorProject& project, const QTemporaryDir& directory) {
    Tests::ObjectCommandsFixture fixture;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(fixture.standard));
    project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(fixture.schema));
    project.projectRoot_ = std::filesystem::path(directory.path().toStdWString());
    auto opened = DirectoryWorkspace::Open(project.projectRoot_);
    Require(opened.Succeeded(), "synthetic directory source");
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
}
void EmptyAndLifetime() {
    QTemporaryDir directory; Require(directory.isValid(), "temporary source");
    auto project = std::make_unique<QtProject::EditorProject>(); Install(*project, directory);
    auto provider = QtProject::MakeObjectContextProvider(*project);
    const auto context = provider(ObjectType::Unit);
    Require(context.Succeeded() && context.value->document && context.value->document->Objects().empty(), "known absent native/LNI creates borrowed empty view");
    Require(Services::ReadObjectFields(*context.value, "u001").Succeeded(), "standard type resolves through empty view");
    Require(!project->objectDocuments_[0], "query never mutates the project");
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
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try { EmptyAndLifetime(); SourceAndNames(); std::cout << "ObjectContextProvider tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

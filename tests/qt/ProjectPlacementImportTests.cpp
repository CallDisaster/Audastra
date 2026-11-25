#include "ProjectPlacementImport.h"
#include "EditorProject.h"
#include "SyntheticObjectCatalogFixture.h"
#include "../PlacementCreationFixtures.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Workspace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <iostream>

namespace {
using namespace War3Map;
using ObjectData::ObjectType;
using QtProject::EditorProject;

void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template<class T> bool HasCode(const T& value, std::string_view code) {
    return std::any_of(value.diagnostics.begin(), value.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
template<class T> std::vector<std::byte> Bytes(Result<T> serialized) {
    Require(serialized.Succeeded() && serialized.value, "original placement member");
    return std::move(*serialized.value);
}

// 原创小清单：一个导入模型，一个装饰物类型（两处摆放），一个单位类型。
constexpr std::string_view kManifest = R"({"format":"sonne-placements/v1",
 "files":[{"source":"rock.mdx","archivePath":"Sonne\\rock.mdx"}],
 "types":[{"key":"rock","kind":"doodad","model":"Sonne\\rock.mdx","base":"D001"},
  {"key":"hero","kind":"unit","model":"Sonne\\rock.mdx","base":"u001"}],
 "instances":[{"type":"rock","x":0,"y":0,"z":310.5,"facing":90,"scale":[1,2,3]},
  {"type":"rock","x":128,"y":-128,"z":0,"facing":0,"scale":[1,1,1]},
  {"type":"hero","x":-256,"y":256,"z":0,"facing":0,"scale":[1,1,1]}]})";

MapData::PlacementManifest Manifest(std::string_view text = kManifest) {
    auto parsed = MapData::ParsePlacementManifest(Tests::ObjectBytes(text));
    Require(parsed.Succeeded() && parsed.value, "synthetic placement list");
    for (auto& file : parsed.value->files) file.bytes = Tests::ObjectBytes("MDLX-synthetic");
    return std::move(*parsed.value);
}

struct Fixture {
    QTemporaryDir source;
    EditorProject project;
    explicit Fixture(bool unitsMember = true) {
        Require(source.isValid() && QDir(source.path()).mkpath("map"), "synthetic map folder");
        Write("map/war3map.w3e", Tests::PlacementTerrainBytes());
        Write("map/war3map.doo", Bytes(MapData::DoodadPlacements{}.Serialize()));
        if (unitsMember) Write("map/war3mapUnits.doo", Bytes(MapData::UnitPlacements{}.Serialize()));
        project.projectRoot_ = std::filesystem::path(source.path().toStdWString());
        auto workspace = DirectoryWorkspace::Open(project.projectRoot_);
        Require(workspace.Succeeded(), "synthetic directory snapshot");
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
        Tests::ObjectCommandsFixture objects(true);
        Require(Tests::InstallSyntheticGameCatalog(project, objects.standard), "synthetic game catalog");
        project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(objects.schema));
        for (std::size_t i = 0; i < objects.documents.size(); ++i) {
            project.objectDocuments_[i] = std::move(objects.documents[i]);
            project.objectUndoStacks_[i] = new QUndoStack(&project);
        }
        project.LoadMapDocument();
        project.ResetEditTracking();
        Require(project.mapEdits_ && project.mapEdits_->Terrain() && project.mapDataUndoStack_ && project.AssetUndoStack()
            && project.editTrackingReady_ && !project.editTrackingFailed_, "synthetic project ready");
    }
    void Write(const char* name, const std::vector<std::byte>& bytes) {
        QFile file(source.filePath(QString::fromUtf8(name)));
        Require(file.open(QIODevice::WriteOnly), "open synthetic member");
        const auto size = static_cast<qint64>(bytes.size());
        Require(file.write(reinterpret_cast<const char*>(bytes.data()), size) == size, "write synthetic member");
    }
    QUndoStack& Assets() { return *project.AssetUndoStack(); }
    QUndoStack& World() { return *project.mapDataUndoStack_; }
    QUndoStack& Types(ObjectType type) { return *project.objectUndoStacks_[static_cast<std::size_t>(type)]; }
    bool HasType(ObjectType type, const char* rawcode) const {
        const auto& document = project.objectDocuments_[static_cast<std::size_t>(type)];
        return document && document->FindObject({type, rawcode});
    }
    std::size_t Doodads() const { const auto snapshot = project.mapEdits_->Snapshot(); return snapshot.Doodads() ? snapshot.Doodads()->doodads.size() : 0; }
    std::size_t Units() const { const auto snapshot = project.mapEdits_->Snapshot(); return snapshot.Units() ? snapshot.Units()->units.size() : 0; }
    bool Imported() const {
        const auto key = EditorProject::AssetKey(QStringLiteral("Sonne\\rock.mdx"));
        const auto entries = project.AssetEntries();
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == key; });
    }
    bool Untouched() {
        return Assets().count() == 0 && World().count() == 0 && Types(ObjectType::Doodad).count() == 0
            && Types(ObjectType::Unit).count() == 0 && !Imported() && !HasType(ObjectType::Doodad, "D000")
            && !HasType(ObjectType::Unit, "u000") && Doodads() == 0 && Units() == 0;
    }
};

void UndoStaysInEachEditor() {
    Fixture f;
    const auto committed = QtProject::CommitProjectPlacementImport(f.project, Manifest());
    Require(committed.Succeeded() && committed.value, "placement list imports");
    Require(committed.value->doodads == 2 && committed.value->units == 1 && committed.value->files == 1
        && committed.value->replacedFiles == 0 && committed.value->types.size() == 2, "import report");
    Require(f.Assets().count() == 1 && f.World().count() == 1 && f.Types(ObjectType::Doodad).count() == 1
        && f.Types(ObjectType::Unit).count() == 1 && f.Types(ObjectType::Ability).count() == 0,
        "files, each object category and the placements record one step in their own editor");
    Require(f.HasType(ObjectType::Doodad, "D000") && f.HasType(ObjectType::Unit, "u000") && f.Doodads() == 2 && f.Units() == 1
        && f.Imported(), "all parts present");
    f.World().undo();
    Require(f.Doodads() == 0 && f.Units() == 0 && f.HasType(ObjectType::Doodad, "D000") && f.HasType(ObjectType::Unit, "u000")
        && f.Imported(), "world undo removes only the placements");
    f.Types(ObjectType::Doodad).undo();
    Require(!f.HasType(ObjectType::Doodad, "D000") && f.HasType(ObjectType::Unit, "u000") && f.Imported(),
        "doodad type undo keeps the unit type and the files");
    f.Assets().undo();
    Require(!f.Imported() && f.HasType(ObjectType::Unit, "u000"), "file undo keeps object types");
    f.Assets().redo(); f.Types(ObjectType::Doodad).redo(); f.World().redo();
    Require(f.Imported() && f.HasType(ObjectType::Doodad, "D000") && f.Doodads() == 2 && f.Units() == 1, "redo restores each part");
    const auto again = QtProject::PlanProjectPlacementImport(f.project, Manifest());
    Require(again.Succeeded() && again.value && again.value->report.replacedFiles == 1
        && again.value->report.types[0].rawcode == "D002", "next plan sees the imported file and allocates past created types");
}

void FailureRollsBackEarlierEditors() {
    // 地图缺单位摆放文件：文件和两类物编已经提交后摆放才失败，前面各步须撤回并移出撤销栈。
    Fixture f(false);
    const auto failed = QtProject::CommitProjectPlacementImport(f.project, Manifest());
    Require(!failed.Succeeded() && HasCode(failed, "PLACEMENT_FILE_UNAVAILABLE"), "missing unit placement file refuses the list");
    Require(f.Untouched(), "earlier steps rolled back without leaving redo entries");
}

void RejectedPlanTouchesNothing() {
    Fixture f;
    auto text = std::string(kManifest);
    text.replace(text.find(R"("x":128)"), 7, R"("x":9000)");
    const auto rejected = QtProject::CommitProjectPlacementImport(f.project, Manifest(text));
    Require(!rejected.Succeeded() && HasCode(rejected, "PLACEMENT_IMPORT_OUT_OF_BOUNDS"), "out of map instance rejects the list");
    Require(f.Untouched(), "rejected list changes no editor");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        UndoStaysInEachEditor(); FailureRollsBackEarlierEditors(); RejectedPlanTouchesNothing();
        std::cout << "ProjectPlacementImport tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "EditorProject.h"
#include "MapEntitiesModule.h"
#include "WorldModule.h"
#include "WorldNavigationBookmark.h"
#include "WorldOutlinerModel.h"
#include "WorldViewport.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace War3Map;
using namespace War3Map::Modules;

namespace {
double Milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

// 仅输出小型回执，不输出地图正文、不写磁盘；失败也保留已完成阶段的真实测量。
struct Receipt {
    QJsonObject data{{"schema", 1}, {"evidence", "Qt CPU navigation only"}, {"passed", false},
        {"gpu_verified", false}, {"native_editor_verified", false}, {"map_copy_created", false}};
    std::map<QString, std::vector<double>> timings;
    ~Receipt() {
        QJsonObject measured;
        for (const auto& [name, samples] : timings) {
            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end());
            QJsonArray values;
            for (const auto value : samples) values.push_back(value);
            measured.insert(name, QJsonObject{{"samples_ms", values}, {"count", static_cast<int>(samples.size())},
                {"maximum_ms", sorted.empty() ? 0.0 : sorted.back()},
                {"median_ms", sorted.empty() ? 0.0 : sorted[sorted.size() / 2]}});
        }
        data.insert("navigation", measured);
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        std::fputs("NAVIGATION_REAL_RECEIPT ", stdout);
        (void)std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
};

std::optional<QJsonObject> Capture(Shell::EditorModule& module, Receipt& receipt, const QString& name) {
    QElapsedTimer timer; timer.start();
    auto captured = module.CaptureNavigationBookmark();
    receipt.timings[name].push_back(Milliseconds(timer));
    return captured;
}

Shell::NavigationRestoreResult Restore(Shell::EditorModule& module, const QJsonObject& state,
    Receipt& receipt, const QString& name) {
    QElapsedTimer timer; timer.start();
    const auto restored = module.RestoreNavigationBookmark(state);
    receipt.timings[name].push_back(Milliseconds(timer));
    return restored;
}

// 只散列七类 typed 文档已持有的原始成员；不复制归档 Body，不把检查费用计入导航函数。
Result<std::string> DocumentDigest(const MapDocument& document) {
    std::string summary;
    for (const auto& member : document.Members()) {
        summary += std::to_string(static_cast<int>(member.kind)) + ":" + member.memberName + ":"
            + std::to_string(static_cast<int>(member.state)) + ":"
            + (member.recordCount ? std::to_string(*member.recordCount) : "unknown") + ":";
        for (const auto& source : member.sources) {
            auto hash = Sha256(source.data);
            if (!hash.Succeeded()) return hash;
            summary += source.name + ":" + std::to_string(source.data.size()) + ":" + *hash.value + ";";
        }
    }
    return Sha256Text(summary);
}

struct UndoState {
    const QUndoStack* owner = nullptr;
    int count = 0, index = 0, cleanIndex = 0;
    bool canUndo = false, canRedo = false;
    bool operator==(const UndoState&) const = default;
};

std::vector<UndoState> UndoStates(const QtProject::EditorProject& project) {
    std::vector<UndoState> result;
    const auto add = [&result](const QUndoStack* stack) {
        if (stack) result.push_back({stack, stack->count(), stack->index(), stack->cleanIndex(), stack->canUndo(), stack->canRedo()});
    };
    add(project.mapDataUndoStack_); add(project.triggerUndoStack_); add(project.assetUndoStack_);
    for (const auto* stack : project.objectUndoStacks_) add(stack);
    return result;
}

bool SameWorldView(const QJsonObject& actual, const QJsonObject& expected) {
    for (const auto* key : {"schema", "map", "selection", "primary", "camera", "search", "category",
        "horizontal", "vertical", "expanded"})
        if (actual.value(QLatin1String(key)) != expected.value(QLatin1String(key))) return false;
    return actual.value("loadedRows").toInt() >= expected.value("loadedRows").toInt();
}

QString FailureText(const std::vector<Diagnostic>& diagnostics) {
    QString result;
    for (std::size_t index = 0; index < std::min(diagnostics.size(), std::size_t{4}); ++index)
        result += QString::fromStdString(diagnostics[index].code + ": " + diagnostics[index].message) + QLatin1Char('\n');
    return result;
}
} // namespace

class NavigationRealTests final : public QObject {
    Q_OBJECT
public:
    NavigationRealTests(QString map, double limit) : map_(std::move(map)), operationLimitMs_(limit) {}
private slots:
    void OwnedMapCpuRoundTrips();
private:
    QString map_;
    double operationLimitMs_ = 0;
};

void NavigationRealTests::OwnedMapCpuRoundTrips() {
    Receipt receipt;
    receipt.data.insert("measurement_scope", "three post-bind bookmark round trips; initial bookmark setup excluded");
    receipt.data.insert("round_trip_count", 3);
    receipt.data.insert("timing_budget_enforced", operationLimitMs_ > 0);
    receipt.data.insert("operation_limit_ms", operationLimitMs_ > 0 ? QJsonValue(operationLimitMs_) : QJsonValue(QJsonValue::Null));
    receipt.data.insert("stage", "map-open");
    const QFileInfo source(map_);
    QVERIFY2(source.isAbsolute() && source.isFile() && source.size() > 0,
        "FORGE_NAVIGATION_OWNED_MAP must name an existing absolute, author-owned map copy");
    receipt.data.insert("sample_bytes", QString::number(source.size()));
    QElapsedTimer timer; timer.start();
    auto opened = MapArchiveSession::Open(std::filesystem::path(map_.toStdWString()));
    receipt.data.insert("map_open_ms", Milliseconds(timer));
    QVERIFY2(opened.Succeeded() && opened.value, qPrintable(FailureText(opened.diagnostics)));
    const auto sampleSha = opened.value->SourceSha256();
    receipt.data.insert("sample_sha256", QString::fromStdString(sampleSha));
    std::uint64_t decodedBytes = 0;
    for (const auto& member : opened.value->Members()) decodedBytes += member.data.size();
    receipt.data.insert("archive_owned_decoded_bytes", QString::number(decodedBytes));

    receipt.data.insert("stage", "typed-document-bind");
    timer.restart();
    QtProject::EditorProject project;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath();
    project.projectRoot_ = project.sourceArchivePath_.parent_path(); // 仅来源元数据；不创建/展开工程树。
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.ResetEditTracking();
    receipt.data.insert("typed_document_bind_ms", Milliseconds(timer));
    QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    const auto& document = *project.mapDocument_;
    QVERIFY2(document.Units() && !document.Units()->units.empty() && document.Regions() && !document.Regions()->entries.empty(),
        "This real-map case requires actual placements and regions; no synthetic records are substituted");
    const auto originalDigest = DocumentDigest(document); QVERIFY(originalDigest.Succeeded() && originalDigest.value);
    const auto originalSnapshotDigest = DocumentDigest(project.mapEdits_->Snapshot());
    QVERIFY(originalSnapshotDigest.Succeeded() && originalSnapshotDigest.value);
    const auto sessionIdentity = project.mapEdits_->SessionIdentity(), mapRevision = project.mapEdits_->Revision();
    const auto editGeneration = project.editRevision_.Generation(), editRevision = project.editRevision_.Revision();
    const auto scopes = project.editRevision_.Current();
    const auto journalCount = project.editRevision_.Journal().size();
    const auto originalUndo = UndoStates(project);
    const auto* const archiveOwner = project.archiveSession_.get();
    const auto* const documentOwner = project.mapDocument_.get();
    const auto* const sessionOwner = project.mapEdits_.get();

    const auto unitIndex = static_cast<std::uint32_t>(std::min(document.Units()->units.size() - 1, std::size_t{127}));
    const Services::WorldObjectId unitId = MapData::MakePlacementId(MapData::PlacementFile::Units, unitIndex, document.Units()->units[unitIndex]);
    const auto regionIndex = static_cast<std::uint32_t>(document.Regions()->entries.size() / 2);
    const Services::WorldObjectId regionId = MapData::MakeRegionId(regionIndex, document.Regions()->entries[regionIndex]);
    QJsonArray selected{WorldNavigation::Encode(unitId), WorldNavigation::Encode(regionId)};
    if (document.Doodads() && !document.Doodads()->doodads.empty()) {
        const auto index = static_cast<std::uint32_t>(document.Doodads()->doodads.size() / 2);
        selected.push_back(WorldNavigation::Encode(MapData::MakePlacementId(MapData::PlacementFile::Doodads, index, document.Doodads()->doodads[index])));
    }
    const bool hasCameraEntity = document.Cameras() && !document.Cameras()->entries.empty();
    if (hasCameraEntity) selected.push_back(WorldNavigation::Encode(MapData::MakeCameraId(0, document.Cameras()->entries[0])));
    receipt.data.insert("camera_entity_coverage", hasCameraEntity ? "actual-sample-record" : "absent-in-sample");
    receipt.data.insert("sample_record_counts", QJsonObject{
        {"units", QString::number(document.Units()->units.size())},
        {"doodads", QString::number(document.Doodads() ? document.Doodads()->doodads.size() : 0)},
        {"regions", QString::number(document.Regions()->entries.size())},
        {"cameras", QString::number(document.Cameras() ? document.Cameras()->entries.size() : 0)}});
    receipt.data.insert("typed_selection", selected);
    receipt.data.insert("typed_document_sha256", QString::fromStdString(*originalSnapshotDigest.value));

    receipt.data.insert("stage", "world-cpu-bind");
    timer.restart();
    // 非空但无任何回调的 API 只禁止 DLL 自动加载。没有 fake renderer、插件或 GPU 成功回执。
    const War3RenderPluginApiV2 disabledBackend{};
    WorldModule world(project, &disabledBackend);
    auto* worldPage = world.Widget(); worldPage->resize(1280, 720); worldPage->show();
    auto* viewport = world.Viewport();
    auto* tree = worldPage->findChild<QTreeView*>("worldOutliner");
    auto* outline = tree ? dynamic_cast<WorldOutlinerModel*>(tree->model()) : nullptr;
    QVERIFY(viewport && tree && outline);
    QTRY_VERIFY_WITH_TIMEOUT(viewport->VertexCount() > 0 && outline->Version() && world.CaptureNavigationBookmark(), 60000);
    receipt.data.insert("world_cpu_bind_ms", Milliseconds(timer));
    QCOMPARE(outline->Version()->document.sourceSha256, sampleSha);
    QCOMPARE(outline->Version()->document.generation, editGeneration);
    QCOMPARE(outline->Version()->document.revision, mapRevision);
    const auto worldBinding = outline->Version()->document;
    const auto worldCatalogRevision = outline->Version()->catalogRevision;
    receipt.data.insert("world_binding", QJsonObject{{"document_id", QString::fromStdString(worldBinding.documentId)},
        {"generation", QString::number(worldBinding.generation)}, {"revision", QString::number(worldBinding.revision)},
        {"source_sha256", QString::fromStdString(worldBinding.sourceSha256)}});
    for (int count = 0; !outline->Find(unitId).isValid() && outline->canFetchMore({}) && count < 32; ++count) outline->fetchMore({});
    QVERIFY(outline->Find(unitId).isValid());
    auto initial = world.CaptureNavigationBookmark(); QVERIFY(initial);
    auto seedA = *initial;
    seedA.insert("selection", selected); seedA.insert("primary", WorldNavigation::Encode(unitId));
    auto cameraA = viewport->SaveCamera(); cameraA.insert("yaw", 0.75); cameraA.insert("distance", 1700.0);
    seedA.insert("camera", cameraA);
    QCOMPARE(world.RestoreNavigationBookmark(seedA), Shell::NavigationRestoreResult::Restored);
    tree->expandAll(); tree->doItemsLayout();
    tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
    tree->horizontalScrollBar()->setValue(tree->horizontalScrollBar()->maximum() / 2);
    const auto worldA = world.CaptureNavigationBookmark(); QVERIFY(worldA);
    QCOMPARE(worldA->value("selection"), QJsonValue(selected));
    QCOMPARE(worldA->value("primary"), QJsonValue(WorldNavigation::Encode(unitId)));
    auto seedB = *worldA;
    seedB.insert("selection", QJsonArray{WorldNavigation::Encode(regionId)});
    seedB.insert("primary", WorldNavigation::Encode(regionId));
    seedB.insert("category", static_cast<int>(Services::WorldCategory::Region));
    seedB.insert("search", QString{}); seedB.insert("loadedRows", 0); seedB.insert("expanded", QJsonArray{});
    seedB.insert("vertical", 0); seedB.insert("horizontal", 0);
    auto cameraB = viewport->SaveCamera(); cameraB.insert("yaw", 1.25); cameraB.insert("distance", 2100.0);
    seedB.insert("camera", cameraB);
    QCOMPARE(world.RestoreNavigationBookmark(seedB), Shell::NavigationRestoreResult::Restored);
    tree->expandAll(); tree->doItemsLayout();
    tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 3);
    const auto worldB = world.CaptureNavigationBookmark(); QVERIFY(worldB);
    QVERIFY(worldA->value("camera") != worldB->value("camera"));
    QCOMPARE(world.RestoreNavigationBookmark(*worldA), Shell::NavigationRestoreResult::Restored);

    receipt.data.insert("stage", "world-round-trips");
    for (int round = 0; round < 3; ++round) {
        QCOMPARE(Restore(world, *worldB, receipt, "world_restore_b"), Shell::NavigationRestoreResult::Restored);
        const auto b = Capture(world, receipt, "world_capture_b"); QVERIFY(b && SameWorldView(*b, *worldB));
        QCOMPARE(viewport->SaveCamera(), worldB->value("camera").toObject());
        QCOMPARE(Restore(world, *worldA, receipt, "world_restore_a"), Shell::NavigationRestoreResult::Restored);
        const auto a = Capture(world, receipt, "world_capture_a"); QVERIFY(a && SameWorldView(*a, *worldA));
        QCOMPARE(viewport->SaveCamera(), worldA->value("camera").toObject());
        const auto* primary = outline->RowAt(tree->currentIndex());
        QVERIFY(primary && primary->id == std::optional(unitId) && primary->selected && primary->primary);
        QVERIFY(outline->Version() && outline->Version()->document == worldBinding);
        QCOMPARE(outline->Version()->catalogRevision, worldCatalogRevision);
        QCOMPARE(project.mapEdits_->Revision(), mapRevision); QCOMPARE(project.editRevision_.Revision(), editRevision);
        QVERIFY(UndoStates(project) == originalUndo);
    }
    const auto backend = viewport->BackendDiagnosticSnapshot();
    QVERIFY(backend.value("injected").toBool() && !backend.value("library_loaded").toBool());
    QVERIFY(!backend.value("renderer_ready").toBool() && !backend.value("frame_timer_active").toBool());
    QVERIFY(!backend.value("api_valid").toBool());
    receipt.data.insert("backend", QJsonObject{{"plugin_loaded", false}, {"renderer_created", false},
        {"qpa", QGuiApplication::platformName()}, {"camera_request_only", true}});
    receipt.data.insert("world_camera_a", worldA->value("camera")); receipt.data.insert("world_camera_b", worldB->value("camera"));
    receipt.data.insert("world_outline_loaded_rows", worldA->value("loadedRows"));
    receipt.data.insert("world_outline_vertical", worldA->value("vertical"));
    receipt.data.insert("world_typed_selection_and_camera_verified", true);

    receipt.data.insert("stage", "entities-bind");
    timer.restart();
    MapEntitiesModule entities(project);
    auto* entitiesPage = entities.Widget(); entitiesPage->resize(960, 480); entitiesPage->show();
    auto* tabs = entitiesPage->findChild<QTabWidget*>();
    auto* raw = entitiesPage->findChild<QCheckBox*>("mapEntitiesRawFields");
    QVERIFY(tabs && raw);
    std::array<QTableWidget*, 4> tables{};
    for (int kind = 0; kind < 4; ++kind) {
        tables[kind] = entitiesPage->findChild<QTableWidget*>(QStringLiteral("mapEntitiesTable%1").arg(kind));
        QVERIFY(tables[kind]);
    }
    QCoreApplication::processEvents();
    receipt.data.insert("entities_bind_ms", Milliseconds(timer));
    for (int kind = 0; kind < 4; ++kind) {
        auto* table = tables[kind]; tabs->setCurrentIndex(kind);
        if (!table->rowCount()) continue;
        table->setCurrentCell(table->rowCount() / 2, std::min(2, table->columnCount() - 1));
        table->doItemsLayout(); table->verticalScrollBar()->setValue(table->verticalScrollBar()->maximum() / 2);
        table->horizontalScrollBar()->setValue(table->horizontalScrollBar()->maximum() / 2);
    }
    tabs->setCurrentIndex(0);
    const auto entitiesA = entities.CaptureNavigationBookmark(); QVERIFY(entitiesA);
    raw->setChecked(true);
    for (int kind = 0; kind < 4; ++kind) if (tables[kind]->rowCount()) {
        tables[kind]->setCurrentCell(0, 0); tables[kind]->doItemsLayout();
        tables[kind]->verticalScrollBar()->setValue(0); tables[kind]->horizontalScrollBar()->setValue(0);
    }
    tabs->setCurrentIndex(2);
    const auto entitiesB = entities.CaptureNavigationBookmark(); QVERIFY(entitiesB);
    QCOMPARE(entities.RestoreNavigationBookmark(*entitiesA), Shell::NavigationRestoreResult::Restored);
    receipt.data.insert("stage", "entities-round-trips");
    for (int round = 0; round < 3; ++round) {
        QCOMPARE(Restore(entities, *entitiesB, receipt, "entities_restore_b"), Shell::NavigationRestoreResult::Restored);
        const auto b = Capture(entities, receipt, "entities_capture_b"); QVERIFY(b && *b == *entitiesB);
        QCOMPARE(Restore(entities, *entitiesA, receipt, "entities_restore_a"), Shell::NavigationRestoreResult::Restored);
        const auto a = Capture(entities, receipt, "entities_capture_a"); QVERIFY(a && *a == *entitiesA);
        const auto savedTables = entitiesA->value("tables").toArray();
        for (int kind = 0; kind < 4; ++kind) {
            const auto saved = savedTables[kind].toObject();
            if (saved.value("selection").isNull()) { QVERIFY(!tables[kind]->currentItem()); continue; }
            const auto id = WorldNavigation::Decode(saved.value("selection")); QVERIFY(id);
            QCOMPARE(tables[kind]->currentRow(), static_cast<int>(WorldNavigation::Index(*id)));
            QCOMPARE(tables[kind]->currentColumn(), saved.value("column").toInt());
            const auto* item = tables[kind]->item(tables[kind]->currentRow(), 0); QVERIFY(item);
            QCOMPARE(item->data(Qt::UserRole + 20).toJsonObject(), WorldNavigation::Encode(*id));
            QCOMPARE(tables[kind]->verticalScrollBar()->value(), saved.value("vertical").toInt());
            QCOMPARE(tables[kind]->horizontalScrollBar()->value(), saved.value("horizontal").toInt());
        }
        QCOMPARE(project.mapEdits_->Revision(), mapRevision); QCOMPARE(project.editRevision_.Revision(), editRevision);
        QVERIFY(UndoStates(project) == originalUndo);
    }
    receipt.data.insert("entities_tables", entitiesA->value("tables"));
    receipt.data.insert("entities_typed_selection_and_scroll_verified", true);

    receipt.data.insert("stage", "document-unchanged");
    QVERIFY(project.archiveSession_.get() == archiveOwner && project.mapDocument_.get() == documentOwner && project.mapEdits_.get() == sessionOwner);
    QCOMPARE(project.archiveSession_->SourceSha256(), sampleSha);
    QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity); QCOMPARE(project.mapEdits_->Revision(), mapRevision);
    QCOMPARE(project.editRevision_.Generation(), editGeneration); QCOMPARE(project.editRevision_.Revision(), editRevision);
    QVERIFY(project.editRevision_.Current() == scopes && project.editRevision_.Journal().size() == journalCount);
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Map) && !project.editRevision_.IsDirty(EditSaveTarget::Project));
    QVERIFY(!project.mapEdits_->IsDirty() && !project.mapEdits_->CanUndo() && !project.mapEdits_->CanRedo());
    QVERIFY(UndoStates(project) == originalUndo);
    const auto afterOriginal = DocumentDigest(*project.mapDocument_);
    const auto afterSnapshot = DocumentDigest(project.mapEdits_->Snapshot());
    QVERIFY(afterOriginal.Succeeded() && afterOriginal.value && *afterOriginal.value == *originalDigest.value);
    QVERIFY(afterSnapshot.Succeeded() && afterSnapshot.value && *afterSnapshot.value == *originalSnapshotDigest.value);
    QCOMPARE(QFileInfo(map_).size(), source.size());
    // 源图完整复核只执行一次，单独计时；它不属于每页/每次导航热路径。
    timer.restart();
    const auto sourceUnchanged = project.archiveSession_->VerifySourceUnchanged();
    receipt.data.insert("source_reverification_ms", Milliseconds(timer));
    QVERIFY2(sourceUnchanged.Succeeded(), qPrintable(FailureText(sourceUnchanged.diagnostics)));
    receipt.data.insert("source_unchanged", true);
    receipt.data.insert("map_data_revision", QString::number(mapRevision));
    receipt.data.insert("edit_revision", QString::number(editRevision));
    receipt.data.insert("edit_generation", QString::number(editGeneration));
    receipt.data.insert("document_unchanged", true); receipt.data.insert("undo_unchanged", true);
    receipt.data.insert("stage", "timing-budget");
    if (operationLimitMs_ > 0) for (const auto& [name, samples] : receipt.timings) {
        QVERIFY(samples.size() == 3);
        QVERIFY2(*std::max_element(samples.begin(), samples.end()) <= operationLimitMs_,
            qPrintable(name + QStringLiteral(" exceeded FORGE_NAVIGATION_MAX_OPERATION_MS; maximum, not mean, is the gate")));
    }
    receipt.data.insert("stage", "complete"); receipt.data.insert("passed", true);
}

int main(int argc, char** argv) {
    const auto map = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP");
    if (map.isEmpty()) {
        std::fputs("NAVIGATION_REAL_RECEIPT {\"schema\":1,\"skipped\":true,\"reason\":\"FORGE_NAVIGATION_OWNED_MAP is not set\"}\n", stdout);
        return 77;
    }
    double limit = 0;
    const auto configured = qEnvironmentVariable("FORGE_NAVIGATION_MAX_OPERATION_MS");
    if (!configured.isEmpty()) {
        bool valid = false; limit = configured.toDouble(&valid);
        if (!valid || !std::isfinite(limit) || limit <= 0 || limit > 600000) {
            std::fputs("FORGE_NAVIGATION_MAX_OPERATION_MS must be a finite positive millisecond limit, at most 600000\n", stderr);
            return 2;
        }
    }
    qputenv("QT_QPA_PLATFORM", "offscreen"); // 强制当前测试进程不可见，不启动作者桌面的窗口或 GPU。
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("NavigationRealTests"));
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) return 2;
    NavigationRealTests tests(map, limit);
    return QTest::qExec(&tests, argc, argv);
}

#include "NavigationRealTests.moc"

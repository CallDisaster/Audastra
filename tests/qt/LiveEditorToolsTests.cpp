#include "LiveEditorTools.h"
#include "EditorProject.h"
#include "ProjectOpen.h"
#include "ProjectLuaModules.h"
#include "Commands/TriggerCommands.h"
#include "Commands/ObjectSnapshotCommand.h"
#include "McpObjectTools.h"
#include "McpVariableTools.h"
#include "McpMapDataTools.h"
#include "McpMapRuntimeTools.h"
#include "McpImportTools.h"
#include "McpObjectResourceTools.h"
#include "McpPlacementTools.h"
#include "McpAuxiliaryTools.h"
#include "ProjectAuxiliaryEdits.h"
#include "McpTerrainEditTools.h"
#include "McpCodeCheckTools.h"
#include "McpCampaignTools.h"
#include "McpProjectSearchTools.h"
#include "McpLuaTestTools.h"
#include "McpLuaFileTools.h"
#include "McpWorldViewportTools.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "../PlacementCreationFixtures.h"
#include "SyntheticObjectCatalogFixture.h"
#include "TerrainWaterEditFixture.h"
#include "../MapDataFixtures.h"
#include "../MapDataSpatialFixtures.h"
#include "../TerrainCornerRampFixtures.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ProjectStore.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Workspace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <vector>

using namespace War3Map;
using namespace War3Map::Trigger;
using War3Map::QtApp::LiveEditorTools;
using War3Map::QtApp::WithBindingToken;
using War3Map::QtProject::EditorProject;

namespace {
bool WriteFile(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray Bytes(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}
QString ErrorCode(const QJsonObject& result) {
    const auto rows = result.value("diagnostics").toArray();
    return rows.isEmpty() ? QString{} : rows[0].toObject().value("code").toString();
}
QString Explain(const QJsonObject& result) {
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}
QJsonObject BindingJson(const TriggerGraphBinding& binding) {
    return {{"source_wtg_sha256", QString::fromStdString(binding.sourceWtgSha256)},
        {"source_wct_sha256", QString::fromStdString(binding.sourceWctSha256)},
        {"ui_catalog_sha256", QString::fromStdString(binding.uiCatalogSha256)},
        {"current_wtg_sha256", QString::fromStdString(binding.currentWtgSha256)},
        {"current_wct_sha256", QString::fromStdString(binding.currentWctSha256)}};
}
QJsonObject SetField(StableId id, const char* field, QJsonValue before, QJsonValue after) {
    return {{"op", "set"}, {"target", QJsonObject{{"kind", "trigger"}, {"id", QString::number(id)}}},
        {"field", field}, {"expected", before}, {"value", after}};
}
QStringList Names(const QJsonArray& tools) {
    QStringList result;
    for (const auto& tool : tools) result.append(tool.toObject().value("name").toString());
    result.sort();
    return result;
}
int ChangeEvents(const QSignalSpy& events) {
    int count = 0;
    for (const auto& row : events) if (row[0].toJsonObject().value("type") == QStringLiteral("changes")) ++count;
    return count;
}
QJsonObject RegionFields(const char* name, int creation) {
    return {{"name", name}, {"creation_number", creation}, {"left", -128}, {"bottom", -64},
        {"right", 256}, {"top", 512}, {"weather_id", ""}, {"ambient_sound", ""}, {"color_bgr", QJsonArray{1, 2, 3}}};
}
QJsonObject CameraFields(const char* name) {
    return {{"name", name}, {"target_x", 10}, {"target_y", 20}, {"z_offset", 30}, {"rotation", 90},
        {"angle_of_attack", 300}, {"target_distance", 1500}, {"roll", 0}, {"field_of_view", 70}, {"far_clipping_plane", 5000}};
}

// 原创、经 WTG/WCT 序列化与解析建立 seal 的小样例；不读取作者地图。
// 外部 UI 与源目录分别冻结，便于区分定义变化与地图来源变化。
struct Fixture {
    // 目录来源会观察父目录，以发现整个source被替换。隔离这层父目录，
    // 防止后台准备事件循环把系统Temp下其他任务的文件变动当作本夹具变化。
    QTemporaryDir sandbox;
    QTemporaryDir source{sandbox.filePath("source-XXXXXX")};
    QTemporaryDir ui{sandbox.filePath("ui-XXXXXX")};
    QTemporaryDir outputs{sandbox.filePath("outputs-XXXXXX")};
    EditorProject project;
    std::unique_ptr<LiveEditorTools> tools;
    QString error;
    StableId trigger = 0;

    bool Initialize(bool selectedLayers = false) {
        // 原创夹具不继承开发安装目录里的真实包；每项运行库测试自行提供合成来源。
        project.runtimePackageSources_.clear(); project.runtimeCatalogDiagnostics_.clear();
        if (!sandbox.isValid() || !source.isValid() || !ui.isValid() || !outputs.isValid()) return Fail("temporary directories");
        const auto uiRoot = ui.filePath("ui");
        if (!WriteFile(uiRoot + "/config", "base\n") ||
            !WriteFile(uiRoot + "/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n") ||
            !WriteFile(uiRoot + "/base/action.txt", "[DoNothing]\n[StoreInteger]\n[[.args]]\ntype=integer\n") ||
            !WriteFile(uiRoot + "/base/event.txt", {}) ||
            !WriteFile(uiRoot + "/base/condition.txt", {}) ||
            !WriteFile(uiRoot + "/base/call.txt", {})) return Fail("original UI fixture");
        const auto root = std::filesystem::path(uiRoot.toStdWString());
        const std::array<std::string, 1> layers{{"base"}};
        auto catalog = selectedLayers ? UiCatalog::LoadLayers(root, layers) : UiCatalog::Load(root);
        if (!catalog.Succeeded()) return Fail("catalog load");
        auto empty = WtgWctCodec{}.CreateEmpty(*catalog.value);
        if (!empty.Succeeded()) return Fail("sealed empty document");
        auto category = TriggerDirectoryEditor::AddCategory(*empty.value, "业务");
        if (!category.Succeeded()) return Fail("category creation");
        auto added = TriggerDocumentEditor::AddTrigger(*empty.value, *category.value, "0-0-1 原始业务");
        if (!added.Succeeded()) return Fail("trigger creation");
        if (!TriggerDocumentEditor::SetTriggerComment(*empty.value, *added.value, "原始说明").Succeeded() ||
            !TriggerDocumentEditor::SetGlobalCustomCode(*empty.value, "// 原有作者代码，仅透传\r\n").Succeeded())
            return Fail("original text fixture");
        const auto encoded = WtgWctCodec{}.Serialize(*empty.value, *catalog.value);
        if (!encoded.Succeeded() ||
            !WriteFile(source.filePath("map/war3map.wtg"), Bytes(encoded.value->wtg)) ||
            !WriteFile(source.filePath("map/war3map.wct"), Bytes(encoded.value->wct))) return Fail("source encoding");
        auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *catalog.value);
        if (!parsed.Succeeded()) return Fail("source seal");
        auto workspace = DirectoryWorkspace::Open(std::filesystem::path(source.path().toStdWString()));
        if (!workspace.Succeeded()) return Fail("directory snapshot");
        project.projectRoot_ = std::filesystem::path(source.path().toStdWString());
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
        project.uiDataRoot_ = root;
        project.activeUiDataRoot_ = root;
        project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(*catalog.value));
        project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(*parsed.value));
        trigger = project.triggerDocument_->triggers[0].id;
        project.triggerUndoStack_ = new QUndoStack(&project);
        QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project,
            [&] { project.ObserveTriggerEdit(); });
        project.ResetEditTracking();
        if (!project.editTrackingReady_ || project.editTrackingFailed_) return Fail("edit tracking");
        tools = std::make_unique<LiveEditorTools>(project);
        return true;
    }
    bool Fail(const char* reason) { error = QString::fromUtf8(reason); return false; }
    bool InstallObjects(bool missing = false, bool custom = false) {
        Tests::ObjectCommandsFixture fixture;
        if (custom) Tests::AddPlacementCustomObjects(fixture);
        if (!Tests::InstallSyntheticGameCatalog(project, fixture.standard)) return Fail("synthetic game catalog");
        project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(fixture.schema));
        for (std::size_t i = 0; i < fixture.documents.size(); ++i) {
            if (!missing) project.objectDocuments_[i] = std::move(fixture.documents[i]);
            project.objectUndoStacks_[i] = new QUndoStack(&project);
            QObject::connect(project.objectUndoStacks_[i], &QUndoStack::indexChanged, &project,
                [this, i] { project.ObserveObjectEdit(static_cast<int>(i)); });
        }
        project.ResetEditTracking();
        return project.editTrackingReady_ && !project.editTrackingFailed_;
    }
    bool InstallBounds(const TerrainCornerRampFixtures::Fixture& native) {
        for (const auto& member : native.members)
            if (!WriteFile(source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)))
                return Fail("bounds native source");
        project.war3GameRoot_ = native.game;
        auto workspace = DirectoryWorkspace::Open(project.projectRoot_);
        if (!workspace.Succeeded()) return Fail("bounds directory snapshot");
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
        project.LoadMapDocument(); project.ResetEditTracking();
        return project.mapEdits_ && project.mapEdits_->Terrain() && project.mapDataUndoStack_
            && project.editTrackingReady_ && !project.editTrackingFailed_;
    }
    bool InstallAuxiliary(bool seeded = false, bool references = false) {
        // 完整原创 T 配对和经典 W3I；引用负例使用实际变量默认值，经过 codec 建立来源 seal。
        if (!WriteFile(ui.filePath("ui/base/define.txt"),
            "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nrect=0,1,1,Region,rect\ncamerasetup=0,1,1,Camera,camerasetup\n[TriggerParams]\n"))
            return Fail("auxiliary UI types");
        auto catalog = UiCatalog::Load(project.uiDataRoot_);
        if (!catalog.Succeeded()) return Fail("auxiliary UI catalog");
        project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(*catalog.value));
        auto empty = WtgWctCodec{}.CreateEmpty(*project.uiCatalog_);
        if (!empty.Succeeded()) return Fail("auxiliary empty T seal");
        const auto category = TriggerDirectoryEditor::AddCategory(*empty.value, "业务");
        if (!category.Succeeded()) return Fail("auxiliary category");
        const auto added = TriggerDocumentEditor::AddTrigger(*empty.value, *category.value, "区域与镜头业务");
        if (!added.Succeeded()) return Fail("auxiliary trigger");
        project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(*empty.value));
        if (references) {
            for (const auto& [name, type, symbol] : std::array{
                std::array<const char*, 3>{"RegionOwner", "rect", "gg_rct_First"},
                std::array<const char*, 3>{"CameraOwner", "camerasetup", "gg_cam_View"}}) {
                if (!TriggerDocumentEditor::AddVariable(*project.triggerDocument_, name, type).Succeeded())
                    return Fail("auxiliary reference variable");
                auto& variable = project.triggerDocument_->variables.back();
                variable.hasDefault = 1; variable.value.text = symbol;
            }
        }
        const auto encoded = WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        if (!encoded.Succeeded() || !WriteFile(source.filePath("map/war3map.wtg"), Bytes(encoded.value->wtg))
            || !WriteFile(source.filePath("map/war3map.wct"), Bytes(encoded.value->wct))
            || !WriteFile(source.filePath("map/war3map.w3i"), Bytes(MapDataFixtures::FullMapSettingsV25().bytes)))
            return Fail("auxiliary sealed sources");
        auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, *project.uiCatalog_);
        if (!parsed.Succeeded()) return Fail("auxiliary T source seal");
        project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(*parsed.value));
        trigger = project.triggerDocument_->triggers.front().id;
        if (seeded) {
            MapData::RegionsDocument regions;
            regions.entries.push_back({-128, -64, 256, 512, "First", 1, {}, "", {1, 2, 3, 177}});
            MapData::CamerasDocument cameras;
            MapData::CameraRecord camera{};
            camera.name = "View"; camera.targetX = 10; camera.targetY = std::bit_cast<float>(0x3f800001u);
            camera.zOffset = 30; camera.rotation = 90; camera.angleOfAttack = 300; camera.targetDistance = 1500;
            camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = std::bit_cast<float>(0x80000000u);
            cameras.entries.push_back(camera);
            const auto w3r = regions.Serialize(), w3c = cameras.Serialize();
            if (!w3r.Succeeded() || !w3c.Succeeded()
                || !WriteFile(source.filePath("map/war3map.w3r"), Bytes(*w3r.value))
                || !WriteFile(source.filePath("map/war3map.w3c"), Bytes(*w3c.value))) return Fail("auxiliary native tables");
        }
        auto workspace = DirectoryWorkspace::Open(project.projectRoot_);
        if (!workspace.Succeeded()) return Fail("auxiliary source snapshot");
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
        project.LoadMapDocument(); project.ResetEditTracking();
        return project.mapEdits_ && project.mapDataUndoStack_ && project.editTrackingReady_ && !project.editTrackingFailed_;
    }
    QJsonObject AuxiliaryRequest(MapDataKind kind, std::optional<std::size_t> row = std::nullopt) const {
        auto result = Context();
        const auto document = project.mapEdits_->Snapshot();
        const auto member = Services::AuxiliaryEditing::MemberIdentity(document, kind);
        if (!member.Succeeded()) return {};
        result.insert("kind", kind == MapDataKind::Regions ? "regions" : "cameras");
        result.insert("expected_member_sha256", QString::fromStdString(*member.value));
        if (row) {
            const auto key = MapData::MapFieldRowKey(document, kind, *row);
            if (!key.Succeeded()) return {};
            result.insert("source", QJsonObject{{"source_index", QString::number(static_cast<qulonglong>(*row))},
                {"row_key", QString::fromStdString(*key.value)}});
        }
        return result;
    }
    QJsonObject Context() const {
        const auto state = tools->Invoke("editor_state", {});
        return {{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")}};
    }
    QJsonObject EditRequest(const QJsonArray& operations) const {
        auto result = Context();
        const auto binding = CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_);
        if (!binding.Succeeded()) return {};
        result.insert("expected", BindingJson(*binding.value));
        result.insert("operations", operations);
        return result;
    }
    QJsonObject DescriptionRequest(QString text = QStringLiteral("新的说明")) const {
        const auto* row = TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, trigger);
        return EditRequest({SetField(trigger, "description", QString::fromStdString(row->description.text), text)});
    }
    std::string Fingerprint() const {
        const auto binding = CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_);
        return binding.Succeeded() ? binding.value->currentWtgSha256 + ":" + binding.value->currentWctSha256 : std::string{};
    }
};
}

class LiveEditorToolsTests final : public QObject {
    Q_OBJECT
private slots:
    void OnlyLiveToolsAndActualCatalogAreExposed();
    void RuntimeToolsRequireExplicitConsentAndShareUndo();
    void CampaignToolsShareSchemasAndPolicy();
    void BatchKeepsDocumentAndExistingUndoCommands();
    void StaleAndInvalidBatchesAreAtomic();
    void BindingTokenReplacesIdentityFields();
    void SourceAndUiDriftRefuseBeforePublishing();
    void SelectedLayersRemainWritable();
    void GenerationRotatesIdentity();
    void PolicyGatesDiscoveryAndExecution();
    void DraftCommitCannotReenterOrHideRevisionChange();
    void HostSaveMustProduceActualOutput();
    void ActivityReportsRealRecordsAndBoundedExamples();
    void ChildChangesNavigateActualOwningTrigger();
    void ObjectToolsShareSchemasAndUndo();
    void MissingObjectsCreateUndoAndPolicy();
    void VariableToolsShareSchemasAndOriginalUndo();
    void VariableFailuresPreserveRevisionAndUndo();
    void EntityFieldsUseOriginalSessionAndAtomicUndo();
    void SpatialEntityQueriesShareSchemasAndObserveOriginalUndo();
    void SettingsAndStringsKeepOriginalGuiUndoAndPolicy();
    void PlacementToolsKeepAuthorEditsAndOriginalUndo();
    void PlacementCreateDeleteUseRealSourcesAndPurePreview();
    void AuxiliaryCrudKeepsOriginalUndoAndProjectRoundtrip();
    void AuxiliaryPartialFieldsAndPrepublishRejection();
    void AuxiliaryPreviewAndReferenceFailuresAreAtomic();
    void AuxiliaryReentryProjectReplacementAndSourceDrift();
    void TerrainToolsKeepSessionPreviewAndOriginalUndo();
    void WaterToolsCaptureActualResourcesAndKeepOriginalUndo();
    void BoundsToolsCaptureActualResourcesAndKeepOriginalUndo();
    void BoundsPreparationRejectsConcurrentRevision();
    void TerrainBlockToolsUseCapturedResourcesCasAndOriginalUndo();
    void ResizeToolsCaptureActualResourcesAndKeepOriginalUndo();
    void BoundaryDraftSavesAndReopensAsProject_data();
    void BoundaryDraftSavesAndReopensAsProject();
    void ImportMetadataUsesSourceAndCurrentAssetOverlay();
    void LuaFileToolsShareAssetUndoAndRejectStaleText();
    void LuaFileToolsCreateOnlyAtProvenArchiveAbsence();
    void ImportsAddQueuesOwnedBytesWithUndo();
    void LuaFilesListPagesCapturedDirectoryAndRejectsStaleContinuation();
    void ResourceToolsUseActualAsyncSourceAndRejectStaleResults();
    void ProjectSearchUsesInjectedOwnedPagesAndStrictRevision();
    void ProjectSearchCallbackReplacementStopsOldRequest();
    void ProjectSearchOwnerDeletionKeepsInvocationAlive();
    void ViewportToolsUseActualHostAndRejectStaleIdentity();
    void GameUiToolsUseHostAndValidateArguments();
};

void LiveEditorToolsTests::OnlyLiveToolsAndActualCatalogAreExposed() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const QStringList expected{"auxiliary_copy", "auxiliary_create", "auxiliary_delete", "auxiliary_update",
        "code.check", "create_campaign", "edit_campaign", "edit_terrain_block", "edit_terrain_bounds", "edit_terrain_resize", "edit_terrain_stroke", "editor_check_trigger_graph",
        "editor_edit_trigger_graph", "editor_get_ui_catalog", "editor_query_trigger_graph", "editor_save_map", "editor_state",
        "entities_copy", "entities_create", "entities_delete", "entities_query", "entities_rotate", "entities_set_fields",
        "export_campaign_member",
        "feedback.draft", "feedback.list", "feedback.status",
        "game_ui_description_create", "game_ui_description_edit", "game_ui_description_read", "game_ui_description_triggers",
        "game_ui_nodes_query", "game_ui_preview_set", "game_ui_render", "game_ui_simulate", "game_ui_trigger_simulation",
        "game_ui_view_read", "game_ui_view_set",
        "get_terrain_edit_state",
        "imports_add", "imports_get", "imports_list", "imports_remove", "lua_file_read", "lua_file_write", "lua_files_list",
        "lua_test_hot_reload", "lua_test_reload_result",
        "map_runtime_query", "map_runtime_set", "map_settings_get", "map_settings_set", "map_test_capture", "map_test_eval", "map_test_result", "map_test_start", "map_test_stop",
        "object_resource_inspect", "object_resource_query", "object_resource_state",
        "objects_apply_text", "objects_create", "objects_delete", "objects_field_options", "objects_get", "objects_list", "objects_set_fields",
        "objects_text",
        "open_campaign", "project_search", "query_campaign", "query_campaign_objects", "query_terrain_points", "redo_campaign", "save_campaign",
        "sounds_get", "sounds_list", "sounds_update",
        "strings_query", "strings_set",
        "undo_campaign", "update.status",
        "variables_create", "variables_delete", "variables_get", "variables_list", "variables_rename", "variables_update",
        "world_viewport_camera_read", "world_viewport_camera_set", "world_viewport_capture", "world_viewport_capture_result"};
    QCOMPARE(Names(f.tools->Tools()), expected);
    QCOMPARE(f.tools->RegisteredToolNames(), expected);
    QCOMPARE(expected.size(), qsizetype{92});
    for (const auto& definition : Mcp::AuxiliaryToolDefinitions()) {
        QCOMPARE(definition.permission, Mcp::ToolPermission::Edit);
        int matches = 0;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject();
            if (tool.value("name") != definition.name) continue;
            ++matches; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QCOMPARE(matches, 1);
    }
    const auto luaDefinitions = Mcp::LuaTestToolDefinitions();
    QCOMPARE(luaDefinitions.size(), std::size_t{2});
    QCOMPARE(luaDefinitions[0].permission, Mcp::ToolPermission::External);
    QCOMPARE(luaDefinitions[1].permission, Mcp::ToolPermission::Query);
    const auto editorState = f.tools->Invoke("editor_state", {});
    QJsonObject luaArgs{{"project_identity", editorState.value("project_identity")}, {"expected_revision", editorState.value("revision")}};
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_test_hot_reload", luaArgs)), QString("LUA_RELOAD_NO_SESSION"));
    int calls = 0;
    f.tools->SetLuaTestHandler([&calls](bool submit) { ++calls; return QJsonObject{{"ok", true}, {"submit", submit}, {"sequence", "7"}}; });
    auto reload = f.tools->Invoke("lua_test_hot_reload", luaArgs);
    QVERIFY(reload.value("ok").toBool() && reload.value("submit").toBool());
    QVERIFY(!f.tools->Invoke("lua_test_reload_result", luaArgs).value("submit").toBool());
    QCOMPARE(calls, 2);
    auto arbitrary = luaArgs; arbitrary.insert("source", "print('unexpected')");
    QVERIFY(!f.tools->Invoke("lua_test_hot_reload", arbitrary).value("ok").toBool()); QCOMPARE(calls, 2);
    auto stale = luaArgs; stale.insert("expected_revision", "9999");
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_test_hot_reload", stale)), QString("LIVE_EDITOR_REVISION_CONFLICT")); QCOMPARE(calls, 2);
    const auto codeDefinition = Mcp::CodeCheckToolDefinition();
    QCOMPARE(codeDefinition.permission, Mcp::ToolPermission::Query);
    bool foundCodeCheck = false;
    for (const auto& value : f.tools->Tools()) {
        const auto tool = value.toObject();
        if (tool.value("name") != QStringLiteral("code.check")) continue;
        QVERIFY(!foundCodeCheck);
        foundCodeCheck = true;
        const auto schema = tool.value("inputSchema").toObject();
        QVERIFY(!schema.value("additionalProperties").toBool());
        const auto properties = schema.value("properties").toObject();
        QCOMPARE(properties.value("action").toObject().value("enum").toArray(), QJsonArray({"submit", "status", "cancel"}));
        QCOMPARE(properties.value("language").toObject().value("enum").toArray(), QJsonArray({"jass", "vjass", "lua53"}));
        QCOMPARE(properties.value("source_kind").toObject().value("enum").toArray(), QJsonArray({"text", "block"}));
        QVERIFY(properties.contains("project_identity") && properties.contains("expected_revision"));
    }
    QVERIFY(foundCodeCheck);
    QVERIFY(f.tools->Server().Workspace() == nullptr);
    const auto state = f.tools->Invoke("editor_state", {});
    QVERIFY(state.value("ok").toBool());
    QCOMPARE(state.value("scope").toString(), QStringLiteral("gui-live-document"));
    QCOMPARE(state.value("source_sha256").toString(), QString::fromStdString(f.project.directorySnapshot_->Snapshot().aggregateSha256));
    QVERIFY(!state.value("source_checked_in_this_call").toBool());
    auto request = f.Context(); request.insert("table", "triggers");
    const auto graph = f.tools->Invoke("editor_query_trigger_graph", request);
    QVERIFY2(graph.value("ok").toBool(), qPrintable(Explain(graph)));
    QCOMPARE(graph.value("rows").toArray().size(), qsizetype{1});
    QCOMPARE(graph.value("rows").toArray()[0].toObject().value("ref").toObject().value("id").toString(), QString::number(f.trigger));
    request = f.Context(); request.insert("kind", "action"); request.insert("search", "DoNothing");
    const auto catalog = f.tools->Invoke("editor_get_ui_catalog", request);
    QVERIFY2(catalog.value("ok").toBool(), qPrintable(Explain(catalog)));
    QCOMPARE(catalog.value("rows").toArray()[0].toObject().value("name").toString(), QStringLiteral("DoNothing"));
    QVERIFY(!catalog.value("rows").toArray()[0].toObject().value("signature").toString().isEmpty());
    // 不给 kind 时搜索全部种类（夹具目录只有动作）。
    request = f.Context(); request.insert("search", "DoNothing");
    const auto anyRows = f.tools->Invoke("editor_get_ui_catalog", request).value("rows").toArray();
    QVERIFY(std::any_of(anyRows.begin(), anyRows.end(), [](const QJsonValue& row) {
        return row.toObject().value("signature").toString().startsWith(QStringLiteral("action:DoNothing("));
    }));
    // 活动编辑器真正的 code.check 接线：独立正文只检语法，不执行未知 native、不写地图。
    // 查询权限足够，不能为了检查正文授予编辑/保存或外部运行权限。
    Mcp::ToolPolicy queryOnly;
    queryOnly.allowEdit = false; queryOnly.allowSave = false; queryOnly.allowExternal = false;
    f.tools->SetPolicy(queryOnly);
    const auto original = f.Fingerprint();
    const auto context = f.Context();
    request = context;
    request.insert("action", "submit"); request.insert("source_kind", "text"); request.insert("language", "jass");
    request.insert("source_name", "MCP/check-only.j");
    request.insert("text", "function CheckOnly takes nothing returns nothing\n    call NotExecutedNative()\nendfunction\n");
    const auto submitted = f.tools->Invoke("code.check", request);
    QVERIFY2(submitted.value("ok").toBool(), qPrintable(Explain(submitted)));
    QCOMPARE(submitted.value("state").toString(), QStringLiteral("queued"));
    QVERIFY(submitted.contains("executes_code") && !submitted.value("executes_code").toBool());
    QVERIFY(submitted.contains("writes_map") && !submitted.value("writes_map").toBool());
    request = context; request.insert("action", "status"); request.insert("request_id", submitted.value("request_id"));
    QJsonObject completed;
    QTRY_VERIFY2_WITH_TIMEOUT((completed = f.tools->Invoke("code.check", request)).value("state").toString() == QStringLiteral("complete"),
        qPrintable(Explain(completed)), 2000);
    QVERIFY2(completed.value("ok").toBool() && completed.value("passed").toBool(), qPrintable(Explain(completed)));
    QVERIFY(completed.value("syntax_checked").toBool());
    QVERIFY(!completed.value("semantic_checked").toBool() && !completed.value("context_complete").toBool());
    QVERIFY(completed.contains("executes_code") && !completed.value("executes_code").toBool());
    QVERIFY(completed.contains("writes_map") && !completed.value("writes_map").toBool());
    QCOMPARE(f.Fingerprint(), original);
    QCOMPARE(f.Context(), context);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::CampaignToolsShareSchemasAndPolicy() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const QStringList queryNames{"query_campaign", "query_campaign_objects"};
    const QStringList saveNames{"export_campaign_member", "save_campaign"};
    const QStringList editNames{"create_campaign", "edit_campaign", "open_campaign", "redo_campaign", "undo_campaign"};
    QStringList expected = queryNames + saveNames + editNames; expected.sort();
    const auto definitions = Mcp::CampaignToolDefinitions(f.tools->Server());
    const auto liveTools = f.tools->Tools();
    QStringList actual;
    for (const auto& definition : definitions) {
        actual.append(definition.name);
        QVERIFY2(expected.contains(definition.name), qPrintable(definition.name));
        const auto permission = queryNames.contains(definition.name) ? Mcp::ToolPermission::Query
            : saveNames.contains(definition.name) ? Mcp::ToolPermission::Save : Mcp::ToolPermission::Edit;
        QCOMPARE(definition.permission, permission);
        int matches = 0;
        for (const auto& value : liveTools) {
            const auto tool = value.toObject();
            if (tool.value("name").toString() != definition.name) continue;
            ++matches; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QCOMPARE(matches, 1);
    }
    actual.sort(); QCOMPARE(actual, expected);

    int writes = 0;
    f.tools->Server().SetCampaignCallbacks([&] { ++writes; return true; }, {});
    Mcp::ToolPolicy policy; policy.allowEdit = false; policy.allowSave = false; f.tools->SetPolicy(policy);
    const auto visible = Names(f.tools->Tools());
    for (const auto& name : expected) {
        QCOMPARE(visible.contains(name), queryNames.contains(name));
        QVERIFY(f.tools->RegisteredToolNames().contains(name));
    }
    QVERIFY(!f.tools->Invoke("create_campaign", {}).value("ok").toBool());
    QCOMPARE(writes, 0); QVERIFY(!f.tools->Server().Campaign());
}

void LiveEditorToolsTests::BatchKeepsDocumentAndExistingUndoCommands() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto* document = f.project.triggerDocument_.get();
    const auto authorCode = document->wct.customCode.ToBytes(); QVERIFY(authorCode.Succeeded());
    const auto original = f.Fingerprint();
    const auto added = TriggerDocumentEditor::AddTrigger(*document, document->categories[0].categoryId, "此前的界面编辑");
    QVERIFY(added.Succeeded());
    const StableId earlierId = *added.value;
    f.project.triggerUndoStack_->push(new AddTriggerUndoCommand(document, earlierId, QStringLiteral("界面新增 T")));
    const auto afterEarlierEdit = f.Fingerprint();
    const auto oldContext = f.Context();
    QSignalSpy changed(&f.project, &EditorProject::TriggerDocumentChanged);
    const auto edited = f.tools->Invoke("editor_edit_trigger_graph", f.EditRequest({
        SetField(f.trigger, "description", QStringLiteral("原始说明"), QStringLiteral("AI 批量说明")),
        SetField(f.trigger, "enabled", true, false)}));
    QVERIFY2(edited.value("ok").toBool(), qPrintable(Explain(edited)));
    QCOMPARE(f.project.triggerDocument_.get(), document);
    QCOMPARE(f.project.triggerUndoStack_->count(), 2);
    QCOMPARE(changed.count(), 1);
    QVERIFY(edited.value("revision") != oldContext.value("expected_revision"));
    QVERIFY(!edited.value("saved").toBool());
    QCOMPARE(edited.value("changes").toObject().value("changed_records").toInt(), 1);
    QCOMPARE(edited.value("changes").toObject().value("trigger_nodes").toInt(), 1);
    QVERIFY(!edited.value("changes").toObject().contains("added_lines"));
    const auto afterBatch = f.Fingerprint();
    f.project.triggerUndoStack_->undo(); QCOMPARE(f.Fingerprint(), afterEarlierEdit);
    f.project.triggerUndoStack_->undo(); QCOMPARE(f.Fingerprint(), original);
    f.project.triggerUndoStack_->redo();
    QVERIFY(TriggerDocumentEditor::FindTrigger(*document, earlierId) != nullptr);
    QCOMPARE(f.Fingerprint(), afterEarlierEdit);
    f.project.triggerUndoStack_->redo(); QCOMPARE(f.Fingerprint(), afterBatch);
    QCOMPARE(f.project.triggerDocument_.get(), document);
    const auto remainingCode = document->wct.customCode.ToBytes(); QVERIFY(remainingCode.Succeeded());
    QVERIFY(*remainingCode.value == *authorCode.value);
}

void LiveEditorToolsTests::BindingTokenReplacesIdentityFields() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto token = f.tools->Invoke("editor_state", {}).value("binding_token").toString();
    QVERIFY(token.startsWith(QStringLiteral("bt1-")));
    const auto* row = TriggerDocumentEditor::FindTrigger(*f.project.triggerDocument_, f.trigger);
    const QJsonArray operations{SetField(f.trigger, "description", QString::fromStdString(row->description.text), QStringLiteral("令牌说明"))};
    QJsonObject request{{"binding_token", token}, {"operations", operations}};
    auto mixed = request; mixed.insert("expected_revision", "0");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", mixed)), QStringLiteral("LIVE_EDITOR_IDENTITY_AMBIGUOUS"));
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", QJsonObject{{"operations", operations}})),
        QStringLiteral("LIVE_EDITOR_IDENTITY_REQUIRED"));
    const auto edited = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY2(edited.value("ok").toBool(), qPrintable(Explain(edited)));
    const auto next = edited.value("binding_token").toString();
    QVERIFY(next.startsWith(QStringLiteral("bt1-")) && next != token);
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", request)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    const auto queried = f.tools->Invoke("editor_query_trigger_graph", QJsonObject{{"binding_token", next}, {"table", "triggers"}});
    QVERIFY2(queried.value("ok").toBool(), qPrintable(Explain(queried)));
    QCOMPARE(queried.value("binding_token").toString(), next);
    const auto outline = f.tools->Invoke("editor_query_trigger_graph", QJsonObject{{"binding_token", next}, {"table", "outline"}});
    QVERIFY2(outline.value("ok").toBool(), qPrintable(Explain(outline)));
    const auto categories = outline.value("categories").toArray();
    QVERIFY(!categories.isEmpty() && outline.value("trigger_count").toString().toInt() == static_cast<int>(f.project.triggerDocument_->triggers.size()));
    const auto first = categories.first().toObject().value("triggers").toArray().first().toObject();
    const auto byName = f.tools->Invoke("editor_query_trigger_graph",
        QJsonObject{{"binding_token", next}, {"table", "outline"}, {"match", first.value("name").toString()}});
    QVERIFY(byName.value("trigger_count").toString().toInt() >= 1);
    const auto none = f.tools->Invoke("editor_query_trigger_graph",
        QJsonObject{{"binding_token", next}, {"table", "outline"}, {"match", "没有这样的名字"}});
    QVERIFY(none.value("ok").toBool() && none.value("categories").toArray().isEmpty());
    const auto legacy = f.tools->Invoke("editor_query_trigger_graph", QJsonObject{{"binding_token", next}, {"table", "legacy_trigger_references"}});
    QVERIFY2(legacy.value("ok").toBool(), qPrintable(Explain(legacy)));
    QVERIFY(legacy.value("references").toArray().isEmpty() && legacy.value("operations").toArray().isEmpty());
    QVERIFY(legacy.value("binding").isObject() && !legacy.value("truncated").toBool());
}

void LiveEditorToolsTests::StaleAndInvalidBatchesAreAtomic() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto original = f.Fingerprint();
    auto stale = f.DescriptionRequest(); stale.insert("project_identity", "different-project");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", stale)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    auto invalid = f.EditRequest({SetField(f.trigger, "description", QStringLiteral("原始说明"), "不得发表"),
        SetField(f.trigger, "enabled", false, true)});
    const auto rejected = f.tools->Invoke("editor_edit_trigger_graph", invalid);
    QVERIFY(!rejected.value("ok").toBool());
    QCOMPARE(f.Fingerprint(), original); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    const auto ready = f.DescriptionRequest();
    QVERIFY(f.tools->Invoke("editor_edit_trigger_graph", ready).value("ok").toBool());
    const auto latest = f.Fingerprint();
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", ready)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(f.Fingerprint(), latest); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    invalid = f.Context(); invalid.insert("table", "triggers"); invalid.insert("expected_revision", "0");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_query_trigger_graph", invalid)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
}

void LiveEditorToolsTests::SourceAndUiDriftRefuseBeforePublishing() {
    {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        const auto request = f.DescriptionRequest(); const auto before = f.Fingerprint();
        QVERIFY(WriteFile(f.source.filePath("map/external-change.txt"), "different"));
        const auto rejected = f.tools->Invoke("editor_edit_trigger_graph", request);
        QVERIFY(!rejected.value("ok").toBool());
        QVERIFY(f.project.sourceChangeConfirmed_);
        QCOMPARE(f.Fingerprint(), before); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    }
    {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        const auto request = f.DescriptionRequest(); const auto before = f.Fingerprint();
        QVERIFY(WriteFile(f.ui.filePath("ui/base/action.txt"), "[DoNothing]\n[NewExternalAction]\n"));
        QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", request)), QStringLiteral("LIVE_EDITOR_UI_SOURCE_CHANGED"));
        QCOMPARE(f.Fingerprint(), before); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    }
    {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        const auto request = f.DescriptionRequest();
        QVERIFY(WriteFile(f.ui.filePath("ui/config"), "base\r\n"));
        QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", request)), QStringLiteral("LIVE_EDITOR_UI_SOURCE_CHANGED"));
        QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    }
}

void LiveEditorToolsTests::SelectedLayersRemainWritable() {
    Fixture f; QVERIFY2(f.Initialize(true), qPrintable(f.error));
    const auto result = f.tools->Invoke("editor_edit_trigger_graph", f.DescriptionRequest());
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QCOMPARE(f.project.triggerUndoStack_->count(), 1);
}

void LiveEditorToolsTests::GenerationRotatesIdentity() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto original = f.Context();
    f.project.ResetEditTracking();
    const auto next = f.Context();
    QCOMPARE(next.value("expected_revision"), original.value("expected_revision"));
    QVERIFY(next.value("project_identity") != original.value("project_identity"));
    auto request = original; request.insert("table", "triggers");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_query_trigger_graph", request)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
}

void LiveEditorToolsTests::PolicyGatesDiscoveryAndExecution() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto request = f.DescriptionRequest(); int draftCalls = 0;
    f.tools->SetBeforeWrite([&] { ++draftCalls; return true; });
    Mcp::ToolPolicy policy; policy.allowEdit = false; policy.disabledTools.insert("editor_save_map");
    f.tools->SetPolicy(policy);
    const auto names = Names(f.tools->Tools());
    QVERIFY(!names.contains("editor_edit_trigger_graph") && !names.contains("editor_save_map"));
    QVERIFY(f.tools->RegisteredToolNames().contains("editor_edit_trigger_graph") && f.tools->RegisteredToolNames().contains("editor_save_map"));
    QVERIFY(!f.tools->Invoke("editor_edit_trigger_graph", request).value("ok").toBool());
    auto save = f.Context(); save.insert("path", f.outputs.filePath("candidate.w3x"));
    QVERIFY(!f.tools->Invoke("editor_save_map", save).value("ok").toBool());
    QCOMPARE(draftCalls, 0); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    f.tools->SetPolicy({});
    QVERIFY(f.tools->Invoke("editor_edit_trigger_graph", request).value("ok").toBool());
    QCOMPARE(draftCalls, 1);
}

void LiveEditorToolsTests::DraftCommitCannotReenterOrHideRevisionChange() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto request = f.DescriptionRequest(); QJsonObject nested;
    f.tools->SetBeforeWrite([&] {
        nested = f.tools->Invoke("editor_edit_trigger_graph", request);
        return TriggerDocumentEditor::SetTriggerComment(*f.project.triggerDocument_, f.trigger, "界面草稿已经提交").Succeeded()
            && f.project.ObserveEditState("提交真实界面草稿");
    });
    const auto result = f.tools->Invoke("editor_edit_trigger_graph", request);
    QCOMPARE(ErrorCode(nested), QStringLiteral("LIVE_EDITOR_BUSY"));
    QCOMPARE(ErrorCode(result), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(QString::fromStdString(f.project.triggerDocument_->triggers[0].description.text), QStringLiteral("界面草稿已经提交"));
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    f.tools->SetBeforeWrite([] { return false; });
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_edit_trigger_graph", f.DescriptionRequest())), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
    f.tools->SetBeforeWrite({});
    QVERIFY(f.tools->Invoke("editor_edit_trigger_graph", f.DescriptionRequest()).value("ok").toBool());
}

void LiveEditorToolsTests::HostSaveMustProduceActualOutput() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto request = f.Context(); request.insert("path", f.outputs.filePath("candidate.w3x"));
    const auto original = f.Fingerprint();
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", request)), QStringLiteral("LIVE_EDITOR_SAVE_UNAVAILABLE"));
    f.tools->SetSaveHandler([](const std::filesystem::path&, bool) { return false; });
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", request)), QStringLiteral("LIVE_EDITOR_SAVE_REJECTED"));
    // 保存链写进诊断表的错误随拒绝一起返回。
    f.tools->SetSaveHandler([&f](const std::filesystem::path&, bool) {
        f.project.AddDiagnostic(QStringLiteral("警告"), QStringLiteral("地图"), QStringLiteral("SAVE_SAMPLE_NOTE"), QStringLiteral("示例提示"));
        f.project.AddDiagnostic(QStringLiteral("错误"), QStringLiteral("地图"), QStringLiteral("SAVE_SAMPLE_REASON"), QStringLiteral("示例原因"));
        return false;
    });
    const auto explained = f.tools->Invoke("editor_save_map", request);
    const auto reasons = explained.value("diagnostics").toArray();
    QVERIFY2(reasons.size() == 2 && reasons[1].toObject().value("code").toString() == QStringLiteral("SAVE_SAMPLE_REASON")
            && reasons[1].toObject().value("message").toString() == QStringLiteral("示例原因"), qPrintable(Explain(explained)));
    f.tools->SetSaveHandler([](const std::filesystem::path&, bool) { return true; });
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", request)), QStringLiteral("LIVE_EDITOR_SAVE_OUTPUT_MISSING"));
    int calls = 0; QJsonObject nested;
    const auto edit = f.DescriptionRequest();
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    f.tools->SetSaveHandler([&](const std::filesystem::path& path, bool inPlace) {
        ++calls; if (inPlace) return false;
        nested = f.tools->Invoke("editor_edit_trigger_graph", edit);
        // 仅检查宿主委托合同和输出 SHA，不冒称这是地图归档、脚本或游戏验收。
        return WriteFile(QString::fromStdWString(path.wstring()), "synthetic host output, not an archive");
    });
    const auto saved = f.tools->Invoke("editor_save_map", request);
    QVERIFY2(saved.value("ok").toBool(), qPrintable(Explain(saved)));
    QCOMPARE(ErrorCode(nested), QStringLiteral("LIVE_EDITOR_BUSY"));
    const auto sha = Sha256File(std::filesystem::path(request.value("path").toString().toStdWString())); QVERIFY(sha.Succeeded());
    QCOMPARE(saved.value("sha256").toString(), QString::fromStdString(*sha.value));
    QCOMPARE(saved.value("save_evidence").toString(), QStringLiteral("host-common-save-handler"));
    QVERIFY(!saved.value("game_verified").toBool()); QCOMPARE(calls, 1);
    QCOMPARE(ChangeEvents(events), 1);
    const auto fileChange = saved.value("changes").toObject();
    QCOMPARE(fileChange.value("files").toInt(), 1);
    const auto item = fileChange.value("items").toArray()[0].toObject();
    QVERIFY(item.value("before").isNull());
    QCOMPARE(item.value("after").toObject().value("sha256"), saved.value("sha256"));
    QCOMPARE(item.value("target"), saved.value("path"));
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", request)), QStringLiteral("LIVE_EDITOR_SAVE_PATH"));
    request = f.Context(); request.insert("in_place", true);
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", request)), QStringLiteral("LIVE_EDITOR_SAVE_PATH"));
    QCOMPARE(calls, 1); QCOMPARE(f.Fingerprint(), original);
    QVERIFY(f.project.directorySnapshot_->VerifyUnchanged().Succeeded());
    auto validation = f.Context(); validation.insert("path", f.outputs.filePath("validation.w3x"));
    validation.insert("runtime_purpose", "internal_validation");
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", validation)), QStringLiteral("LIVE_EDITOR_SAVE_UNAVAILABLE"));
    int validationCalls = 0;
    f.tools->SetValidationSaveHandler([&](const std::filesystem::path& path) {
        ++validationCalls; return WriteFile(QString::fromStdWString(path.wstring()), "synthetic validation host output, not an archive");
    });
    auto invalidPurpose = validation; invalidPurpose.insert("in_place", true);
    QCOMPARE(ErrorCode(f.tools->Invoke("editor_save_map", invalidPurpose)), QStringLiteral("RUNTIME_INTERNAL_COPY_REQUIRED"));
    const auto copied = f.tools->Invoke("editor_save_map", validation);
    QVERIFY2(copied.value("ok").toBool(), qPrintable(Explain(copied)));
    QVERIFY(!copied.value("project_baseline_changed").toBool()); QCOMPARE(validationCalls, 1); QCOMPARE(calls, 1);
    QCOMPARE(f.Fingerprint(), original); QVERIFY(f.project.directorySnapshot_->VerifyUnchanged().Succeeded());
}

void LiveEditorToolsTests::ActivityReportsRealRecordsAndBoundedExamples() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    auto request = f.DescriptionRequest(QString(4096, QChar(0x7532)));
    events.clear();
    const auto result = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QCOMPARE(events.count(), 3); QCOMPARE(ChangeEvents(events), 1);
    const auto begin = events[0][0].toJsonObject(), end = events[1][0].toJsonObject();
    QCOMPARE(begin.value("type").toString(), QStringLiteral("tool_start"));
    QCOMPARE(end.value("type").toString(), QStringLiteral("tool_end"));
    QCOMPARE(begin.value("id"), end.value("id")); QVERIFY(!begin.value("id").toString().isEmpty());
    QVERIFY(end.value("success").toBool()); QVERIFY(!end.value("is_error").toBool());
    QVERIFY(end.value("elapsed_ms").toInteger(-1) >= 0);
    QCOMPARE(end.value("details").toObject(), result);
    const auto changeEvent = events[2][0].toJsonObject();
    QCOMPARE(changeEvent.value("tool_call_id"), begin.value("id"));
    QCOMPARE(changeEvent.value("trigger_nodes").toInt(), 1);
    const auto changes = result.value("changes").toObject();
    QCOMPARE(changes.value("changed_records").toInt(), 1); QCOMPARE(changes.value("trigger_nodes").toInt(), 1);
    QVERIFY(!changes.contains("files") && !changes.contains("added_lines") && !changes.contains("removed_lines"));
    const auto after = changes.value("examples").toArray()[0].toObject().value("after").toObject();
    const auto text = after.value("description").toObject();
    QVERIFY(text.value("truncated").toBool());
    QCOMPARE(text.value("bytes").toString(), QStringLiteral("12288"));
    QCOMPARE(text.value("sha256").toString().size(), qsizetype{64});
    const auto item = changeEvent.value("items").toArray()[0].toObject();
    QCOMPARE(item.value("kind").toString(), QStringLiteral("trigger"));
    QCOMPARE(item.value("object_id").toString(), QString::number(f.trigger));
    QCOMPARE(item.value("before").toObject().value("description").toString(), QStringLiteral("原始说明"));
    QCOMPARE(item.value("after").toObject(), after);
    QVERIFY(QJsonDocument(changes.value("examples").toArray()).toJson(QJsonDocument::Compact).size() < 64 * 1024);
    events.clear();
    const auto rejected = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY(!rejected.value("ok").toBool()); QCOMPARE(events.count(), 2);
    QCOMPARE(ChangeEvents(events), 0);
    QVERIFY(events[1][0].toJsonObject().value("is_error").toBool());
}

void LiveEditorToolsTests::ChildChangesNavigateActualOwningTrigger() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const QJsonObject parent{{"kind", "trigger_ecas"},
        {"owner", QJsonObject{{"kind", "trigger"}, {"id", QString::number(f.trigger)}}}, {"ordered_children", QJsonArray{}}};
    const QJsonObject root{{"function", "StoreInteger"}, {"kind", "action"}, {"enabled", true},
        {"arguments", QJsonArray{QJsonObject{{"scalar", QJsonObject{{"kind", 3}, {"value", "7"}}}}}}, {"children", QJsonArray{}}};
    const auto request = f.EditRequest({QJsonObject{{"op", "insert_subtree"}, {"parent", parent}, {"root", root}}});
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    const auto result = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QCOMPARE(ChangeEvents(events), 1);
    const auto changes = result.value("changes").toObject();
    QCOMPARE(changes.value("trigger_nodes").toInt(), 3);
    QCOMPARE(changes.value("added_records").toInt(), 2);
    QCOMPARE(changes.value("changed_records").toInt(), 1);
    int children = 0;
    for (const auto& value : changes.value("items").toArray()) {
        const auto item = value.toObject();
        const auto kind = item.value("record_kind").toString();
        if (kind != QStringLiteral("eca") && kind != QStringLiteral("argument")) continue;
        ++children;
        QCOMPARE(item.value("kind").toString(), QStringLiteral("trigger"));
        QCOMPARE(item.value("object_id").toString(), QString::number(f.trigger));
        QCOMPARE(item.value("source_trigger_id").toString(), QString::number(f.trigger));
        QVERIFY(item.value("record_id").toString() != item.value("object_id").toString());
        QVERIFY(item.value("before").isNull()); QVERIFY(item.value("after").isObject());
        const auto expected = kind == QStringLiteral("eca") ? f.project.triggerDocument_->ecaNodes[0].id : f.project.triggerDocument_->argumentNodes[0].id;
        QCOMPARE(item.value("record_id").toString(), QString::number(expected));
    }
    QCOMPARE(children, 2);
    QVERIFY(!changes.contains("files"));
}

void LiveEditorToolsTests::ObjectToolsShareSchemasAndUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallObjects(), qPrintable(f.error));
    QVERIFY(f.project.editTrackingReady_ && !f.project.editTrackingFailed_);
    for (const auto& definition : Mcp::ObjectToolDefinitions()) {
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject();
            if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QVERIFY2(found, qPrintable(definition.name));
    }
    const auto type = ObjectData::ObjectType::Unit;
    auto* document = f.project.objectDocuments_[0].get();
    const auto original = document->DocumentFingerprint();
    Services::ObjectCommandContext context; context.type = type; context.document = document;
    context.standard = f.project.standardCatalog_.get(); context.schema = f.project.objectFieldSchema_.get();
    context.documents[0] = document;
    const std::array uiChanges{Services::ObjectFieldChange{"unam", {}, {}, std::string("界面先改名称")}};
    const auto ui = Services::PlanObjectFieldChanges(context, "u001", uiChanges); QVERIFY(ui.Succeeded());
    f.project.objectUndoStacks_[0]->push(new QtProject::ObjectSnapshotCommand(ui.value->before, ui.value->after,
        [&](const ObjectData::LniObjectDocument& snapshot) { *document = snapshot; f.project.ObserveObjectEdit(0); }, QStringLiteral("已有界面编辑")));
    const auto afterUi = document->DocumentFingerprint();
    auto request = f.Context(); request.insert("kind", "unit"); request.insert("id", "u001");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 250}},
        QJsonObject{{"field", "uabi"}, {"value", QJsonArray{"A001", "A001"}}}});
    const auto stale = request;
    QSignalSpy changed(f.tools.get(), &LiveEditorTools::Changed);
    const auto edited = f.tools->Invoke("objects_set_fields", request);
    QVERIFY2(edited.value("ok").toBool(), qPrintable(Explain(edited)));
    QCOMPARE(f.project.objectDocuments_[0].get(), document); QCOMPARE(f.project.objectUndoStacks_[0]->count(), 2);
    QVERIFY(edited.value("revision") != stale.value("expected_revision")); QVERIFY(!edited.value("saved").toBool());
    const auto afterMcp = document->DocumentFingerprint();
    f.project.objectUndoStacks_[0]->undo(); QCOMPARE(document->DocumentFingerprint(), afterUi);
    f.project.objectUndoStacks_[0]->undo(); QCOMPARE(document->DocumentFingerprint(), original);
    f.project.objectUndoStacks_[0]->redo(); QCOMPARE(document->DocumentFingerprint(), afterUi);
    f.project.objectUndoStacks_[0]->redo(); QCOMPARE(document->DocumentFingerprint(), afterMcp);
    QVERIFY(changed.count() > 0);
    const auto rejected = f.tools->Invoke("objects_set_fields", stale);
    QCOMPARE(ErrorCode(rejected), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(document->DocumentFingerprint(), afterMcp);
}

void LiveEditorToolsTests::MissingObjectsCreateUndoAndPolicy() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallObjects(true), qPrintable(f.error));
    QVERIFY(f.project.objectDocuments_[0] == nullptr);
    auto request = f.Context(); request.insert("kind", "unit"); request.insert("base_id", "u001"); request.insert("new_id", "U001"); request.insert("name", "新建测试对象");
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    const auto rejected = f.tools->Invoke("objects_create", request);
    QVERIFY(!rejected.value("ok").toBool()); QVERIFY(f.project.objectDocuments_[0] == nullptr);
    QCOMPARE(f.project.objectUndoStacks_[0]->count(), 0);
    QVERIFY(f.tools->Invoke("objects_list", QJsonObject{{"kind", "unit"}}).value("ok").toBool());
    f.tools->SetPolicy({});
    const auto created = f.tools->Invoke("objects_create", request);
    QVERIFY2(created.value("ok").toBool(), qPrintable(Explain(created)));
    auto* document = f.project.objectDocuments_[0].get(); QVERIFY(document != nullptr);
    QVERIFY(document->FindObject({ObjectData::ObjectType::Unit, "U001"}) != nullptr);
    QVERIFY(f.project.nativeObjectDocuments_[0] != nullptr); QVERIFY(f.project.objectDirty_[0]);
    QCOMPARE(f.project.objectUndoStacks_[0]->count(), 1);
    f.project.objectUndoStacks_[0]->undo(); QVERIFY(document->Objects().empty()); QVERIFY(!f.project.objectDirty_[0]);
    f.project.objectUndoStacks_[0]->redo(); QVERIFY(document->FindObject({ObjectData::ObjectType::Unit, "U001"}) != nullptr);
    request = f.Context(); request.insert("kind", "unit"); request.insert("id", "U001");
    request.insert("changes", QJsonArray{QJsonObject{{"field", "uhpm"}, {"value", 300}}, QJsonObject{{"field", "uabi"}, {"value", QJsonArray{"A999"}}}});
    const auto before = document->DocumentFingerprint();
    const auto invalid = f.tools->Invoke("objects_set_fields", request);
    QVERIFY(!invalid.value("ok").toBool()); QCOMPARE(document->DocumentFingerprint(), before);
    QCOMPARE(f.project.objectUndoStacks_[0]->count(), 1);
    request = f.Context(); request.insert("kind", "unit"); request.insert("id", "U001");
    const auto deleted = f.tools->Invoke("objects_delete", request);
    QVERIFY(!deleted.value("ok").toBool()); QVERIFY(deleted.contains("reference_audit"));
    QCOMPARE(document->DocumentFingerprint(), before);
}

void LiveEditorToolsTests::VariableToolsShareSchemasAndOriginalUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto* document = f.project.triggerDocument_.get();
    const auto original = f.Fingerprint();
    const auto codeBytes = document->wct.customCode.ToBytes(); QVERIFY(codeBytes.Succeeded());
    for (const auto& definition : Mcp::VariableToolDefinitions()) {
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject();
            if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QVERIFY2(found, qPrintable(definition.name));
    }
    const auto earlier = f.tools->Invoke("editor_edit_trigger_graph", f.DescriptionRequest());
    QVERIFY2(earlier.value("ok").toBool(), qPrintable(Explain(earlier)));
    const auto afterEarlier = f.Fingerprint();
    auto request = f.Context();
    request.insert("name", "night_counter"); request.insert("type_name", "integer");
    request.insert("initialized", true); request.insert("initial_value", "42");
    QSignalSpy activity(f.tools.get(), &LiveEditorTools::Activity);
    const auto created = f.tools->Invoke("variables_create", request);
    QVERIFY2(created.value("ok").toBool(), qPrintable(Explain(created)));
    QCOMPARE(document->variables.size(), std::size_t{1});
    const auto id = document->variables[0].id;
    QCOMPARE(document->variables[0].name.text, std::string("night_counter"));
    QCOMPARE(document->variables[0].value.text, std::string("42"));
    QCOMPARE(f.project.triggerDocument_.get(), document);
    QCOMPARE(f.project.triggerUndoStack_->count(), 2);
    QCOMPARE(ChangeEvents(activity), 1);
    QVERIFY(created.value("changes").toObject().value("items").toArray().size() == 1);
    request = f.Context(); request.insert("id", QString::number(id));
    request.insert("changes", QJsonObject{{"initial_value", "73"}});
    const auto updated = f.tools->Invoke("variables_update", request);
    QVERIFY2(updated.value("ok").toBool(), qPrintable(Explain(updated)));
    QCOMPARE(document->variables[0].value.text, std::string("73"));
    QCOMPARE(f.project.triggerUndoStack_->count(), 3);
    const auto afterUpdate = f.Fingerprint();
    request = f.Context(); request.insert("id", QString::number(id));
    request.insert("changes", QJsonObject{{"initial_value", "73"}});
    const auto unchanged = f.tools->Invoke("variables_update", request);
    QVERIFY2(unchanged.value("ok").toBool(), qPrintable(Explain(unchanged)));
    QCOMPARE(f.project.triggerUndoStack_->count(), 3); QCOMPARE(f.Fingerprint(), afterUpdate);
    f.project.triggerUndoStack_->undo(); QCOMPARE(document->variables[0].value.text, std::string("42"));
    f.project.triggerUndoStack_->undo(); QVERIFY(document->variables.empty()); QCOMPARE(f.Fingerprint(), afterEarlier);
    f.project.triggerUndoStack_->undo(); QCOMPARE(f.Fingerprint(), original);
    f.project.triggerUndoStack_->redo(); f.project.triggerUndoStack_->redo(); f.project.triggerUndoStack_->redo();
    QCOMPARE(f.Fingerprint(), afterUpdate); QCOMPARE(document->variables[0].id, id);
    const auto unchangedCode = document->wct.customCode.ToBytes(); QVERIFY(unchangedCode.Succeeded());
    QVERIFY(*unchangedCode.value == *codeBytes.value);
    request = f.Context(); request.insert("table", "variables"); request.insert("variable_ids", QJsonArray{QString::number(id)});
    const auto exact = f.tools->Invoke("editor_query_trigger_graph", request);
    QVERIFY2(exact.value("ok").toBool(), qPrintable(Explain(exact)));
    QCOMPARE(exact.value("rows").toArray().size(), qsizetype{1});
}

void LiveEditorToolsTests::VariableFailuresPreserveRevisionAndUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto request = f.Context(); request.insert("name", "night_counter"); request.insert("type_name", "integer");
    const auto original = f.Fingerprint();
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    QVERIFY(!f.tools->Invoke("variables_create", request).value("ok").toBool());
    QCOMPARE(f.Fingerprint(), original); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    QVERIFY(f.tools->Invoke("variables_list", {}).value("ok").toBool());
    f.tools->SetPolicy({});
    const auto created = f.tools->Invoke("variables_create", request);
    QVERIFY2(created.value("ok").toBool(), qPrintable(Explain(created)));
    const auto id = f.project.triggerDocument_->variables[0].id;
    const auto after = f.Fingerprint(); const auto context = f.Context();
    auto stale = request; stale.remove("name"); stale.remove("type_name");
    stale.insert("id", QString::number(id)); stale.insert("changes", QJsonObject{{"initialized", true}, {"initial_value", "17"}});
    QCOMPARE(ErrorCode(f.tools->Invoke("variables_update", stale)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    auto invalid = context; invalid.insert("id", QString::number(id));
    invalid.insert("changes", QJsonObject{{"is_array", true}, {"array_size", 8193}});
    QVERIFY(!f.tools->Invoke("variables_update", invalid).value("ok").toBool());
    auto erase = context; erase.insert("id", QString::number(id));
    QVERIFY(!f.tools->Invoke("variables_delete", erase).value("ok").toBool()); // 原作者不透明代码仍可能引用。
    auto rename = erase; rename.insert("name", "night_renamed");
    QVERIFY(!f.tools->Invoke("variables_rename", rename).value("ok").toBool());
    QCOMPARE(f.Fingerprint(), after); QCOMPARE(f.Context(), context); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
}

void LiveEditorToolsTests::AuxiliaryCrudKeepsOriginalUndoAndProjectRoundtrip() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallAuxiliary(), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    auto* document = f.project.mapDocument_.get(); const auto triggers = f.Fingerprint();
    std::vector<MapDocument> steps{session->Snapshot()};
    auto settings = *steps.front().Settings(); settings.author = "Earlier GUI author";
    QVERIFY(session->EditSettings(settings).Succeeded());
    QtProject::PushMapDataEdit(f.project, "作者已有地图命令", "作者先改作者名");
    (void)f.project.ObserveMapDataEdit("作者先改作者名");
    steps.push_back(session->Snapshot());
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    for (const auto kind : {MapDataKind::Regions, MapDataKind::Cameras}) {
        const bool region = kind == MapDataKind::Regions;
        QJsonObject previous;
        const std::array<QString, 4> tools{"auxiliary_create", "auxiliary_update", "auxiliary_copy", "auxiliary_delete"};
        for (std::size_t operation = 0; operation < tools.size(); ++operation) {
            auto request = f.AuxiliaryRequest(kind, operation == 0 ? std::nullopt : std::optional<std::size_t>{0});
            if (operation == 0) request.insert("fields", region ? RegionFields("First", 1) : CameraFields("View"));
            if (operation == 1) {
                // 后续请求直接消费上次真实回执中的定位和成员身份。
                auto locator = previous.value("result_identity").toObject();
                request.insert("expected_member_sha256", locator.take("member_sha256")); request.insert("source", locator);
                request.insert("fields", region ? QJsonObject{{"name", "Renamed"}, {"top", 900}}
                    : QJsonObject{{"name", "Renamed View"}, {"rotation", 45}});
            }
            if (operation == 2) request.insert("fields", region ? QJsonObject{{"name", "Second"}, {"creation_number", 2}}
                : QJsonObject{{"name", "Second View"}});
            const auto count = undo->count(); const auto revision = session->Revision();
            events.clear();
            // 同时核最终 MCP tool_result，防止宿主已发布却返回错误封装或另一条命令回执。
            const auto rpc = f.tools->Server().HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", "auxiliary-live"},
                {"method", "tools/call"}, {"params", QJsonObject{{"name", tools[operation]}, {"arguments", request}}}}).toJson(QJsonDocument::Compact));
            QVERIFY(rpc && !rpc->error);
            const auto envelope = rpc->result.toObject();
            QVERIFY(envelope.contains("isError") && !envelope.value("isError").toBool());
            const auto content = envelope.value("content").toArray(); QCOMPARE(content.size(), qsizetype{1});
            QCOMPARE(content[0].toObject().value("type").toString(), QStringLiteral("text"));
            const auto payload = QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8());
            QVERIFY(payload.isObject()); const auto result = payload.object();
            QVERIFY2(result.value("ok").toBool() && result.value("applied").toBool(), qPrintable(Explain(result)));
            QCOMPARE(result.value("session_identity").toString(), QString::number(session->SessionIdentity()));
            QCOMPARE(result.value("command_identity").toString(), QString::number(session->LastEdit().CommandIdentity()));
            QVERIFY(session->LastEdit().CommandIdentity() != 0);
            QCOMPARE(session->Revision(), revision + 1); QCOMPARE(undo->count(), count + 1);
            QCOMPARE(events.size(), qsizetype{2});
            const auto toolResult = events[1][0].toJsonObject();
            QCOMPARE(toolResult.value("type").toString(), QStringLiteral("tool_end"));
            QVERIFY(toolResult.value("success").toBool()); QCOMPARE(toolResult.value("details").toObject(), result);
            QVERIFY(!result.value("saved").toBool() && !result.value("runtime_verified").toBool());
            steps.push_back(session->Snapshot()); previous = result;
        }
        auto noOp = f.AuxiliaryRequest(kind, 0);
        noOp.insert("fields", QJsonObject{{"name", region ? "Second" : "Second View"}});
        const auto revision = session->Revision(); const auto count = undo->count(); const auto context = f.Context();
        const auto result = f.tools->Invoke("auxiliary_update", noOp);
        QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
        QVERIFY(!result.value("changed").toBool() && !result.value("applied").toBool());
        QCOMPARE(undo->count(), count); QCOMPARE(session->Revision(), revision); QCOMPARE(f.Context(), context);
    }
    const auto final = session->Snapshot();
    const auto target = std::filesystem::path(f.outputs.filePath("auxiliary.forge").toStdWString());
    const auto saved = f.project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "save auxiliary project" : saved.diagnostics.front().message.c_str());
    auto restored = QtProject::PrepareProjectOpen(target);
    QVERIFY2(restored.Succeeded(), restored.diagnostics.empty() ? "reopen auxiliary project" : restored.diagnostics.front().message.c_str());
    auto* restoredEdits = (*restored.value)->mapDataEdits.get(); QVERIFY(restoredEdits);
    const auto reopened = restoredEdits->Snapshot();
    for (const auto kind : {MapDataKind::Regions, MapDataKind::Cameras}) {
        QCOMPARE(reopened.Member(kind).state, MapDataState::Ready);
        QCOMPARE(reopened.Member(kind).sources.front().data, final.Member(kind).sources.front().data);
    }
    // .forge 只证明工程保存恢复；源图成员仍缺席，不代替 w3x、原版编辑器或游戏验收。
    QVERIFY(!QFileInfo::exists(f.source.filePath("map/war3map.w3r")));
    QVERIFY(!QFileInfo::exists(f.source.filePath("map/war3map.w3c")));
    for (std::size_t i = steps.size() - 1; i > 0; --i) {
        undo->undo(); QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(session->Snapshot(), steps[i - 1]));
    }
    QCOMPARE(session->Snapshot().Member(MapDataKind::Regions).state, MapDataState::Missing);
    QCOMPARE(session->Snapshot().Member(MapDataKind::Cameras).state, MapDataState::Missing);
    for (std::size_t i = 1; i < steps.size(); ++i) {
        undo->redo(); QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(session->Snapshot(), steps[i]));
    }
    QCOMPARE(undo->command(0)->text(), QStringLiteral("作者已有地图命令"));
    QCOMPARE(f.project.mapDocument_.get(), document); QCOMPARE(f.project.mapEdits_.get(), session);
    QCOMPARE(f.project.mapDataUndoStack_, undo); QCOMPARE(f.Fingerprint(), triggers);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::AuxiliaryPartialFieldsAndPrepublishRejection() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallAuxiliary(true), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    const auto before = session->Snapshot(); const auto triggers = f.Fingerprint();
    auto region = f.AuxiliaryRequest(MapDataKind::Regions, 0); region.insert("fields", QJsonObject{{"top", 900}});
    const auto edited = f.tools->Invoke("auxiliary_update", region);
    QVERIFY2(edited.value("applied").toBool(), qPrintable(Explain(edited)));
    auto camera = f.AuxiliaryRequest(MapDataKind::Cameras, 0); camera.insert("fields", QJsonObject{{"rotation", 45}});
    const auto changed = f.tools->Invoke("auxiliary_update", camera);
    QVERIFY2(changed.value("applied").toBool(), qPrintable(Explain(changed)));
    const auto after = session->Snapshot();
    QCOMPARE(after.Regions()->entries.front().colorBgra[3], std::uint8_t{177});
    QCOMPARE(std::bit_cast<std::uint32_t>(after.Cameras()->entries.front().nearClippingPlane), 0x80000000u);
    QCOMPARE(std::bit_cast<std::uint32_t>(after.Cameras()->entries.front().targetY), 0x3f800001u);
    QCOMPARE(undo->count(), 2);
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(f.project); QVERIFY(binding.Succeeded());
    Services::AuxiliaryEditRequest request;
    request.kind = MapDataKind::Regions; request.action = Services::AuxiliaryEditAction::Update;
    request.expectedMemberSha256 = *Services::AuxiliaryEditing::MemberIdentity(after, request.kind).value;
    request.expectedRowKey = *MapData::MapFieldRowKey(after, request.kind, 0).value;
    request.record = after.Regions()->entries.front(); std::get<MapData::RegionRecord>(request.record).top = 1000;
    const auto revision = session->Revision(); const auto context = f.Context();
    int validations = 0;
    const auto refused = QtProject::ApplyAuxiliaryEditWithResult(f.project, *binding.value, request, "拒绝发布",
        [&](const auto&, const auto&, const auto& receipt) {
            ++validations;
            if (receipt.SessionIdentity() != session->SessionIdentity() || !receipt.CommandIdentity())
                return Status::Failure("Test", "BAD_RECEIPT", "预发布必须提供原事务回执");
            return Status::Failure("Test", "AUX_TEST_DENIED", "预发布拒绝");
        });
    QVERIFY(!refused.Succeeded()); QCOMPARE(validations, 1);
    QVERIFY(std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(), [](const auto& d) { return d.code == "AUX_TEST_DENIED"; }));
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(session->Snapshot(), after));
    QCOMPARE(session->Revision(), revision); QCOMPARE(f.Context(), context); QCOMPARE(undo->count(), 2);
    undo->undo(); undo->undo(); QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(session->Snapshot(), before));
    undo->redo(); undo->redo(); QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(session->Snapshot(), after));
    QCOMPARE(f.Fingerprint(), triggers); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::AuxiliaryPreviewAndReferenceFailuresAreAtomic() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallAuxiliary(), qPrintable(f.error));
    const auto before = f.project.mapEdits_->Snapshot(); const auto context = f.Context(); int drafts = 0;
    const auto initialRevision = f.project.mapEdits_->Revision();
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    f.tools->SetBeforeWrite([&] { ++drafts; return true; });
    for (const auto kind : {MapDataKind::Regions, MapDataKind::Cameras}) {
        auto create = f.AuxiliaryRequest(kind);
        create.insert("fields", kind == MapDataKind::Regions ? RegionFields("First", 1) : CameraFields("View"));
        create.insert("dry_run", true);
        const auto preview = f.tools->Invoke("auxiliary_create", create);
        QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
        QVERIFY(preview.contains("candidate_identity") && !preview.contains("result_identity"));
        QVERIFY(!preview.value("applied").toBool()); QCOMPARE(drafts, 0);
        QCOMPARE(f.Context(), context); QCOMPARE(f.project.mapDataUndoStack_->count(), 0);
        QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(f.project.mapEdits_->Snapshot(), before));
        create.insert("dry_run", "true");
        // 共用 JSON-RPC schema 在 handler/BeforeWrite 之前拒绝类型；业务层的
        // AUX_ARGUMENT_TYPE 仅适用于直接 facade 调用，不能冒充外层 -32602 合同。
        events.clear();
        const auto invalidRpc = f.tools->Server().HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
            {"id", "auxiliary-invalid-type"}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "auxiliary_create"}, {"arguments", create}}}}).toJson(QJsonDocument::Compact));
        QVERIFY(invalidRpc && invalidRpc->error);
        QCOMPARE(invalidRpc->id.toString(), QStringLiteral("auxiliary-invalid-type"));
        QCOMPARE(invalidRpc->error->value("code").toInt(), Mcp::JsonRpcResponse::kInvalidParams);
        QCOMPARE(invalidRpc->error->value("message").toString(), QStringLiteral("arguments.dry_run must be a boolean"));
        QVERIFY(invalidRpc->result.isNull());
        const auto mapped = f.tools->Invoke("auxiliary_create", create);
        QCOMPARE(ErrorCode(mapped), QStringLiteral("LIVE_EDITOR_RPC_ERROR"));
        QCOMPARE(mapped.value("diagnostics").toArray()[0].toObject().value("message").toString(),
            QStringLiteral("arguments.dry_run must be a boolean"));
        QCOMPARE(events.count(), 0); QCOMPARE(drafts, 0);
        QCOMPARE(f.Context(), context); QCOMPARE(f.project.mapEdits_->Revision(), initialRevision);
        QCOMPARE(f.project.mapDataUndoStack_->count(), 0); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
        QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(f.project.mapEdits_->Snapshot(), before));
    }
    auto create = f.AuxiliaryRequest(MapDataKind::Regions); create.insert("fields", RegionFields("First", 1));
    f.tools->SetBeforeWrite([] { return false; });
    QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_create", create)), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
    f.tools->SetBeforeWrite({});
    QVERIFY(f.tools->Invoke("auxiliary_create", create).value("applied").toBool());
    const auto after = f.project.mapEdits_->Snapshot(); const auto revision = f.project.mapEdits_->Revision();
    const auto active = f.Context();
    for (const bool dry : {true, false}) {
        auto invalid = f.AuxiliaryRequest(MapDataKind::Regions, 0); invalid.insert("dry_run", dry);
        invalid.insert("fields", QJsonObject{{"left", 1000}});
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_update", invalid)), QStringLiteral("REGION_BOUNDS_INVALID"));
        invalid.insert("fields", QJsonObject{{"top", 900}});
        invalid.insert("expected_member_sha256", QString(64, QChar('a')));
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_update", invalid)), QStringLiteral("AUX_ROW_STALE"));
        invalid = f.AuxiliaryRequest(MapDataKind::Regions, 0); invalid.insert("dry_run", dry);
        invalid.insert("fields", QJsonObject{{"top", 900}});
        auto locator = invalid.value("source").toObject(); locator.insert("row_key", "old-row"); invalid.insert("source", locator);
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_update", invalid)), QStringLiteral("AUX_ROW_STALE"));
        auto stale = create; stale.insert("dry_run", dry);
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_create", stale)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
        auto conflict = f.AuxiliaryRequest(MapDataKind::Regions); conflict.insert("dry_run", dry);
        conflict.insert("fields", RegionFields("First", 2));
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_create", conflict)), QStringLiteral("AUX_SYMBOL_DUPLICATE"));
    }
    QCOMPARE(f.Context(), active); QCOMPARE(f.project.mapEdits_->Revision(), revision);
    QCOMPARE(f.project.mapDataUndoStack_->count(), 1);
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(f.project.mapEdits_->Snapshot(), after));
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    for (const auto& definition : Mcp::AuxiliaryToolDefinitions()) {
        QVERIFY(!Names(f.tools->Tools()).contains(definition.name));
        QVERIFY(!f.tools->Invoke(definition.name, create).value("ok").toBool());
    }
    Fixture linked; QVERIFY2(linked.Initialize(), qPrintable(linked.error));
    QVERIFY2(linked.InstallAuxiliary(true, true), qPrintable(linked.error));
    const auto linkedBefore = linked.project.mapEdits_->Snapshot(); const auto t = linked.Fingerprint();
    for (const auto kind : {MapDataKind::Regions, MapDataKind::Cameras}) for (const bool dry : {true, false}) {
        auto remove = linked.AuxiliaryRequest(kind, 0); remove.insert("dry_run", dry);
        QCOMPARE(ErrorCode(linked.tools->Invoke("auxiliary_delete", remove)), QStringLiteral("AUX_REFERENCED"));
        remove.insert("fields", QJsonObject{{"name", "New Name"}});
        QCOMPARE(ErrorCode(linked.tools->Invoke("auxiliary_update", remove)), QStringLiteral("AUX_REFERENCED"));
    }
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(linked.project.mapEdits_->Snapshot(), linkedBefore));
    QCOMPARE(linked.project.mapDataUndoStack_->count(), 0); QCOMPARE(linked.Fingerprint(), t);
    linked.project.triggerDocument_.reset();
    for (const bool dry : {true, false}) {
        auto remove = linked.AuxiliaryRequest(MapDataKind::Cameras, 0); remove.insert("dry_run", dry);
        QCOMPARE(ErrorCode(linked.tools->Invoke("auxiliary_delete", remove)), QStringLiteral("AUX_REFERENCE_COVERAGE"));
    }
    QCOMPARE(linked.project.mapDataUndoStack_->count(), 0);
}

void LiveEditorToolsTests::AuxiliaryReentryProjectReplacementAndSourceDrift() {
    Fixture f, other;
    QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallAuxiliary(), qPrintable(f.error));
    QVERIFY2(other.Initialize(), qPrintable(other.error)); QVERIFY2(other.InstallAuxiliary(), qPrintable(other.error));
    const auto before = f.project.mapEdits_->Snapshot(); const auto oldSession = f.project.mapEdits_->SessionIdentity();
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(before, other.project.mapEdits_->Snapshot()));
    QCOMPARE(f.Fingerprint(), other.Fingerprint());
    auto request = f.AuxiliaryRequest(MapDataKind::Regions); request.insert("fields", RegionFields("First", 1));
    QJsonObject nested;
    f.tools->SetBeforeWrite([&] {
        nested = f.tools->Invoke("auxiliary_create", request);
        f.project.projectRoot_ = other.project.projectRoot_;
        f.project.directorySnapshot_ = std::move(other.project.directorySnapshot_);
        f.project.mapDocument_ = std::make_unique<MapDocument>(*other.project.mapDocument_);
        f.project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*f.project.mapDocument_);
        f.project.triggerDocument_ = std::make_unique<TriggerDocument>(*other.project.triggerDocument_);
        f.project.ResetEditTracking();
        return true;
    });
    const auto result = f.tools->Invoke("auxiliary_create", request);
    QCOMPARE(ErrorCode(nested), QStringLiteral("LIVE_EDITOR_BUSY"));
    QCOMPARE(ErrorCode(result), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QVERIFY(f.project.mapEdits_->SessionIdentity() != oldSession); QCOMPARE(f.project.mapDataUndoStack_->count(), 0);
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(f.project.mapEdits_->Snapshot(), before));
    f.tools->SetBeforeWrite({});
    auto current = f.AuxiliaryRequest(MapDataKind::Cameras); current.insert("fields", CameraFields("View"));
    f.project.savingMap_ = true;
    for (const bool dry : {true, false}) {
        current.insert("dry_run", dry);
        QCOMPARE(ErrorCode(f.tools->Invoke("auxiliary_create", current)), QStringLiteral("LIVE_EDITOR_BUSY"));
    }
    f.project.savingMap_ = false;
    const auto stableContext = f.Context();
    const auto path = QString::fromStdWString((f.project.projectRoot_ / "map/war3map.w3i").wstring());
    QVERIFY(WriteFile(path, Bytes(MapDataFixtures::FullMapSettingsV25().bytes) + QByteArray("changed")));
    for (const bool dry : {true, false}) {
        current.insert("dry_run", dry);
        QVERIFY(!f.tools->Invoke("auxiliary_create", current).value("ok").toBool());
        QVERIFY(f.project.sourceChangeConfirmed_);
    }
    QCOMPARE(f.Context(), stableContext); QCOMPARE(f.project.mapDataUndoStack_->count(), 0);
    QVERIFY(Services::AuxiliaryEditing::AreEquivalentMapSources(f.project.mapEdits_->Snapshot(), before));
}

void LiveEditorToolsTests::BoundaryDraftSavesAndReopensAsProject_data() {
    QTest::addColumn<bool>("pathing");
    QTest::newRow("boundary") << false;
    QTest::newRow("pathing-restriction") << true;
}

void LiveEditorToolsTests::BoundaryDraftSavesAndReopensAsProject() {
    QFETCH(bool, pathing);
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    MapData::TerrainGridSnapshot grid;
    grid.header.width = 5; grid.header.height = 5; grid.header.offset = {-256, -256}; grid.header.mainTileset = 'L';
    grid.header.groundTilesets = {"Ldrt"}; grid.header.cliffTilesets = {"CLdi"}; grid.vertices.resize(25);
    for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
    MapData::RefreshTerrainDerived(grid, 0, 0, 5, 5);
    const auto terrain = MapData::SerializeMapTerrain(grid); QVERIFY(terrain.Succeeded());
    std::vector<std::byte> sourcePathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 16u, 16u}) for (unsigned shift = 0; shift < 32; shift += 8)
        sourcePathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    sourcePathing.resize(16 + 16 * 16, std::byte{2});
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(*terrain.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wpm"), Bytes(sourcePathing)));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    const auto state = f.tools->Invoke("get_terrain_edit_state", {}); QVERIFY(state.value("ok").toBool());
    auto request = f.Context(); request.insert("terrain_binding", state.value("terrain_binding"));
    request.insert("dry_run", false);
    request.insert("brush", pathing
        ? QJsonObject{{"kind", "pathing_restriction"}, {"blocked_type", "fly"}, {"size", 1}, {"value", true}}
        : QJsonObject{{"kind", "boundary"}, {"size", 1}, {"value", true}});
    request.insert("points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}});
    const auto applied = f.tools->Invoke("edit_terrain_stroke", request);
    QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
    QCOMPARE(applied.value("terrain_pathing_gate").toObject().value("passed").toBool(), pathing);
    QCOMPARE(f.project.mapDataUndoStack_->count(), 1);
    const auto changed = MapData::SerializeMapTerrain(*f.project.mapEdits_->Terrain()); QVERIFY(changed.Succeeded());
    const auto target = std::filesystem::path(f.outputs.filePath("boundary.forge").toStdWString());
    const auto saved = f.project.SaveProjectToPath(target);
    QVERIFY2(saved.Succeeded(), saved.diagnostics.empty() ? "save boundary project" : saved.diagnostics.front().message.c_str());
    auto restored = QtProject::PrepareProjectOpen(target);
    QVERIFY2(restored.Succeeded(), restored.diagnostics.empty() ? "reopen boundary project" : restored.diagnostics.front().message.c_str());
    auto* edits = (*restored.value)->mapDataEdits.get(); QVERIFY(edits && edits->Terrain());
    QCOMPARE(*MapData::SerializeMapTerrain(*edits->Terrain()).value, *changed.value);
    QCOMPARE(edits->InspectTerrainPoint({0, 0}).value->pathingFlags,
        std::optional<std::uint8_t>{static_cast<std::uint8_t>(pathing ? 6 : 2)});
    QCOMPARE(edits->VerifyTerrainPathingForSave().Succeeded(), pathing);
    QFile source(f.source.filePath("map/war3map.w3e")); QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), Bytes(*terrain.value));
    QFile originalPathing(f.source.filePath("map/war3map.wpm")); QVERIFY(originalPathing.open(QIODevice::ReadOnly));
    QCOMPARE(originalPathing.readAll(), Bytes(sourcePathing));
    if (pathing) {
        auto package = MapArchiveSession::Open(target); QVERIFY(package.Succeeded()); QVERIFY(!package.value->HasOpaqueMembers());
        for (int scenario = 0; scenario < 3; ++scenario) {
            std::vector<MapArchiveMember> members;
            for (const auto& item : package.value->Members()) {
                if (scenario != 2 && item.name == "overlay/editor/pathing-restrictions.bin") continue;
                auto bytes = package.value->ReadMember(item.name); QVERIFY(bytes.Succeeded());
                if (item.name == "project.json" && scenario != 0) {
                    auto manifest = QJsonDocument::fromJson(Bytes(*bytes.value)).object();
                    if (scenario == 1) manifest.remove("pathingRestrictions");
                    else manifest["pathingRestrictions"] = 3;
                    const auto json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
                    bytes.value->assign(reinterpret_cast<const std::byte*>(json.constData()),
                        reinterpret_cast<const std::byte*>(json.constData()) + json.size());
                }
                members.push_back({item.name, std::move(*bytes.value)});
            }
            const auto broken = std::filesystem::path(f.outputs.filePath(QStringLiteral("unbound-%1.forge").arg(scenario)).toStdWString());
            QVERIFY(ProjectStore::Save(broken, members).Succeeded());
            QVERIFY(!QtProject::PrepareProjectOpen(broken).Succeeded());
        }
        MapData::TerrainBrush clear; clear.kind = MapData::TerrainBrushKind::PathingRestriction;
        clear.pathingRestrictions = 0x0e; clear.value = false; clear.size = 1;
        const MapData::TerrainStrokePoint point{0, 0};
        QVERIFY(edits->EditTerrain(clear, std::span(&point, 1)).Succeeded());
        QCOMPARE(edits->InspectTerrainPoint(point).value->pathingFlags, std::optional<std::uint8_t>{2});
    }
}

void LiveEditorToolsTests::TerrainToolsKeepSessionPreviewAndOriginalUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    MapData::TerrainGridSnapshot grid;
    grid.header.width = 5; grid.header.height = 5; grid.header.offset = {-256, -256}; grid.header.mainTileset = 'L';
    grid.header.groundTilesets = {"Ldrt", "Lgrs"}; grid.header.cliffTilesets = {"CLdi"}; grid.vertices.resize(25);
    for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
    MapData::RefreshTerrainDerived(grid, 0, 0, 5, 5);
    const auto terrain = MapData::SerializeMapTerrain(grid); QVERIFY(terrain.Succeeded());
    std::vector<std::byte> pathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 16u, 16u}) for (unsigned shift = 0; shift < 32; shift += 8)
        pathing.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    pathing.resize(16 + 16 * 16, std::byte{2});
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(*terrain.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wpm"), Bytes(pathing)));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    QVERIFY(f.project.editTrackingReady_ && !f.project.editTrackingFailed_);
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    QVERIFY(session && undo && session->Terrain());
    const auto originalTerrain = session->Terrain();
    const auto triggers = f.Fingerprint();
    const auto state = f.tools->Invoke("get_terrain_edit_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(Explain(state)));
    QVERIFY(state.value("pathing_available").toBool());
    for (const auto& definition : Mcp::TerrainEditToolDefinitions()) {
        bool found = false;
        for (const auto& entry : f.tools->Tools()) if (entry.toObject().value("name") == definition.name) {
            QCOMPARE(entry.toObject().value("inputSchema").toObject(), WithBindingToken(definition.inputSchema)); found = true;
        }
        QVERIFY(found);
    }
    auto request = f.Context(); request.insert("terrain_binding", state.value("terrain_binding"));
    request.insert("dry_run", true);
    request.insert("brush", QJsonObject{{"kind", "raise"}, {"size", 1}, {"strength", 16}});
    request.insert("points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}});
    const auto context = f.Context();
    int beforeWrites = 0;
    f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
    const auto preview = f.tools->Invoke("edit_terrain_stroke", request);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(!preview.value("applied").toBool()); QCOMPARE(undo->count(), 0); QCOMPARE(session->Revision(), std::uint64_t{0});
    QCOMPARE(session->Terrain().get(), originalTerrain.get()); QCOMPARE(f.Context(), context);
    QCOMPARE(beforeWrites, 0);
    auto malformed = request; malformed["dry_run"] = "true";
    QVERIFY(!f.tools->Invoke("edit_terrain_stroke", malformed).value("ok").toBool()); QCOMPARE(beforeWrites, 0);
    auto rejectedWrite = request; rejectedWrite["dry_run"] = false;
    QVERIFY(!f.tools->Invoke("edit_terrain_stroke", rejectedWrite).value("ok").toBool()); QCOMPARE(beforeWrites, 1);
    QCOMPARE(undo->count(), 0); QCOMPARE(f.Context(), context);
    f.tools->SetBeforeWrite({});
    QSignalSpy documentReload(&f.project, &EditorProject::MapDocumentChanged);
    QSignalSpy undoEvents(undo, &QUndoStack::indexChanged);
    request["dry_run"] = false;
    const auto applied = f.tools->Invoke("edit_terrain_stroke", request);
    QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
    QVERIFY(applied.value("applied").toBool()); QVERIFY(!applied.value("map_saved").toBool());
    QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
    QCOMPARE(undo->count(), 1); QCOMPARE(undoEvents.count(), 1); QCOMPARE(documentReload.count(), 0);
    QCOMPARE(session->InspectTerrainPoint({0, 0}).value->groundHeight, 16.f);
    QVERIFY(!f.tools->Invoke("edit_terrain_stroke", request).value("ok").toBool()); QCOMPARE(undo->count(), 1);
    undo->undo(); QCOMPARE(session->InspectTerrainPoint({0, 0}).value->groundHeight, 0.f);
    QVERIFY(!session->IsDirty()); undo->redo(); QCOMPARE(session->InspectTerrainPoint({0, 0}).value->groundHeight, 16.f);
    QCOMPARE(f.Fingerprint(), triggers);
    const auto current = f.tools->Invoke("get_terrain_edit_state", {});
    auto query = QJsonObject{{"terrain_binding", current.value("terrain_binding")}, {"points", request.value("points")}};
    QVERIFY(f.tools->Invoke("query_terrain_points", query).value("ok").toBool());
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    auto forbidden = f.Context(); forbidden.insert("terrain_binding", current.value("terrain_binding"));
    forbidden.insert("dry_run", false); forbidden.insert("brush", request.value("brush")); forbidden.insert("points", request.value("points"));
    QVERIFY(!f.tools->Invoke("edit_terrain_stroke", forbidden).value("ok").toBool());
    QVERIFY(f.tools->Invoke("query_terrain_points", query).value("ok").toBool()); QCOMPARE(undo->count(), 1);
    QFile input(f.source.filePath("map/war3map.w3e")); QVERIFY(input.open(QIODevice::ReadOnly)); QCOMPARE(input.readAll(), Bytes(*terrain.value)); input.close();
    auto altered = *terrain.value; altered.back() = std::byte{3};
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(altered)));
    QVERIFY(!f.tools->Invoke("get_terrain_edit_state", {}).value("ok").toBool());
    QCOMPARE(session->InspectTerrainPoint({0, 0}).value->groundHeight, 16.f); QCOMPARE(undo->count(), 1);
}

void LiveEditorToolsTests::WaterToolsCaptureActualResourcesAndKeepOriginalUndo() {
    std::vector<std::byte> shallow;
    for (const int waterLayerOffset : {32, 0}) for (const bool deep : {false, true}) {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        auto members = Tests::TerrainWaterMembers();
        if (waterLayerOffset == 0) {
            const auto water = std::find_if(members.begin(), members.end(),
                [](const auto& member) { return member.name == "TerrainArt/Water.slk"; });
            QVERIFY(water != members.end());
            auto table = Bytes(water->data);
            QVERIFY(table.contains("C;X2;K0.25\n"));
            table.replace("C;X2;K0.25\n", "C;X2;K0\n");
            water->data = Tests::WaterText(table.toStdString());
        }
        for (const auto& member : members)
            QVERIFY(WriteFile(f.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
        const auto original = members.front().data;
        f.project.war3GameRoot_ = std::filesystem::path(f.outputs.path().toStdWString()) / "game";
        QVERIFY(std::filesystem::create_directory(f.project.war3GameRoot_));
        const auto game = WriteVerifiedArchive(f.project.war3GameRoot_ / "war3.mpq",
            std::vector<MapArchiveMember>{{"fixture.txt", Tests::WaterText("owned optional profile source")}});
        QVERIFY(game.Succeeded());
        auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
        f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
        f.project.LoadMapDocument(); f.project.ResetEditTracking();
        QVERIFY(f.project.editTrackingReady_ && !f.project.editTrackingFailed_);
        auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
        QVERIFY(session && undo && session->Terrain());
        const auto sessionIdentity = session->SessionIdentity();
        const auto triggerFingerprint = f.Fingerprint();
        const auto request = [&](bool dryRun) {
            auto args = f.Context();
            const auto state = f.tools->Invoke("get_terrain_edit_state", {});
            args["terrain_binding"] = state.value("terrain_binding"); args["dry_run"] = dryRun;
            args["brush"] = QJsonObject{{"kind", deep ? "deep_water" : "shallow_water"}, {"shape", "square"},
                {"size", 1}, {"cliff_texture", 0}, {"seed", 4294967295.0}, {"enforce_water_limits", true}};
            args["points"] = QJsonArray{QJsonObject{{"x", 512}, {"y", 512}}};
            return args;
        };
        int beforeWrites = 0;
        f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
        const auto previewRequest = request(true);
        const auto preview = f.tools->Invoke("edit_terrain_stroke", previewRequest);
        QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
        QVERIFY(!preview.value("applied").toBool()); QVERIFY(!preview.value("map_saved").toBool());
        QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0); QCOMPARE(session->Revision(), std::uint64_t{0});
        QCOMPARE(*MapData::SerializeMapTerrain(*session->Terrain()).value, original);
        auto malformed = previewRequest; auto brush = malformed.value("brush").toObject();
        brush["enforce_water_limits"] = "true"; malformed["brush"] = brush;
        QVERIFY(!f.tools->Invoke("edit_terrain_stroke", malformed).value("ok").toBool()); QCOMPARE(beforeWrites, 0);
        auto denied = previewRequest; denied["dry_run"] = false;
        QCOMPARE(ErrorCode(f.tools->Invoke("edit_terrain_stroke", denied)), QString("LIVE_EDITOR_DRAFT_REJECTED"));
        QCOMPARE(beforeWrites, 1); QCOMPARE(undo->count(), 0);
        f.tools->SetBeforeWrite({});
        QSignalSpy reloads(&f.project, &EditorProject::MapDocumentChanged);
        QSignalSpy undoEvents(undo, &QUndoStack::indexChanged);
        const auto applied = f.tools->Invoke("edit_terrain_stroke", denied);
        QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
        QVERIFY(applied.value("applied").toBool()); QVERIFY(!applied.value("map_saved").toBool());
        QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
        QCOMPARE(undo->count(), 1); QCOMPARE(undoEvents.count(), 1); QCOMPARE(reloads.count(), 0);
        const auto changed = MapData::SerializeMapTerrain(*session->Terrain()); QVERIFY(changed.Succeeded());
        QVERIFY(*changed.value != original);
        const auto center = session->InspectTerrainPoint({512, 512}); QVERIFY(center.Succeeded());
        QVERIFY(center.value->vertex.HasWater());
        // 正偏移32的岸线L3会经CleanWater/Converge将深水中心从L0收敛至L1；
        // 零偏移没有该升层，必须同时保留浅L1/深L0及完整W3E不同的断言。
        if (waterLayerOffset == 32) QCOMPARE(int(center.value->vertex.CliffLevel()), 1);
        else if (deep) QCOMPARE(int(center.value->vertex.CliffLevel()), 0);
        else QCOMPARE(int(center.value->vertex.CliffLevel()), 1);
        if (deep && waterLayerOffset == 0) QVERIFY(*changed.value != shallow);
        if (!deep) shallow = *changed.value;
        QVERIFY(!f.tools->Invoke("edit_terrain_stroke", denied).value("ok").toBool()); QCOMPARE(undo->count(), 1);
        undo->undo(); QCOMPARE(*MapData::SerializeMapTerrain(*session->Terrain()).value, original); QVERIFY(!session->IsDirty());
        undo->redo(); QCOMPARE(*MapData::SerializeMapTerrain(*session->Terrain()).value, *changed.value);
        QCOMPARE(f.Fingerprint(), triggerFingerprint);
        QCOMPARE(Tests::WaterTake(IO::ReadFileBytes(f.project.projectRoot_ / "map/war3map.w3e")), original);
        // 可选profile的实际winner必须参与捕获，不能靠原有UI/MiscData默认值继续准备。
        const auto patch = WriteVerifiedArchive(f.project.war3GameRoot_ / "war3patch.mpq",
            std::vector<MapArchiveMember>{{"Units/MiscData.txt", Tests::WaterText("[Terrain]\nMaxHeight=1408\n")}});
        QVERIFY(patch.Succeeded());
        const auto refused = f.tools->Invoke("edit_terrain_stroke", request(true));
        QVERIFY(!refused.value("ok").toBool()); QCOMPARE(undo->count(), 1);
        QCOMPARE(*MapData::SerializeMapTerrain(*session->Terrain()).value, *changed.value);
        QVERIFY(std::filesystem::remove(f.project.war3GameRoot_ / "war3patch.mpq"));
        auto altered = original; altered.back() ^= std::byte{1};
        QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(altered)));
        QVERIFY(!f.tools->Invoke("get_terrain_edit_state", {}).value("ok").toBool());
        QCOMPARE(*MapData::SerializeMapTerrain(*session->Terrain()).value, *changed.value); QCOMPARE(undo->count(), 1);
        QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(session->SessionIdentity(), sessionIdentity);
        QCOMPARE(f.project.mapDataUndoStack_, undo);
    }
}

void LiveEditorToolsTests::BoundsToolsCaptureActualResourcesAndKeepOriginalUndo() {
    namespace F = TerrainCornerRampFixtures;
    F::Fixture native; Fixture f;
    QVERIFY2(f.Initialize() && f.InstallBounds(native), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get(); auto* undo = f.project.mapDataUndoStack_;
    const auto terrainBytes = [&] { return F::Take(MapData::SerializeMapTerrain(*session->Terrain())); };
    const auto settingsBytes = [&] { return F::Take(session->Snapshot().Settings()->Serialize()); };
    const auto originalTerrain = terrainBytes(), originalSettings = settingsBytes();
    const auto originalUnits = F::Take(session->Snapshot().Units()->Serialize());
    const auto originalDoodads = F::Take(session->Snapshot().Doodads()->Serialize());
    const auto triggers = f.Fingerprint();
    const auto state = f.tools->Invoke("get_terrain_edit_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(Explain(state)));
    auto request = f.Context();
    request["terrain_binding"] = state.value("terrain_binding"); request["dry_run"] = true;
    request["west"] = 2; request["south"] = 3; request["east"] = 30; request["north"] = 29;
    const auto context = f.Context();
    int beforeWrites = 0;
    f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
    const auto preview = f.tools->Invoke("edit_terrain_bounds", request);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(!preview.value("applied").toBool()); QVERIFY(!preview.value("map_saved").toBool());
    QVERIFY(preview.value("native_save_required").toBool());
    QVERIFY(preview.value("changes").toObject().value("has_changes").toBool());
    QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0); QCOMPARE(session->Revision(), std::uint64_t{0});
    QCOMPARE(terrainBytes(), originalTerrain); QCOMPARE(settingsBytes(), originalSettings); QCOMPARE(f.Context(), context);
    for (const auto& malformed : {QJsonObject{{"dry_run", "true"}}, QJsonObject{{"points", QJsonArray{}}},
        QJsonObject{{"west", 2.5}}, QJsonObject{{"north", 2049}}}) {
        auto invalid = request;
        for (auto it = malformed.begin(); it != malformed.end(); ++it) invalid[it.key()] = it.value();
        QVERIFY(!f.tools->Invoke("edit_terrain_bounds", invalid).value("ok").toBool());
        QCOMPARE(beforeWrites, 0); QCOMPARE(undo->count(), 0);
    }
    auto write = request; write["dry_run"] = false;
    QCOMPARE(ErrorCode(f.tools->Invoke("edit_terrain_bounds", write)), QStringLiteral("LIVE_EDITOR_DRAFT_REJECTED"));
    QCOMPARE(beforeWrites, 1); QCOMPARE(undo->count(), 0); QCOMPARE(f.Context(), context);
    f.tools->SetBeforeWrite({});
    QSignalSpy reloads(&f.project, &EditorProject::MapDocumentChanged);
    QSignalSpy undoEvents(undo, &QUndoStack::indexChanged);
    const auto applied = f.tools->Invoke("edit_terrain_bounds", write);
    QVERIFY2(applied.value("ok").toBool(), qPrintable(Explain(applied)));
    QVERIFY(applied.value("applied").toBool()); QVERIFY(applied.value("native_save_required").toBool());
    QVERIFY(!applied.value("map_saved").toBool()); QVERIFY(applied.value("edit_receipt").isObject());
    QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(f.project.mapDataUndoStack_, undo);
    QCOMPARE(undo->count(), 1); QCOMPARE(undoEvents.count(), 1); QCOMPARE(reloads.count(), 0);
    const auto settings = *session->Snapshot().Settings();
    QCOMPARE(settings.playableWidth, 28); QCOMPARE(settings.playableHeight, 26);
    const std::array<std::int32_t, 4> margins{2, 2, 3, 3}; QVERIFY(settings.cameraMargins == margins);
    const std::array<float, 8> camera{256, 384, 3840, 3712, 256, 3712, 3840, 384};
    QVERIFY(settings.cameraBounds == camera);
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        const bool edge = x < 32 && y < 32 && (x < 2 || x >= 30 || y < 3 || y >= 29);
        QCOMPARE(session->Terrain()->vertices[y * 33 + x].IsMapEdge(), edge);
    }
    QCOMPARE(F::Take(session->Snapshot().Units()->Serialize()), originalUnits);
    QCOMPARE(F::Take(session->Snapshot().Doodads()->Serialize()), originalDoodads);
    QCOMPARE(f.Fingerprint(), triggers); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    const auto changedTerrain = terrainBytes(), changedSettings = settingsBytes();
    QVERIFY(!f.tools->Invoke("edit_terrain_bounds", write).value("ok").toBool()); QCOMPARE(undo->count(), 1);
    undo->undo(); QCOMPARE(terrainBytes(), originalTerrain); QCOMPARE(settingsBytes(), originalSettings);
    undo->redo(); QCOMPARE(terrainBytes(), changedTerrain); QCOMPARE(settingsBytes(), changedSettings);
    auto repeat = f.Context();
    repeat["terrain_binding"] = f.tools->Invoke("get_terrain_edit_state", {}).value("terrain_binding");
    repeat["dry_run"] = false; repeat["west"] = 2; repeat["south"] = 3; repeat["east"] = 30; repeat["north"] = 29;
    const auto noOp = f.tools->Invoke("edit_terrain_bounds", repeat);
    QVERIFY2(noOp.value("ok").toBool(), qPrintable(Explain(noOp)));
    QVERIFY(!noOp.value("applied").toBool()); QCOMPARE(undo->count(), 1);
    QFile terrainSource(f.source.filePath("map/war3map.w3e")); QVERIFY(terrainSource.open(QIODevice::ReadOnly));
    QCOMPARE(terrainSource.readAll(), Bytes(originalTerrain)); terrainSource.close();
    // 不发伪来源变化信号；实际文件SHA必须拒绝后续范围命令，保留原会话和Undo。
    QVERIFY(WriteFile(f.source.filePath("map/UI/MiscData.txt"), "[CameraMargins]\nLeft=4\nRight=4\nBottom=4\nTop=4\n"));
    repeat["dry_run"] = true;
    QVERIFY(!f.tools->Invoke("edit_terrain_bounds", repeat).value("ok").toBool());
    QCOMPARE(terrainBytes(), changedTerrain); QCOMPARE(settingsBytes(), changedSettings); QCOMPARE(undo->count(), 1);
}

void LiveEditorToolsTests::BoundsPreparationRejectsConcurrentRevision() {
    namespace F = TerrainCornerRampFixtures;
    F::Fixture native; Fixture f;
    QVERIFY2(f.Initialize() && f.InstallBounds(native), qPrintable(f.error));
    auto* session = f.project.mapEdits_.get();
    const auto originalTerrain = F::Take(MapData::SerializeMapTerrain(*session->Terrain()));
    auto request = f.Context();
    request["terrain_binding"] = f.tools->Invoke("get_terrain_edit_state", {}).value("terrain_binding");
    request["dry_run"] = false; request["west"] = 2; request["south"] = 3; request["east"] = 30; request["north"] = 29;
    bool timerRan = false, authorEditSucceeded = false;
    QJsonObject reentered;
    QTimer::singleShot(0, &f.project, [&] {
        timerRan = true;
        reentered = f.tools->Invoke("edit_terrain_bounds", request);
        auto settings = *session->Snapshot().Settings(); settings.mapName = "concurrent-author-bounds-edit";
        authorEditSucceeded = session->EditSettings(settings).Succeeded();
    });
    const auto rejected = f.tools->Invoke("edit_terrain_bounds", request);
    QVERIFY(timerRan); QVERIFY(authorEditSucceeded);
    QCOMPARE(ErrorCode(reentered), QStringLiteral("LIVE_EDITOR_BUSY"));
    QVERIFY(!rejected.value("ok").toBool()); QCOMPARE(ErrorCode(rejected), QStringLiteral("TERRAIN_EDIT_STALE"));
    QCOMPARE(f.project.mapEdits_.get(), session); QCOMPARE(session->Revision(), std::uint64_t{1});
    QCOMPARE(session->Snapshot().Settings()->mapName, std::string("concurrent-author-bounds-edit"));
    QCOMPARE(F::Take(MapData::SerializeMapTerrain(*session->Terrain())), originalTerrain);
    QCOMPARE(f.project.mapDataUndoStack_->count(), 0);
}

void LiveEditorToolsTests::EntityFieldsUseOriginalSessionAndAtomicUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    MapData::RegionsDocument regions;
    regions.entries.push_back({-128.f, -64.f, 128.f, 64.f, "原创区域", 31, {}, "", {12, 34, 56, 78}});
    const auto source = regions.Serialize(); QVERIFY(source.Succeeded());
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3r"), Bytes(*source.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3s"), QByteArray("bad"))); // 原损坏声音必须保留 Invalid。
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    auto document = MapDocument::FromDirectory(*directory.value); QVERIFY(document.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.mapDocument_ = std::make_unique<MapDocument>(std::move(*document.value));
    f.project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*f.project.mapDocument_);
    auto* originalDocument = f.project.mapDocument_.get();
    auto* originalSession = f.project.mapEdits_.get();
    auto* originalUndo = f.project.mapDataUndoStack_;
    QVERIFY(originalUndo != nullptr);
    f.project.ResetEditTracking(); QVERIFY(f.project.editTrackingReady_);
    const auto triggers = f.Fingerprint();
    const auto sessionIdentity = originalSession->SessionIdentity();
    const auto query = [&] { return f.tools->Invoke("entities_query", QJsonObject{{"kind", "regions"}}); };
    const auto field = [](const QJsonObject& page, const QString& id) {
        for (const auto& value : page.value("rows").toArray()[0].toObject().value("fields").toArray())
            if (value.toObject().value("id").toString() == id) return value.toObject();
        return QJsonObject{};
    };
    const auto request = [&](const QJsonObject& page, QString value) {
        auto result = f.Context(); result.insert("kind", "regions");
        result.insert("expected_member_sha256", page.value("member").toObject().value("member_sha256"));
        const auto row = page.value("rows").toArray()[0].toObject();
        result.insert("changes", QJsonArray{QJsonObject{{"source_index", "0"}, {"row_key", row.value("row_key")},
            {"field", "left"}, {"expected_raw", field(page, "left").value("raw")}, {"value", value}}});
        return result;
    };
    for (const auto& definition : Mcp::MapDataToolDefinitions()) {
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject(); if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QVERIFY(found);
    }
    auto initial = query(); QVERIFY2(initial.value("ok").toBool(), qPrintable(Explain(initial)));
    QCOMPARE(field(initial, "left").value("value").toDouble(), -128.);
    const auto firstRequest = request(initial, "-127");
    const auto edited = f.tools->Invoke("entities_set_fields", firstRequest);
    QVERIFY2(edited.value("ok").toBool(), qPrintable(Explain(edited)));
    QCOMPARE(edited.value("session_identity").toString(), QString::number(sessionIdentity));
    QCOMPARE(originalUndo->count(), 1);
    QCOMPARE(field(query(), "left").value("value").toDouble(), -127.);
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_set_fields", firstRequest)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    auto active = query(); auto invalid = request(active, "-126");
    auto changes = invalid.value("changes").toArray();
    auto second = changes[0].toObject(); second.insert("field", "right");
    second.insert("expected_raw", field(active, "right").value("raw")); second.insert("value", "not-a-number");
    changes.append(second); invalid.insert("changes", changes);
    const auto beforeRejected = f.Context();
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_set_fields", invalid)), QStringLiteral("MAP_FIELD_INPUT_INVALID"));
    QCOMPARE(f.Context(), beforeRejected); QCOMPARE(originalUndo->count(), 1);
    QCOMPARE(field(query(), "left").value("value").toDouble(), -127.);
    originalUndo->undo(); QCOMPARE(field(query(), "left").value("value").toDouble(), -128.);
    originalUndo->redo(); QCOMPARE(field(query(), "left").value("value").toDouble(), -127.);
    originalUndo->undo();
    const auto final = MapData::ProjectMapData(*originalDocument, *originalSession); QVERIFY(final.Succeeded());
    QCOMPARE(final.value->Member(MapDataKind::Sounds).state, MapDataState::Invalid);
    QVERIFY(final.value->Member(MapDataKind::Regions).sources[0].data == *source.value);
    QCOMPARE(f.project.mapDocument_.get(), originalDocument); QCOMPARE(f.project.mapEdits_.get(), originalSession);
    QCOMPARE(f.project.mapDataUndoStack_, originalUndo); QCOMPARE(f.Fingerprint(), triggers);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    const auto beforeSourceDrift = f.Context();
    auto drift = Bytes(*source.value); drift[8] = static_cast<char>(drift[8] ^ 1);
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3r"), drift));
    QVERIFY(!query().value("ok").toBool());
    auto rejected = request(initial, "-127");
    QVERIFY(!f.tools->Invoke("entities_set_fields", rejected).value("ok").toBool());
    QCOMPARE(f.Context(), beforeSourceDrift); QCOMPARE(originalSession->SessionIdentity(), sessionIdentity);
    QCOMPARE(originalUndo->count(), 1); QCOMPARE(originalUndo->index(), 0);
}

void LiveEditorToolsTests::SettingsAndStringsKeepOriginalGuiUndoAndPolicy() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto w3i = MapDataFixtures::FullMapSettingsV25().bytes;
    const QByteArray wts("// original ordering\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\nSTRING 7\r\n{\r\nUntouched\r\n}\r\n");
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3i"), Bytes(w3i)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.wts"), wts));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    auto document = MapDocument::FromDirectory(*directory.value); QVERIFY(document.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.mapDocument_ = std::make_unique<MapDocument>(std::move(*document.value));
    f.project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*f.project.mapDocument_);
    f.project.ResetEditTracking(); QVERIFY(f.project.editTrackingReady_);
    auto* originalDocument = f.project.mapDocument_.get();
    auto* originalSession = f.project.mapEdits_.get();
    auto* originalUndo = f.project.mapDataUndoStack_;
    QVERIFY(originalUndo != nullptr);
    const auto triggers = f.Fingerprint();
    const auto settings = [&] { return f.tools->Invoke("map_settings_get", {}); };
    const auto strings = [&] { return f.tools->Invoke("strings_query", {}); };
    const auto field = [](const QJsonObject& page, const char* id) {
        const auto rows = page.value("rows").toArray();
        if (rows.size() != 1) return QJsonObject{};
        for (const auto& value : rows[0].toObject().value("fields").toArray())
            if (value.toObject().value("id") == QLatin1String(id)) return value.toObject();
        return QJsonObject{};
    };
    const auto request = [&](const QJsonObject& page, bool shared, QJsonArray changes) {
        auto result = f.Context();
        result.insert("expected_member_sha256", page.value("member").toObject().value("member_sha256"));
        result.insert("write_mode", shared ? "shared_string" : "literal");
        if (!shared) result.insert("setting_id", "w3i");
        result.insert("changes", changes);
        return result;
    };
    const auto initial = settings(); QVERIFY2(initial.value("ok").toBool(), qPrintable(Explain(initial)));
    QCOMPARE(field(initial, "author").value("value").toString(), QStringLiteral("Creator"));
    QCOMPARE(field(initial, "description").value("wire_value").toString(), QStringLiteral("TRIGSTR_0042"));
    QCOMPARE(field(initial, "description").value("value").toString(), QStringLiteral("Original description"));
    auto edit = request(initial, false, {QJsonObject{{"field", "author"},
        {"expected_raw", field(initial, "author").value("raw")}, {"value", "迁移后的作者"}}});
    const auto changed = f.tools->Invoke("map_settings_set", edit);
    QVERIFY2(changed.value("ok").toBool(), qPrintable(Explain(changed)));
    QCOMPARE(changed.value("session_identity").toString(), QString::number(originalSession->SessionIdentity()));
    QCOMPARE(originalUndo->count(), 1);
    QCOMPARE(field(settings(), "author").value("value").toString(), QStringLiteral("迁移后的作者"));
    QCOMPARE(ErrorCode(f.tools->Invoke("map_settings_set", edit)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    auto stringPage = strings(); QVERIFY2(stringPage.value("ok").toBool(), qPrintable(Explain(stringPage)));
    QJsonObject stringRow;
    for (const auto& row : stringPage.value("rows").toArray())
        if (row.toObject().value("string_id") == QStringLiteral("42")) stringRow = row.toObject();
    QVERIFY(!stringRow.isEmpty());
    QJsonValue stringRaw;
    for (const auto& value : stringRow.value("fields").toArray())
        if (value.toObject().value("id") == QStringLiteral("value")) stringRaw = value.toObject().value("raw");
    QVERIFY(stringRaw.isString());
    const auto stringEdit = request(stringPage, true, {QJsonObject{{"string_id", "42"},
        {"expected_raw", stringRaw}, {"value", "共享文字已修改"}}});
    const auto changedString = f.tools->Invoke("strings_set", stringEdit);
    QVERIFY2(changedString.value("ok").toBool(), qPrintable(Explain(changedString)));
    QCOMPARE(originalUndo->count(), 2);
    QCOMPARE(field(settings(), "description").value("value").toString(), QStringLiteral("共享文字已修改"));
    QCOMPARE(field(settings(), "description").value("wire_value").toString(), QStringLiteral("TRIGSTR_0042"));
    originalUndo->undo();
    QCOMPARE(field(settings(), "description").value("value").toString(), QStringLiteral("Original description"));
    QCOMPARE(field(settings(), "author").value("value").toString(), QStringLiteral("迁移后的作者"));
    originalUndo->undo();
    const auto restored = MapData::ProjectMapData(*originalDocument, *originalSession); QVERIFY(restored.Succeeded());
    QVERIFY(restored.value->Member(MapDataKind::Settings).sources[0].data == w3i);
    QCOMPARE(Bytes(restored.value->Member(MapDataKind::Strings).sources[0].data), wts);
    originalUndo->redo(); originalUndo->redo();
    QCOMPARE(field(settings(), "description").value("value").toString(), QStringLiteral("共享文字已修改"));
    const auto beforePolicy = f.Context();
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    const auto names = Names(f.tools->Tools());
    QVERIFY(names.contains("map_settings_get") && names.contains("strings_query"));
    QVERIFY(!names.contains("map_settings_set") && !names.contains("strings_set"));
    QVERIFY(!f.tools->Invoke("strings_set", stringEdit).value("ok").toBool());
    QVERIFY(settings().value("ok").toBool());
    QCOMPARE(f.Context(), beforePolicy); QCOMPARE(originalUndo->count(), 2);
    QCOMPARE(f.project.mapDocument_.get(), originalDocument); QCOMPARE(f.project.mapEdits_.get(), originalSession);
    QCOMPARE(f.project.mapDataUndoStack_, originalUndo); QCOMPARE(f.Fingerprint(), triggers);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::PlacementToolsKeepAuthorEditsAndOriginalUndo() {
    using namespace MapData;
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    UnitPlacements units;
    UnitPlacement unit;
    unit.rawcode = "hfoo"; unit.creationId = 19; unit.position = {64, -128, 16};
    unit.scale = {1, 2, 0.5f}; unit.rotation = 1; unit.owner = 3;
    unit.unknownOwnerBytes = {12, 34}; unit.inventory = {{2, "I000"}};
    unit.abilities = {{"A000", true, 2}};
    unit.random = RandomCustomTable{{{"hpea", 37}, {"hrif", 63}}};
    unit.itemSets = {{{{"I001", 73}, {"I002", 27}}}};
    units.units.push_back(unit);
    RegionsDocument regions;
    regions.entries.push_back({-128.f, -64.f, 128.f, 64.f, "作者区域", 31, {}, "", {12, 34, 56, 78}});
    const auto originalUnits = units.Serialize(), originalRegions = regions.Serialize();
    QVERIFY(originalUnits.Succeeded() && originalRegions.Succeeded());
    QVERIFY(WriteFile(f.source.filePath("map/war3mapUnits.doo"), Bytes(*originalUnits.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3r"), Bytes(*originalRegions.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3s"), "bad"));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    auto document = MapDocument::FromDirectory(*directory.value); QVERIFY(document.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.mapDocument_ = std::make_unique<MapDocument>(std::move(*document.value));
    f.project.mapEdits_ = std::make_unique<MapDataEditSession>(*f.project.mapDocument_);
    auto* const originalDocument = f.project.mapDocument_.get();
    auto* const originalSession = f.project.mapEdits_.get();
    auto* const undo = f.project.mapDataUndoStack_;
    QVERIFY(undo != nullptr); f.project.ResetEditTracking(); QVERIFY(f.project.editTrackingReady_);
    const auto triggerSha = f.Fingerprint();
    const auto sessionIdentity = originalSession->SessionIdentity();
    // 原作者命令先进入同一个会话与 Undo；Agent 复制不得覆盖这个区域变化。
    QVERIFY(SetRegionBounds(*originalSession, MakeRegionId(0, regions.entries[0]), -127, -64, 128, 64).Succeeded());
    QtProject::PushMapDataEdit(f.project, "作者区域命令", "作者已有编辑");
    (void)f.project.ObserveMapDataEdit("作者已有编辑");
    const auto query = [&] { return f.tools->Invoke("entities_query", QJsonObject{{"kind", "units"}}); };
    const auto current = [&] { return ProjectMapData(*originalDocument, *originalSession); };
    const auto request = [&](const QJsonObject& page) {
        auto args = f.Context(); args.insert("kind", "units");
        args.insert("expected_member_sha256", page.value("member").toObject().value("member_sha256"));
        const auto row = page.value("rows").toArray()[0].toObject();
        args.insert("sources", QJsonArray{QJsonObject{{"source_index", row.value("source_index")}, {"row_key", row.value("row_key")}}});
        args.insert("offset", QJsonObject{{"x", 1}, {"y", 2}, {"z", -3}}); return args;
    };
    for (const auto& definition : Mcp::PlacementToolDefinitions()) {
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject(); if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
        }
        QVERIFY(found);
    }
    auto page = query(); QVERIFY2(page.value("ok").toBool(), qPrintable(Explain(page)));
    const auto copyRequest = request(page);
    auto copied = f.tools->Invoke("entities_copy", copyRequest);
    QVERIFY2(copied.value("ok").toBool(), qPrintable(Explain(copied)));
    QCOMPARE(copied.value("session_identity").toString(), QString::number(sessionIdentity));
    QCOMPARE(copied.value("before").toObject().value("rows").toArray().size(), 1);
    QCOMPARE(copied.value("after").toObject().value("rows").toArray().size(), 1);
    QVERIFY(!copied.value("saved").toBool()); QVERIFY(!copied.value("runtime_verified").toBool());
    QCOMPARE(undo->count(), 2);
    auto after = current(); QVERIFY(after.Succeeded());
    QCOMPARE(after.value->Regions()->entries[0].left, -127.f);
    QCOMPARE(after.value->Units()->units.size(), std::size_t{2});
    UnitPlacements expected = *originalDocument->Units();
    auto clone = expected.units.front(); clone.position = {65, -126, 13}; clone.creationId = 20;
    expected.units.push_back(clone); const auto expectedBytes = expected.Serialize(); QVERIFY(expectedBytes.Succeeded());
    QVERIFY(after.value->Member(MapDataKind::Units).sources[0].data == *expectedBytes.value);
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_copy", copyRequest)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    const auto rejectedIdentity = f.Context(); auto invalid = request(query());
    invalid.insert("offset", QJsonObject{{"x", "1"}, {"y", 0}, {"z", 0}});
    QVERIFY(!f.tools->Invoke("entities_copy", invalid).value("ok").toBool());
    QCOMPARE(f.Context(), rejectedIdentity); QCOMPARE(undo->count(), 2);
    undo->undo(); auto restored = current(); QVERIFY(restored.Succeeded());
    QCOMPARE(restored.value->Units()->units.size(), std::size_t{1});
    QCOMPARE(restored.value->Regions()->entries[0].left, -127.f);
    undo->redo(); QCOMPARE(current().value->Units()->units.size(), std::size_t{2});
    // 当前 GUI 行定位允许改刚复制的行；同一个原会话持有真实命令回执。
    auto active = query(); const auto copiedRow = active.value("rows").toArray()[1].toObject();
    auto rotate = f.Context(); rotate.insert("kind", "units");
    rotate.insert("expected_member_sha256", active.value("member").toObject().value("member_sha256"));
    rotate.insert("source", QJsonObject{{"source_index", copiedRow.value("source_index")}, {"row_key", copiedRow.value("row_key")}});
    rotate.insert("facing_radians", 1.5);
    const auto rotated = f.tools->Invoke("entities_rotate", rotate);
    QVERIFY2(rotated.value("ok").toBool(), qPrintable(Explain(rotated)));
    QCOMPARE(rotated.value("session_identity").toString(), QString::number(sessionIdentity));
    QCOMPARE(rotated.value("rotation_unit").toString(), QStringLiteral("radians"));
    QCOMPARE(current().value->Units()->units[1].rotation, 1.5f);
    QCOMPARE(current().value->Regions()->entries[0].left, -127.f);
    undo->undo(); undo->undo(); undo->undo();
    const auto final = current(); QVERIFY(final.Succeeded());
    QVERIFY(final.value->Member(MapDataKind::Units).sources[0].data == *originalUnits.value);
    QVERIFY(final.value->Member(MapDataKind::Regions).sources[0].data == *originalRegions.value);
    QCOMPARE(final.value->Member(MapDataKind::Sounds).state, MapDataState::Invalid);
    QCOMPARE(originalSession->SessionIdentity(), sessionIdentity);
    QCOMPARE(f.project.mapDocument_.get(), originalDocument); QCOMPARE(f.project.mapEdits_.get(), originalSession);
    QCOMPARE(f.project.mapDataUndoStack_, undo); QCOMPARE(f.Fingerprint(), triggerSha);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::SpatialEntityQueriesShareSchemasAndObserveOriginalUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto members = MapDataSpatialFixtures::Members();
    for (const auto& member : members)
        QVERIFY(WriteFile(f.source.filePath("map/" + QString::fromStdString(member.name)), Bytes(member.data)));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.LoadMapDocument(); f.project.ResetEditTracking();
    QVERIFY(f.project.mapEdits_ && f.project.mapDataUndoStack_);
    auto* const session = f.project.mapEdits_.get(); auto* const undo = f.project.mapDataUndoStack_;
    const auto context = f.Context();
    const auto triggerSha = QString::fromStdString(f.Fingerprint());
    int beforeWrites = 0; f.tools->SetBeforeWrite([&] { ++beforeWrites; return false; });
    for (const auto& definition : Mcp::MapDataToolDefinitions()) {
        if (definition.name != "entities_query") continue;
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject(); if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
            QVERIFY(definition.inputSchema.value("properties").toObject().value("filter").toObject()
                .value("properties").toObject().contains("world_rect"));
        }
        QVERIFY(found);
    }
    const QJsonObject rectangle{{"left", -32}, {"bottom", -16}, {"right", 32}, {"top", 16}};
    const QJsonObject filter{{"type_id", "hfoo"}, {"player", 0}, {"world_rect", rectangle}};
    const auto request = [&](const QJsonObject& cursor = {}) {
        auto arguments = f.Context(); arguments["kind"] = "units"; arguments["filter"] = filter;
        QJsonObject page{{"limit", 1}}; if (!cursor.isEmpty()) page["cursor"] = cursor;
        arguments["page"] = page; return arguments;
    };
    const auto first = f.tools->Invoke("entities_query", request());
    QVERIFY2(first.value("ok").toBool(), qPrintable(Explain(first)));
    QCOMPARE(first.value("matched_total").toString(), QStringLiteral("3"));
    QCOMPARE(first.value("rows").toArray().first().toObject().value("source_index").toString(), QStringLiteral("0"));
    const auto cursor = first.value("next_cursor").toObject(); const auto oldNextRequest = request(cursor);
    const auto second = f.tools->Invoke("entities_query", oldNextRequest);
    QVERIFY2(second.value("ok").toBool(), qPrintable(Explain(second)));
    QCOMPARE(second.value("rows").toArray().first().toObject().value("source_index").toString(), QStringLiteral("3"));
    for (const auto kind : {"doodads", "regions", "cameras"}) {
        auto arguments = f.Context(); arguments["kind"] = kind;
        arguments["filter"] = QJsonObject{{"world_rect", rectangle}};
        const auto page = f.tools->Invoke("entities_query", arguments);
        QVERIFY2(page.value("ok").toBool(), qPrintable(Explain(page)));
        arguments.remove("filter"); arguments["page"] = QJsonObject{{"limit", 1}};
        const auto head = f.tools->Invoke("entities_query", arguments); QVERIFY(head.value("ok").toBool());
        arguments["page"] = QJsonObject{{"limit", 1}, {"cursor", head.value("next_cursor")}};
        const auto next = f.tools->Invoke("entities_query", arguments);
        QVERIFY2(next.value("ok").toBool(), qPrintable(Explain(next)));
        QCOMPARE(next.value("rows").toArray().first().toObject().value("source_index").toString(), QStringLiteral("1"));
        QCOMPARE(next.value("total"), head.value("total"));
    }
    QCOMPARE(beforeWrites, 0); QCOMPARE(f.Context(), context); QCOMPARE(undo->count(), 0);
    QCOMPARE(session->Revision(), std::uint64_t{0});
    f.tools->SetBeforeWrite({});
    const auto row = first.value("rows").toArray().first().toObject();
    auto edit = f.Context(); edit["kind"] = "units";
    edit["expected_member_sha256"] = first.value("member").toObject().value("member_sha256");
    edit["changes"] = QJsonArray{QJsonObject{{"source_index", row.value("source_index")}, {"row_key", row.value("row_key")},
        {"field", "owner"}, {"expected_raw", "0"}, {"value", "1"}}};
    const auto changed = f.tools->Invoke("entities_set_fields", edit);
    QVERIFY2(changed.value("ok").toBool(), qPrintable(Explain(changed))); QCOMPARE(undo->count(), 1);
    const auto after = f.tools->Invoke("entities_query", request());
    QVERIFY(after.value("ok").toBool()); QCOMPARE(after.value("matched_total").toString(), QStringLiteral("2"));
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_query", oldNextRequest)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_query", request(cursor))), QStringLiteral("ENTITY_QUERY_CURSOR_STALE"));
    QCOMPARE(undo->count(), 1); QCOMPARE(f.project.mapEdits_.get(), session);
    undo->undo();
    const auto restored = f.tools->Invoke("entities_query", request());
    QVERIFY(restored.value("ok").toBool()); QCOMPARE(restored.value("matched_total").toString(), QStringLiteral("3"));
    const auto current = MapData::ProjectMapData(*f.project.mapDocument_, *session); QVERIFY(current.Succeeded());
    for (const auto& member : members) {
        const auto actual = std::find_if(current.value->Members().begin(), current.value->Members().end(),
            [&](const auto& value) { return value.memberName == member.name; });
        QVERIFY(actual != current.value->Members().end()); QCOMPARE(actual->sources.size(), std::size_t{1});
        QCOMPARE(actual->sources.front().data, member.data);
    }
    QCOMPARE(QString::fromStdString(f.Fingerprint()), triggerSha); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::PlacementCreateDeleteUseRealSourcesAndPurePreview() {
    using namespace MapData;
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); QVERIFY2(f.InstallObjects(false, true), qPrintable(f.error));
    const auto unitBytes = UnitPlacements{}.Serialize(), doodadBytes = DoodadPlacements{}.Serialize();
    QVERIFY(unitBytes.Succeeded() && doodadBytes.Succeeded());
    const auto terrainBytes = Tests::PlacementTerrainBytes();
    QVERIFY(WriteFile(f.source.filePath("map/war3mapUnits.doo"), Bytes(*unitBytes.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.doo"), Bytes(*doodadBytes.value)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3i"), Bytes(MapDataFixtures::FullMapSettingsV25().bytes)));
    QVERIFY(WriteFile(f.source.filePath("map/war3map.w3e"), Bytes(terrainBytes)));
    auto directory = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(directory.Succeeded());
    auto document = MapDocument::FromDirectory(*directory.value); QVERIFY(document.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*directory.value));
    f.project.mapDocument_ = std::make_unique<MapDocument>(std::move(*document.value));
    f.project.mapEdits_ = std::make_unique<MapDataEditSession>(*f.project.mapDocument_);
    QVERIFY(f.project.mapEdits_->AttachTerrain(terrainBytes).Succeeded());
    f.project.ResetEditTracking(); QVERIFY(f.project.editTrackingReady_);
    auto* const originalSession = f.project.mapEdits_.get();
    auto* const undo = f.project.mapDataUndoStack_; QVERIFY(undo);
    const auto sourceSha = f.project.directorySnapshot_->Snapshot().aggregateSha256;
    const auto originalIdentity = f.Context(); const auto triggerSha = f.Fingerprint();
    const auto current = [&] { return ProjectMapData(*f.project.mapDocument_, *originalSession); };
    const auto base = [&](const char* kind) {
        const auto page = f.tools->Invoke("entities_query", {{"kind", kind}});
        auto args = f.Context(); args.insert("kind", kind);
        args.insert("expected_member_sha256", page.value("member").toObject().value("member_sha256"));
        return args;
    };
    const auto item = [](const char* kind, const char* id) {
        return QJsonObject{{"object_kind", kind}, {"object_id", id}, {"x", 64}, {"y", -128}};
    };
    auto units = base("units");
    units.insert("placements", QJsonArray{item("unit", "u001"), item("unit", "U001"), item("item", "i001"), item("item", "I001")});
    auto previewArgs = units; previewArgs.insert("dry_run", true);
    int draftSubmits = 0;
    f.tools->SetBeforeWrite([&] { ++draftSubmits; return false; });
    const auto preview = f.tools->Invoke("entities_create", previewArgs);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(preview.value("dry_run").toBool()); QVERIFY(!preview.value("applied").toBool());
    QCOMPARE(preview.value("candidate_ids").toArray().size(), 4); QVERIFY(preview.value("new_ids").toArray().isEmpty());
    QVERIFY(!preview.contains("session_identity") && !preview.contains("command_identity"));
    QCOMPARE(draftSubmits, 0); QCOMPARE(undo->count(), 0); QCOMPARE(originalSession->Revision(), std::uint64_t{0});
    QCOMPARE(f.Context(), originalIdentity); QVERIFY(current().value->Units()->units.empty());
    f.tools->SetBeforeWrite([&] { ++draftSubmits; return true; });
    const auto created = f.tools->Invoke("entities_create", units);
    QVERIFY2(created.value("ok").toBool(), qPrintable(Explain(created)));
    QCOMPARE(draftSubmits, 1); QCOMPARE(undo->count(), 1);
    QCOMPARE(created.value("session_identity").toString(), QString::number(originalSession->SessionIdentity()));
    QCOMPARE(current().value->Units()->units.size(), std::size_t{4});
    auto doodads = base("doodads");
    doodads.insert("placements", QJsonArray{item("doodad", "D001"), item("doodad", "D101"), item("destructable", "b001"), item("destructable", "b101")});
    const auto ground = f.tools->Invoke("entities_create", doodads);
    QVERIFY2(ground.value("ok").toBool(), qPrintable(Explain(ground)));
    QCOMPARE(undo->count(), 2); QCOMPARE(current().value->Doodads()->doodads.size(), std::size_t{4});
    const auto createIds = created.value("new_ids").toArray();
    auto remove = base("units"); QJsonArray sources;
    for (const auto index : {3, 1}) {
        const auto id = createIds[index].toObject();
        sources.append(QJsonObject{{"source_index", id.value("source_index")}, {"row_key", id.value("row_key")}});
    }
    remove.insert("sources", sources);
    const auto deleted = f.tools->Invoke("entities_delete", remove);
    QVERIFY2(deleted.value("ok").toBool(), qPrintable(Explain(deleted)));
    QCOMPARE(undo->count(), 3);
    auto after = current(); QVERIFY(after.Succeeded()); QCOMPARE(after.value->Units()->units.size(), std::size_t{2});
    QCOMPARE(after.value->Units()->units[0].rawcode, std::string("u001")); QCOMPARE(after.value->Units()->units[1].rawcode, std::string("i001"));
    QVERIFY(deleted.value("after").toObject().value("rows").toArray().isEmpty());
    // 无效末项不影响前项；预览也不触发草稿提交，陈旧请求继续拒绝。
    auto invalid = base("units"); auto outside = item("unit", "u001"); outside.insert("x", 1025);
    invalid.insert("placements", QJsonArray{item("unit", "u001"), outside}); invalid.insert("dry_run", true);
    const auto beforeInvalid = f.Context(); const auto submits = draftSubmits;
    QVERIFY(!f.tools->Invoke("entities_create", invalid).value("ok").toBool());
    QCOMPARE(f.Context(), beforeInvalid); QCOMPARE(draftSubmits, submits); QCOMPARE(undo->count(), 3);
    QCOMPARE(ErrorCode(f.tools->Invoke("entities_create", previewArgs)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    undo->undo(); QCOMPARE(current().value->Units()->units.size(), std::size_t{4});
    const auto previewIdentity = f.Context(); auto redoPreview = base("units");
    redoPreview.insert("placements", QJsonArray{item("unit", "u001")}); redoPreview.insert("dry_run", true);
    QVERIFY(f.tools->Invoke("entities_create", redoPreview).value("ok").toBool());
    QVERIFY(undo->canRedo()); QCOMPARE(f.Context(), previewIdentity); QCOMPARE(draftSubmits, submits);
    undo->redo(); QCOMPARE(current().value->Units()->units.size(), std::size_t{2});
    undo->undo(); undo->undo(); undo->undo();
    const auto restored = current(); QVERIFY(restored.Succeeded());
    QVERIFY(restored.value->Member(MapDataKind::Units).sources[0].data == *unitBytes.value);
    QVERIFY(restored.value->Member(MapDataKind::Doodads).sources[0].data == *doodadBytes.value);
    QVERIFY(!originalSession->IsDirty());
    Mcp::ToolPolicy readOnly; readOnly.allowEdit = false; f.tools->SetPolicy(readOnly);
    const auto visibleTools = Names(f.tools->Tools());
    QVERIFY(!visibleTools.contains("entities_create") && !visibleTools.contains("entities_delete"));
    // 注册表保留已禁用工具；面向助手的发现列表和实际调用受权限约束。
    QVERIFY(f.tools->RegisteredToolNames().contains("entities_create") && f.tools->RegisteredToolNames().contains("entities_delete"));
    auto deniedPreview = base("units");
    deniedPreview.insert("placements", QJsonArray{item("unit", "u001")}); deniedPreview.insert("dry_run", true);
    const auto deniedIdentity = f.Context(); const auto deniedSubmits = draftSubmits;
    QVERIFY(!f.tools->Invoke("entities_create", deniedPreview).value("ok").toBool());
    QCOMPARE(f.Context(), deniedIdentity); QCOMPARE(draftSubmits, deniedSubmits);
    f.tools->SetPolicy({});
    QVERIFY(f.tools->Invoke("entities_create", deniedPreview).value("ok").toBool());
    QCOMPARE(f.Context(), deniedIdentity); QCOMPARE(draftSubmits, deniedSubmits);
    const auto reloaded = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(reloaded.Succeeded());
    QCOMPARE(reloaded.value->Snapshot().aggregateSha256, sourceSha);
    QCOMPARE(f.project.mapEdits_.get(), originalSession); QCOMPARE(f.project.mapDataUndoStack_, undo);
    QCOMPARE(f.Fingerprint(), triggerSha); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::ImportMetadataUsesSourceAndCurrentAssetOverlay() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(WriteFile(f.source.filePath("resource/alpha.bin"), "original opaque payload"));
    auto source = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(source.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    f.project.ResetEditTracking();
    const auto triggerSha = f.Fingerprint();
    const auto initial = f.tools->Invoke("imports_list", {{"query", "alpha"}});
    QVERIFY2(initial.value("ok").toBool(), qPrintable(Explain(initial)));
    QCOMPARE(initial.value("rows").toArray().size(), 1);
    const auto original = initial.value("rows").toArray()[0].toObject();
    QCOMPARE(original.value("state").toString(), QStringLiteral("present"));
    QCOMPARE(original.value("source_path").toString(), QStringLiteral("resource/alpha.bin"));
    QVERIFY(original.value("sha256").isString());
    const QJsonObject get{{"import_id", original.value("import_id")},
        {"expected_inventory_sha256", initial.value("inventory_sha256")}};
    QVERIFY(f.tools->Invoke("imports_get", get).value("ok").toBool());
    for (const auto& definition : Mcp::ImportToolDefinitions()) {
        const auto tools = f.tools->Tools();
        const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& item) {
            return item.toObject().value("name").toString() == definition.name;
        });
        QVERIFY(found != tools.end());
        QCOMPARE(found->toObject().value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
    }
    QtProject::AssetState assets;
    assets.edits.push_back({"resource/alpha.bin", "resource/beta.bin"});
    const auto pendingDisk = f.outputs.filePath("private-source-must-not-be-exposed.bin");
    QVERIFY(WriteFile(pendingDisk, "12345678901234567"));
    assets.imports.push_back({"queued.bin", pendingDisk, 17});
    f.project.RestoreAssets(assets);
    QVERIFY(f.project.editTrackingReady_ && !f.project.editTrackingFailed_);
    // 合法入队后移走原文件：只读元数据不得再次读取它，也不得伪造冻结SHA。
    QVERIFY(QFile::remove(pendingDisk));
    const auto overlay = f.tools->Invoke("imports_list", {});
    QVERIFY2(overlay.value("ok").toBool(), qPrintable(Explain(overlay)));
    QVERIFY(overlay.value("includes_unsaved_changes").toBool());
    bool moved = false, queued = false;
    for (const auto& item : overlay.value("rows").toArray()) {
        const auto row = item.toObject();
        if (row.value("source_path").toString() == "resource/alpha.bin") {
            moved = true;
            QCOMPARE(row.value("archive_path").toString(), QStringLiteral("resource/beta.bin"));
            QCOMPARE(row.value("sha256"), original.value("sha256"));
        }
        if (row.value("kind").toString() == "queued-unfrozen") {
            queued = true; QVERIFY(row.value("sha256").isNull());
            QCOMPARE(row.value("reported_bytes").toString(), QStringLiteral("17"));
        }
    }
    QVERIFY(moved && queued);
    QVERIFY(!QJsonDocument(overlay).toJson().contains("private-source-must-not-be-exposed"));
    QVERIFY(!f.tools->Invoke("imports_get", get).value("ok").toBool());
    QCOMPARE(f.Fingerprint(), triggerSha); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    QVERIFY(WriteFile(f.source.filePath("resource/alpha.bin"), "external source change"));
    QVERIFY(!f.tools->Invoke("imports_list", {}).value("ok").toBool());
}

void LiveEditorToolsTests::LuaFileToolsShareAssetUndoAndRejectStaleText() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const QByteArray original("print(1)\r\nprint(2)\r\n");
    QVERIFY(WriteFile(f.source.filePath("scripts/main.lua"), original));
    auto source = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(source.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    f.project.ResetEditTracking();
    for (const auto& definition : Mcp::LuaFileToolDefinitions()) {
        const auto tools = f.tools->Tools();
        const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& item) {
            return item.toObject().value("name").toString() == definition.name;
        });
        QVERIFY(found != tools.end());
        QCOMPARE(found->toObject().value("inputSchema").toObject(), WithBindingToken(definition.inputSchema));
    }
    const auto list = f.tools->Invoke("lua_files_list", f.Context());
    QVERIFY2(list.value("ok").toBool(), qPrintable(Explain(list)));
    QCOMPARE(list.value("files").toArray().size(), 1);
    const auto path = list.value("files").toArray()[0].toObject().value("path").toString();
    QVERIFY(path.endsWith(QStringLiteral("main.lua")));
    auto request = f.Context(); request.insert("path", path);
    const auto read = f.tools->Invoke("lua_file_read", request);
    QVERIFY2(read.value("ok").toBool(), qPrintable(Explain(read)));
    QCOMPARE(read.value("text").toString(), QString::fromUtf8(original));
    const auto revision = f.Context().value("expected_revision");

    auto missingSha = request; missingSha.insert("text", "print('unread overwrite')\n");
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", missingSha)), QString("LUA_FILE_WRITE_REJECTED"));
    auto equivalentPath = missingSha; equivalentPath.insert("path", QStringLiteral("SCRIPTS\\MAIN.LUA"));
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", equivalentPath)), QString("LUA_FILE_WRITE_REJECTED"));
    auto emptySha = missingSha; emptySha.insert("expected_sha256", "");
    // 外层 JSON-RPC schema 先于 Lua 工具适配器拒绝不匹配 sha256 pattern 的空字符串。
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", emptySha)), QString("LIVE_EDITOR_RPC_ERROR"));
    QCOMPARE(f.Context().value("expected_revision"), revision);
    QCOMPARE(f.project.AssetUndoStack()->count(), 0);
    QCOMPARE(f.tools->Invoke("lua_file_read", request).value("text").toString(), QString::fromUtf8(original));

    auto stale = request; stale.insert("text", "print(3)\n"); stale.insert("expected_sha256", QString(64, QLatin1Char('0')));
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", stale)), QString("LUA_FILE_WRITE_REJECTED"));
    QCOMPARE(f.Context().value("expected_revision"), revision);
    auto traversal = f.Context(); traversal.insert("path", "../outside.lua"); traversal.insert("text", "x");
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", traversal)), QString("LUA_FILE_ARGUMENT_INVALID"));
    auto created = f.Context(); created.insert("path", "scripts/new.lua"); created.insert("text", "x");
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", created)), QString("LUA_FILE_WRITE_REJECTED"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 0);

    auto write = f.Context(); write.insert("path", path); write.insert("text", "print(3)\nprint(4)\n");
    write.insert("expected_sha256", read.value("sha256"));
    const auto written = f.tools->Invoke("lua_file_write", write);
    QVERIFY2(written.value("ok").toBool(), qPrintable(Explain(written)));
    QVERIFY(written.value("changes").isObject());
    QCOMPARE(written.value("created").toBool(), false);
    QCOMPARE(written.value("changes").toObject().value("items").toArray()[0].toObject().value("before"), read.value("sha256"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    auto after = f.Context(); after.insert("path", path);
    // 原文件是 CRLF，写入后保持 CRLF。
    QCOMPARE(f.tools->Invoke("lua_file_read", after).value("text").toString(), QStringLiteral("print(3)\r\nprint(4)\r\n"));
    QCOMPARE(f.tools->Invoke("lua_file_read", after).value("sha256"), written.value("sha256"));
    // 再用旧版本身份写入必须拒绝。
    auto replay = write; replay.insert("expected_revision", f.Context().value("expected_revision"));
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", replay)), QString("LUA_FILE_WRITE_REJECTED"));
    f.project.AssetUndoStack()->undo();
    auto undone = f.Context(); undone.insert("path", path);
    QCOMPARE(f.tools->Invoke("lua_file_read", undone).value("text").toString(), QString::fromUtf8(original));
    f.project.AssetUndoStack()->redo();
    auto redone = f.Context(); redone.insert("path", path);
    QCOMPARE(f.tools->Invoke("lua_file_read", redone).value("text").toString(),
        QStringLiteral("print(3)\r\nprint(4)\r\n"));
    f.project.AssetUndoStack()->undo();
    undone = f.Context(); undone.insert("path", path);
    QCOMPARE(f.tools->Invoke("lua_file_read", undone).value("text").toString(),
        QString::fromUtf8(original));

    QCOMPARE(ErrorCode(f.tools->Invoke("map_test_start", f.Context())), QString("MAP_TEST_UNAVAILABLE"));
    int launches = 0;
    f.tools->SetMapTestHandler([&launches](const QString& name, const QJsonObject& arguments) {
        if (name != "map_test_start" || !arguments.contains("project_identity")) return QJsonObject{{"ok", false}};
        return ++launches == 1 ? QJsonObject{{"ok", true}, {"started", true}}
            : QJsonObject{{"ok", false}, {"code", "MAP_TEST_REJECTED"}, {"message", "没有找到游戏"}};
    });
    QVERIFY(f.tools->Invoke("map_test_start", f.Context()).value("started").toBool());
    QCOMPARE(ErrorCode(f.tools->Invoke("map_test_start", f.Context())), QString("MAP_TEST_REJECTED"));
    auto staleTest = f.Context(); staleTest.insert("expected_revision", "9999");
    QCOMPARE(ErrorCode(f.tools->Invoke("map_test_start", staleTest)), QString("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(launches, 2);
    // 测试场景参数先校验再交给宿主：start_seconds 不能单独出现，scenario 带 reads 原样转交。
    auto lonelyStart = f.Context(); lonelyStart.insert("start_seconds", 3);
    QCOMPARE(ErrorCode(f.tools->Invoke("map_test_start", lonelyStart)), QString("MAP_TEST_ARGUMENT_INVALID"));
    auto scenario = f.Context(); scenario.insert("scenario", QStringLiteral("Preload(\"通过\")"));
    scenario.insert("reads", QJsonArray{QStringLiteral("Count")}); scenario.insert("start_seconds", 2.5);
    QCOMPARE(ErrorCode(f.tools->Invoke("map_test_start", scenario)), QString("MAP_TEST_REJECTED"));
    QCOMPARE(launches, 3);
    // 超长场景直接说出上限和当前长度，不转交宿主。
    auto oversized = f.Context(); oversized.insert("scenario", QString(65537, QLatin1Char('x')));
    const auto tooLong = f.tools->Invoke("map_test_start", oversized);
    QCOMPARE(ErrorCode(tooLong), QString("MAP_TEST_ARGUMENT_INVALID"));
    QVERIFY(tooLong.value("diagnostics").toArray()[0].toObject().value("message").toString().contains("当前 65537 字符"));
    QCOMPARE(launches, 3);

    QtProject::AssetState ambiguous;
    ambiguous.edits.push_back({path, path}); ambiguous.edits.push_back({path, path});
    f.project.RestoreAssets(ambiguous);
    const auto entries = f.project.AssetEntries();
    QVERIFY(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return EditorProject::AssetKey(entry.path) == EditorProject::AssetKey(path) && entry.ambiguous;
    }));
    auto ambiguousWrite = f.Context(); ambiguousWrite.insert("path", path); ambiguousWrite.insert("text", "print(5)\n");
    const auto ambiguousResult = f.tools->Invoke("lua_file_write", ambiguousWrite);
    QCOMPARE(ErrorCode(ambiguousResult), QString("LUA_FILE_WRITE_REJECTED"));
    QVERIFY(!ambiguousResult.contains("created"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    QCOMPARE(f.project.AssetUndoStack()->index(), 0);
    f.project.RestoreAssets({});

    QVERIFY(WriteFile(f.source.filePath("scripts/main.lua"), "print(9)\r\nprint(2)\r\n"));
    auto changedSource = f.Context(); changedSource.insert("path", path); changedSource.insert("text", "print(5)\n");
    changedSource.insert("expected_sha256", read.value("sha256"));
    const auto denied = f.tools->Invoke("lua_file_write", changedSource);
    QVERIFY(!denied.value("ok").toBool());
    QVERIFY(!denied.contains("created"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    QCOMPARE(f.project.AssetUndoStack()->index(), 0);
    changedSource.remove("expected_sha256");
    const auto deniedWithoutSha = f.tools->Invoke("lua_file_write", changedSource);
    QVERIFY(!deniedWithoutSha.value("ok").toBool());
    QVERIFY(!deniedWithoutSha.contains("created"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    QCOMPARE(f.project.AssetUndoStack()->index(), 0);
}

void LiveEditorToolsTests::LuaFileToolsCreateOnlyAtProvenArchiveAbsence() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto source = std::filesystem::path(f.outputs.filePath("owned-lua-source.w3x").toStdWString());
    const std::array<MapArchiveMember, 1> members{{{"readme.txt", {std::byte{0x58}}}}};
    const auto written = WriteVerifiedArchive(source, members); QVERIFY(written.Succeeded());
    auto archive = MapArchiveSession::Open(source); QVERIFY(archive.Succeeded());
    f.project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*archive.value));
    f.project.sourceArchivePath_ = source;
    f.project.ResetEditTracking();
    f.tools = std::make_unique<LiveEditorTools>(f.project);
    auto create = f.Context(); create.insert("path", "scripts/new.lua"); create.insert("text", "return 42\n");
    auto inventedVersion = create; inventedVersion.insert("expected_sha256", QString(64, QLatin1Char('a')));
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_file_write", inventedVersion)), QString("LUA_FILE_WRITE_REJECTED"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 0);
    const auto created = f.tools->Invoke("lua_file_write", create);
    QVERIFY2(created.value("ok").toBool(), qPrintable(Explain(created)));
    QVERIFY(created.value("created").toBool());
    QVERIFY(created.value("changes").toObject().value("items").toArray()[0].toObject().value("before").isNull());
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    auto read = f.Context(); read.insert("path", "scripts/new.lua");
    QCOMPARE(f.tools->Invoke("lua_file_read", read).value("text").toString(), QStringLiteral("return 42\n"));
    f.project.AssetUndoStack()->undo();
    QCOMPARE(f.project.AssetUndoStack()->index(), 0);
    read = f.Context(); read.insert("path", "scripts/new.lua");
    QVERIFY(!f.tools->Invoke("lua_file_read", read).value("ok").toBool());
}

void LiveEditorToolsTests::ImportsAddQueuesOwnedBytesWithUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto source = std::filesystem::path(f.outputs.filePath("imports-source.w3x").toStdWString());
    const std::array<MapArchiveMember, 1> members{{{"war3mapImported\\old.mdx", {std::byte{0x4d}}}}};
    QVERIFY(WriteVerifiedArchive(source, members).Succeeded());
    auto archive = MapArchiveSession::Open(source); QVERIFY(archive.Succeeded());
    f.project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*archive.value));
    f.project.sourceArchivePath_ = source;
    f.project.ResetEditTracking();
    f.tools = std::make_unique<LiveEditorTools>(f.project);
    const auto disk = f.outputs.filePath("hero.mdx");
    QVERIFY(WriteFile(disk, "MDLX-original"));
    const auto request = [&](const QString& diskPath, const QString& archivePath) {
        auto value = f.Context();
        value.insert("files", QJsonArray{QJsonObject{{"disk_path", QDir::toNativeSeparators(diskPath)}, {"archive_path", archivePath}}});
        return value;
    };
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(disk, "scripts\\main.lua"))), QString("IMPORTS_ADD_ARGUMENT_INVALID"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request("hero.mdx", "war3mapImported\\hero.mdx"))), QString("IMPORTS_ADD_ARGUMENT_INVALID"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(disk, "..\\hero.mdx"))), QString("IMPORTS_ADD_ARGUMENT_INVALID"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(disk, "war3map.w3u"))), QString("IMPORTS_ADD_REJECTED"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(f.outputs.filePath("missing.mdx"), "war3mapImported\\x.mdx"))),
        QString("IMPORTS_ADD_FILE_UNREADABLE"));
    // 归档里已有的成员（包括清单外的）须明确替换。
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(disk, "WAR3MAPIMPORTED\\OLD.MDX"))), QString("IMPORTS_ADD_REJECTED"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 0);

    const auto added = f.tools->Invoke("imports_add", request(disk, "war3mapImported\\hero.mdx"));
    QVERIFY2(added.value("ok").toBool(), qPrintable(Explain(added)));
    const auto row = added.value("added").toArray()[0].toObject();
    QCOMPARE(row.value("archive_path").toString(), QStringLiteral("war3mapImported\\hero.mdx"));
    QCOMPARE(row.value("size").toInt(), 13);
    QVERIFY(!row.value("replaced").toBool());
    QCOMPARE(row.value("sha256").toString().size(), 64);
    QCOMPARE(f.project.AssetUndoStack()->count(), 1);
    // 调用时已读入内存，磁盘文件之后再变不影响待导入内容。
    QVERIFY(WriteFile(disk, "MDLX-changed-later"));
    const auto pending = [&]() -> std::optional<QtProject::AssetEntry> {
        for (const auto& entry : f.project.AssetEntries())
            if (EditorProject::AssetKey(entry.path) == EditorProject::AssetKey("war3mapImported\\hero.mdx")) return entry;
        return std::nullopt;
    };
    QVERIFY(pending() && pending()->ownedPayload && pending()->size == 13);
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(pending()->ownedPayload->data()), 13), QByteArray("MDLX-original"));

    QCOMPARE(ErrorCode(f.tools->Invoke("imports_add", request(disk, "war3mapImported\\hero.mdx"))), QString("IMPORTS_ADD_REJECTED"));
    auto replace = request(disk, "war3mapImported\\old.mdx"); replace.insert("replace_existing", true);
    const auto replaced = f.tools->Invoke("imports_add", replace);
    QVERIFY2(replaced.value("ok").toBool(), qPrintable(Explain(replaced)));
    QVERIFY(replaced.value("added").toArray()[0].toObject().value("replaced").toBool());
    QCOMPARE(f.project.AssetUndoStack()->count(), 2);

    // 删除：待导入文件移出队列，原图成员记为删除；整批一个撤销步骤。
    const auto removeRequest = [&](const QStringList& paths) {
        auto value = f.Context();
        value.insert("archive_paths", QJsonArray::fromStringList(paths));
        return value;
    };
    const auto hasOld = [&] {
        for (const auto& entry : f.project.AssetEntries())
            if (EditorProject::AssetKey(entry.path) == EditorProject::AssetKey("war3mapImported\\old.mdx")) return true;
        return false;
    };
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove", removeRequest({"scripts\\main.lua"}))), QString("IMPORTS_REMOVE_ARGUMENT_INVALID"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove", removeRequest({"war3map.w3u"}))), QString("IMPORTS_REMOVE_REJECTED"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove", removeRequest({"war3mapImported\\missing.mdx"}))), QString("IMPORTS_REMOVE_REJECTED"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove",
        removeRequest({"war3mapImported\\hero.mdx", "WAR3MAPIMPORTED/HERO.MDX"}))), QString("IMPORTS_REMOVE_REJECTED"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 2);
    // 删除前按当前内容查找路径：待导入的界面文本写着 hero.mdl（.mdl/.mdx 互通）时默认拒绝，确认后才删。
    const auto note = f.outputs.filePath("ref.txt");
    QVERIFY(WriteFile(note, "Art=war3mapImported\\\\Hero.mdl\n"));
    const auto noteAdded = f.tools->Invoke("imports_add", request(note, "war3mapImported\\ref.txt"));
    QVERIFY2(noteAdded.value("ok").toBool(), qPrintable(Explain(noteAdded)));
    QCOMPARE(f.project.AssetUndoStack()->count(), 3);
    auto badFlag = removeRequest({"war3mapImported\\hero.mdx"}); badFlag.insert("allow_referenced", "yes");
    // 非布尔值在协议层按 schema 拒绝，不进入删除。
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove", badFlag)), QString("LIVE_EDITOR_RPC_ERROR"));
    QCOMPARE(ErrorCode(f.tools->Invoke("imports_remove", removeRequest({"war3mapImported\\hero.mdx"}))),
        QString("IMPORTS_REMOVE_REFERENCED"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 3);
    auto allowed = removeRequest({"war3mapImported\\hero.mdx"}); allowed.insert("allow_referenced", true);
    const auto forced = f.tools->Invoke("imports_remove", allowed);
    QVERIFY2(forced.value("ok").toBool(), qPrintable(Explain(forced)));
    const auto mentions = forced.value("removed").toArray()[0].toObject().value("mentioned_in").toArray();
    QCOMPARE(mentions.size(), 1);
    QCOMPARE(mentions[0].toObject().value("member").toString(), QStringLiteral("war3mapImported\\ref.txt"));
    QCOMPARE(mentions[0].toObject().value("kind").toString(), QStringLiteral("ui-text"));
    QCOMPARE(f.project.AssetUndoStack()->count(), 4);
    f.project.AssetUndoStack()->undo();
    // 同一批删除的文件之间互相提到不算引用。
    const auto together = f.tools->Invoke("imports_remove", removeRequest({"war3mapImported\\hero.mdx", "war3mapImported\\ref.txt"}));
    QVERIFY2(together.value("ok").toBool(), qPrintable(Explain(together)));
    f.project.AssetUndoStack()->undo();
    f.project.AssetUndoStack()->undo();
    QCOMPARE(f.project.AssetUndoStack()->index(), 2);
    const auto removedResult = f.tools->Invoke("imports_remove",
        removeRequest({"war3mapImported\\hero.mdx", "war3mapImported\\old.mdx"}));
    QVERIFY2(removedResult.value("ok").toBool(), qPrintable(Explain(removedResult)));
    const auto removedRows = removedResult.value("removed").toArray();
    QCOMPARE(removedRows.size(), 2);
    QVERIFY(!removedRows[0].toObject().value("in_source_map").toBool());
    QVERIFY(removedRows[1].toObject().value("in_source_map").toBool());
    QVERIFY(!removedResult.value("references_checked").toBool());
    QCOMPARE(f.project.AssetUndoStack()->count(), 3);
    QVERIFY(!pending() && !hasOld());
    f.project.AssetUndoStack()->undo();
    QVERIFY(pending() && hasOld());

    f.project.AssetUndoStack()->undo();
    f.project.AssetUndoStack()->undo();
    QVERIFY(!pending());
}

void LiveEditorToolsTests::LuaFilesListPagesCapturedDirectoryAndRejectsStaleContinuation() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    for (int i = 129; i >= 0; --i) {
        const auto name = QStringLiteral("scripts/deep/part_%1.lua").arg(i, 3, 10, QLatin1Char('0'));
        QVERIFY(WriteFile(f.source.filePath(name), "return true\n"));
    }
    QVERIFY(WriteFile(f.source.filePath("scripts/ignored.txt"), "not Lua"));
    auto source = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(source.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    f.project.ResetEditTracking();
    const auto expected = QtProject::ListLuaScripts(f.project);
    QCOMPARE(expected.size(), std::size_t{130});
    const auto revision = f.Context().value("expected_revision");
    const auto rpc = f.tools->Server().HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
        {"id", QString(126, QLatin1Char('x'))}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "lua_files_list"}, {"arguments", f.Context()}}}}).toJson(QJsonDocument::Compact));
    QVERIFY(rpc && !rpc->error);
    QVERIFY(Mcp::SerializeResponse(*rpc).size() <= 65536);
    const auto rpcText = rpc->result.toObject().value("content").toArray()[0].toObject().value("text").toString();
    QCOMPARE(QJsonDocument::fromJson(rpcText.toUtf8()).object().value("files").toArray().size(), qsizetype{64});
    auto page = f.Context();
    QStringList actual;
    for (const int offset : {0, 64, 128}) {
        page.insert("offset", offset);
        const auto result = f.tools->Invoke("lua_files_list", page);
        QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
        QCOMPARE(result.value("total").toInt(), 130);
        QCOMPARE(result.value("offset").toInt(), offset);
        QCOMPARE(result.value("limit").toInt(), 64);
        QCOMPARE(result.value("project_identity"), page.value("project_identity"));
        QCOMPARE(result.value("revision"), revision);
        QCOMPARE(result.value("scope"), QJsonValue(QStringLiteral("captured-assets-and-overlay")));
        QCOMPARE(result.value("source_checked_in_this_call"), QJsonValue(false));
        QCOMPARE(result.value("complete").toBool(), offset == 128);
        const auto files = result.value("files").toArray();
        QCOMPARE(files.size(), offset == 128 ? qsizetype{2} : qsizetype{64});
        for (const auto& file : files) actual.push_back(file.toObject().value("path").toString());
        if (offset == 128) QVERIFY(result.value("next_offset").isNull());
        else QCOMPARE(result.value("next_offset").toInt(), offset + files.size());
        QCOMPARE(f.tools->Invoke("lua_files_list", page), result);
    }
    for (std::size_t i = 0; i < expected.size(); ++i) QCOMPARE(actual[static_cast<qsizetype>(i)], expected[i].path);
    QCOMPARE(f.Context().value("expected_revision"), revision);
    QCOMPARE(f.project.AssetUndoStack()->count(), 0);
    page.insert("offset", 130);
    const auto end = f.tools->Invoke("lua_files_list", page);
    QVERIFY(end.value("ok").toBool()); QVERIFY(end.value("files").toArray().isEmpty());
    QVERIFY(end.value("complete").toBool()); QVERIFY(end.value("next_offset").isNull());
    page.insert("offset", 131);
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_files_list", page)), QStringLiteral("LUA_FILES_OFFSET_RANGE"));
    page.insert("offset", 1); page.insert("limit", 1);
    const auto single = f.tools->Invoke("lua_files_list", page);
    QVERIFY(single.value("ok").toBool()); QCOMPARE(single.value("files").toArray().size(), qsizetype{1});
    QCOMPARE(single.value("next_offset").toInt(), 2);
    page.insert("offset", 64);
    QtProject::AssetState changed;
    changed.edits.push_back({"scripts/deep/part_000.lua", "scripts/deep/renamed.lua"});
    f.project.RestoreAssets(changed);
    QCOMPARE(ErrorCode(f.tools->Invoke("lua_files_list", page)), QStringLiteral("LIVE_EDITOR_REVISION_CONFLICT"));
    auto fresh = f.Context(); fresh.insert("offset", 0); fresh.insert("limit", 256);
    const auto renamed = f.tools->Invoke("lua_files_list", fresh);
    QVERIFY2(renamed.value("ok").toBool(), qPrintable(Explain(renamed)));
    QCOMPARE(renamed.value("files").toArray().size(), qsizetype{130});
    QCOMPARE(renamed.value("total").toInt(), 130);
    Fixture empty; QVERIFY2(empty.Initialize(), qPrintable(empty.error));
    const auto none = empty.tools->Invoke("lua_files_list", empty.Context());
    QVERIFY(none.value("ok").toBool()); QCOMPARE(none.value("total").toInt(), 0);
    QVERIFY(none.value("files").toArray().isEmpty()); QVERIFY(none.value("complete").toBool());
    QVERIFY(none.value("next_offset").isNull());
}

void LiveEditorToolsTests::ResourceToolsUseActualAsyncSourceAndRejectStaleResults() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QByteArray texture(21, '\0');
    texture[2] = 2; texture[12] = 1; texture[14] = 1; texture[16] = 24; texture[17] = 32;
    texture[18] = 17; texture[19] = 33; texture[20] = 65;
    QVERIFY(WriteFile(f.source.filePath("resource/icon.tga"), texture));
    auto source = DirectoryWorkspace::Open(f.project.projectRoot_); QVERIFY(source.Succeeded());
    f.project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    f.project.ResetEditTracking();
    const auto fingerprint = f.Fingerprint();
    const auto state = f.tools->Invoke("object_resource_state", {});
    QVERIFY2(state.value("ok").toBool(), qPrintable(Explain(state)));
    QJsonObject query{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"resource_revision", state.value("resource_revision")}, {"origin", "import"}, {"kind", "texture"}};
    auto result = f.tools->Invoke("object_resource_query", query);
    QCOMPARE(ErrorCode(result), QString("OBJECT_RESOURCE_INDEX_PENDING"));
    QTRY_VERIFY_WITH_TIMEOUT((result = f.tools->Invoke("object_resource_query", query)).value("ok").toBool(), 3000);
    QCOMPARE(result.value("rows").toArray().size(), 1);
    const auto value = result.value("rows").toArray()[0].toObject().value("value");
    QJsonObject inspect{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
        {"resource_revision", state.value("resource_revision")}, {"kind", "texture"}, {"value", value}};
    QCOMPARE(ErrorCode(f.tools->Invoke("object_resource_inspect", inspect)), QString("OBJECT_RESOURCE_INSPECT_PENDING"));
    QTRY_VERIFY_WITH_TIMEOUT(ErrorCode(result = f.tools->Invoke("object_resource_inspect", inspect)) != "OBJECT_RESOURCE_INSPECT_PENDING", 3000);
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QVERIFY(result.value("source_verified").toBool());
    QVERIFY(!result.value("payload_included").toBool());
    QCOMPARE(result.value("bytes").toString(), QString("21"));
    QCOMPARE(result.value("width").toInt(), 1); QCOMPARE(result.value("height").toInt(), 1);
    QCOMPARE(f.Fingerprint(), fingerprint); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    // 查过另一个资源后，先读完的结果仍直接取回，不会被挤掉重新排队。
    auto other = inspect; other["value"] = "resource/missing.tga";
    QCOMPARE(ErrorCode(f.tools->Invoke("object_resource_inspect", other)), QString("OBJECT_RESOURCE_INSPECT_PENDING"));
    QTRY_VERIFY_WITH_TIMEOUT(ErrorCode(f.tools->Invoke("object_resource_inspect", other)) != "OBJECT_RESOURCE_INSPECT_PENDING", 3000);
    QVERIFY2((result = f.tools->Invoke("object_resource_inspect", inspect)).value("ok").toBool(), qPrintable(Explain(result)));
    QCOMPARE(result.value("bytes").toString(), QString("21"));

    QVERIFY(f.tools->Invoke("editor_edit_trigger_graph", f.DescriptionRequest()).value("ok").toBool());
    QCOMPARE(ErrorCode(f.tools->Invoke("object_resource_inspect", inspect)), QString("OBJECT_RESOURCE_STALE"));
    const auto nextState = f.tools->Invoke("object_resource_state", {});
    QVERIFY(nextState.value("resource_revision") != state.value("resource_revision"));
    inspect["project_identity"] = nextState.value("project_identity");
    inspect["expected_revision"] = nextState.value("revision"); inspect["resource_revision"] = nextState.value("resource_revision");
    // 旧修订的结果不可复用；新身份重新后台读真实文件。
    QCOMPARE(ErrorCode(f.tools->Invoke("object_resource_inspect", inspect)), QString("OBJECT_RESOURCE_INSPECT_PENDING"));
    QTRY_VERIFY_WITH_TIMEOUT(ErrorCode(result = f.tools->Invoke("object_resource_inspect", inspect)) != "OBJECT_RESOURCE_INSPECT_PENDING", 3000);
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QtProject::AssetState renamed;
    renamed.edits.push_back({"resource/icon.tga", "resource/moved.tga"});
    f.project.RestoreAssets(renamed);
    const auto renamedState = f.tools->Invoke("object_resource_state", {});
    inspect["project_identity"] = renamedState.value("project_identity");
    inspect["expected_revision"] = renamedState.value("revision");
    inspect["resource_revision"] = renamedState.value("resource_revision");
    inspect["value"] = "resource/moved.tga";
    QCOMPARE(ErrorCode(f.tools->Invoke("object_resource_inspect", inspect)), QString("OBJECT_RESOURCE_INSPECT_PENDING"));
    QTRY_VERIFY_WITH_TIMEOUT(ErrorCode(result = f.tools->Invoke("object_resource_inspect", inspect)) != "OBJECT_RESOURCE_INSPECT_PENDING", 3000);
    QVERIFY2(result.value("ok").toBool(), qPrintable(Explain(result)));
    QCOMPARE(result.value("source_path").toString(), QString("resource/icon.tga"));
    QVERIFY(WriteFile(f.source.filePath("resource/icon.tga"), "changed"));
    QVERIFY(!f.tools->Invoke("object_resource_inspect", inspect).value("ok").toBool());
}

void LiveEditorToolsTests::ProjectSearchUsesInjectedOwnedPagesAndStrictRevision() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto unbound = f.tools->Invoke("project_search", {});
    QVERIFY(!unbound.value("ok").toBool()); QCOMPARE(ErrorCode(unbound), QStringLiteral("PROJECT_SEARCH_HOST_UNAVAILABLE"));
    std::string revision = "synthetic-live-context-1";
    int calls = 0;
    f.tools->SetProjectSearchQuery([&] { return revision; },
        [&](std::string_view expected, std::string_view query, const Services::ProjectSearchOptions& options,
            std::size_t pageLimit, const Services::ProjectSearchCursor* cursor) {
            ++calls;
            Services::ProjectSearchContext context;
            context.revision = revision; context.triggers = f.project.triggerDocument_.get(); context.triggerState = Services::ProjectSearchState::Ready;
            return Services::SearchProjectPage(context, expected, query, options, pageLimit, cursor);
        });
    const auto fingerprint = f.Fingerprint(); const auto editRevision = f.project.editRevision_.Revision();
    Mcp::ToolPolicy queryOnly; queryOnly.allowEdit = false; queryOnly.allowSave = false; queryOnly.allowExternal = false;
    f.tools->SetPolicy(queryOnly);
    const auto identity = f.tools->Invoke("editor_state", {}).value("project_identity");
    const QJsonObject request{{"project_identity", identity}, {"expected_context_revision", QString::fromStdString(revision)},
        {"query", "原始"}, {"page", QJsonObject{{"limit", 1}}}};
    const auto first = f.tools->Invoke("project_search", request);
    QVERIFY2(first.value("ok").toBool(), qPrintable(Explain(first))); QCOMPARE(calls, 1);
    QVERIFY(first.value("read_only").toBool()); QVERIFY(first.contains("source_checked") && !first.value("source_checked").toBool());
    QCOMPARE(first.value("scope").toString(), QStringLiteral("gui-live-document"));
    QCOMPARE(first.value("context_revision").toString(), QString::fromStdString(revision));
    QVERIFY(!first.value("rows").toArray().isEmpty());
    QCOMPARE(first.value("rows").toArray()[0].toObject().value("source_id").toString(), QString::number(f.trigger));
    // 文件监视只给“可能变化”：搜索自行做同一只读来源核对，通过即清除提示并照常返回。
    f.project.sourceChangeSuspected_ = true;
    const auto checked = f.tools->Invoke("project_search", request);
    QVERIFY2(checked.value("ok").toBool(), qPrintable(Explain(checked))); QCOMPARE(calls, 2);
    QVERIFY(checked.value("source_checked").toBool());
    QVERIFY(!f.project.sourceChangeSuspected_ && !f.project.sourceChangeConfirmed_);
    revision = "synthetic-live-context-2";
    const auto stale = f.tools->Invoke("project_search", request);
    QVERIFY(!stale.value("ok").toBool()); QCOMPARE(ErrorCode(stale), QStringLiteral("PROJECT_SEARCH_STALE")); QCOMPARE(calls, 2);
    f.tools->SetProjectSearchQuery([&] { return revision; },
        [&](std::string_view expected, std::string_view query, const Services::ProjectSearchOptions& options,
            std::size_t pageLimit, const Services::ProjectSearchCursor* cursor) {
            Services::ProjectSearchContext context; context.revision = revision;
            context.triggers = f.project.triggerDocument_.get(); context.triggerState = Services::ProjectSearchState::Ready;
            auto page = Services::SearchProjectPage(context, expected, query, options, pageLimit, cursor);
            revision = "synthetic-live-context-3"; return page;
        });
    const auto during = f.tools->Invoke("project_search", {{"query", "原始"}});
    QVERIFY(!during.value("ok").toBool()); QCOMPARE(ErrorCode(during), QStringLiteral("PROJECT_SEARCH_STALE"));
    queryOnly.allowQuery = false; f.tools->SetPolicy(queryOnly);
    QVERIFY(!f.tools->Invoke("project_search", {}).value("ok").toBool());
    QCOMPARE(f.Fingerprint(), fingerprint); QCOMPARE(f.project.editRevision_.Revision(), editRevision);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    // 来源确实变化时拒绝搜索并给出核对诊断，不读取已变化的来源。
    queryOnly.allowQuery = true; f.tools->SetPolicy(queryOnly);
    QVERIFY(WriteFile(f.source.filePath("map/external-change.txt"), "different"));
    f.project.sourceChangeSuspected_ = true;
    const auto changed = f.tools->Invoke("project_search", {{"query", "原始"}});
    QVERIFY(!changed.value("ok").toBool()); QCOMPARE(ErrorCode(changed), QStringLiteral("PROJECT_SEARCH_SOURCE_CHANGED"));
    QVERIFY(f.project.sourceChangeConfirmed_);
    f.tools->SetProjectSearchQuery({}, {});
}

void LiveEditorToolsTests::ProjectSearchCallbackReplacementStopsOldRequest() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto fingerprint = f.Fingerprint(); const auto revision = f.project.editRevision_.Revision();
    const auto query = [&](std::string_view expected, std::string_view text, const Services::ProjectSearchOptions& options,
        std::size_t limit, const Services::ProjectSearchCursor* cursor) {
        Services::ProjectSearchContext context; context.revision = "replacement-context";
        context.triggers = f.project.triggerDocument_.get(); context.triggerState = Services::ProjectSearchState::Ready;
        return Services::SearchProjectPage(context, expected, text, options, limit, cursor);
    };
    int oldQueries = 0;
    f.tools->SetProjectSearchQuery([&] {
        f.tools->SetProjectSearchQuery([] { return std::string("replacement-context"); }, query);
        return std::string("original-context");
    }, [&](auto, auto, const auto&, auto, const auto*) {
        ++oldQueries;
        return Result<Services::ProjectSearchPage>::Failure("Test", "OLD_CALLBACK_RAN", "旧 query 不应调用。");
    });
    const auto duringIdentity = f.tools->Invoke("project_search", {});
    QCOMPARE(ErrorCode(duringIdentity), QStringLiteral("PROJECT_SEARCH_HOST_CHANGED")); QCOMPARE(oldQueries, 0);
    QVERIFY(f.tools->Invoke("project_search", {}).value("ok").toBool());
    f.tools->SetProjectSearchQuery([] { return std::string("replacement-context"); },
        [&](std::string_view expected, std::string_view text, const Services::ProjectSearchOptions& options,
            std::size_t limit, const Services::ProjectSearchCursor* cursor) {
            ++oldQueries;
            auto page = query(expected, text, options, limit, cursor);
            f.tools->SetProjectSearchQuery([] { return std::string("replacement-context"); }, query);
            return page;
        });
    const auto duringQuery = f.tools->Invoke("project_search", {});
    QCOMPARE(ErrorCode(duringQuery), QStringLiteral("PROJECT_SEARCH_HOST_CHANGED")); QCOMPARE(oldQueries, 1);
    QVERIFY(f.tools->Invoke("project_search", {}).value("ok").toBool());
    QCOMPARE(f.Fingerprint(), fingerprint); QCOMPARE(f.project.editRevision_.Revision(), revision);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::ProjectSearchOwnerDeletionKeepsInvocationAlive() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto fingerprint = f.Fingerprint(); const auto revision = f.project.editRevision_.Revision();
    QSignalSpy events(f.tools.get(), &LiveEditorTools::Activity);
    f.tools->SetProjectSearchQuery([] { return std::string("owner-context"); },
        [&](std::string_view expected, std::string_view text, const Services::ProjectSearchOptions& options,
            std::size_t limit, const Services::ProjectSearchCursor* cursor) {
            Services::ProjectSearchContext context; context.revision = "owner-context";
            context.triggers = f.project.triggerDocument_.get(); context.triggerState = Services::ProjectSearchState::Ready;
            auto page = Services::SearchProjectPage(context, expected, text, options, limit, cursor);
            f.tools.reset();
            return page;
        });
    auto* invoked = f.tools.get();
    const auto result = invoked->Invoke("project_search", {});
    QVERIFY(!f.tools); QCOMPARE(ErrorCode(result), QStringLiteral("PROJECT_SEARCH_HOST_CHANGED"));
    QCOMPARE(events.size(), 1); QCOMPARE(events[0][0].toJsonObject().value("type").toString(), QStringLiteral("tool_start"));
    QCOMPARE(f.Fingerprint(), fingerprint); QCOMPARE(f.project.editRevision_.Revision(), revision);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0);
}

void LiveEditorToolsTests::ViewportToolsUseActualHostAndRejectStaleIdentity() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto state = f.tools->Invoke("editor_state", {});
    QJsonObject args{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")}};
    QCOMPARE(ErrorCode(f.tools->Invoke("world_viewport_camera_read", args)), QString("WORLD_VIEWPORT_UNSUPPORTED"));
    int calls = 0;
    QSignalSpy events(f.tools.get(), &QtApp::LiveEditorTools::Activity);
    f.tools->SetWorldViewportHandler([&calls](const QString&, const QJsonObject&) {
        ++calls;
        return QJsonObject{{"ok", true}, {"status", "ready"}, {"image", QJsonObject{{"data", "fixture-payload"}}}};
    });
    auto result = f.tools->Invoke("world_viewport_camera_read", args);
    QVERIFY(result.value("ok").toBool()); QVERIFY(result.contains("image")); QCOMPARE(calls, 1);
    const auto details = events.last()[0].toJsonObject().value("details").toObject();
    QVERIFY(!details.contains("image")); QVERIFY(details.value("image_returned").toBool());
    auto stale = args; stale["expected_revision"] = "9999";
    QCOMPARE(ErrorCode(f.tools->Invoke("world_viewport_camera_read", stale)), QString("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(calls, 1);
    auto invalid = args; invalid["path"] = "injected";
    QVERIFY(!f.tools->Invoke("world_viewport_camera_read", invalid).value("ok").toBool()); QCOMPARE(calls, 1);
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    QVERIFY(!Names(f.tools->Tools()).contains("world_viewport_camera_set"));
    QVERIFY(Names(f.tools->Tools()).contains("world_viewport_camera_read"));
}

void LiveEditorToolsTests::GameUiToolsUseHostAndValidateArguments() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto state = f.tools->Invoke("editor_state", {});
    QJsonObject args{{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")}};
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_view_read", args)), QString("GAME_UI_UNSUPPORTED"));
    QStringList received;
    f.tools->SetGameUiHandler([&received](const QString& name, const QJsonObject& arguments) {
        received.append(name + QLatin1Char(':') + arguments.value("race").toString());
        return QJsonObject{{"ok", true}};
    });
    auto set = args; set["race"] = "Orc"; set["add_roots"] = QJsonArray{"EscMenuMainPanel"};
    QVERIFY(f.tools->Invoke("game_ui_view_set", set).value("ok").toBool());
    QCOMPARE(received, QStringList{"game_ui_view_set:Orc"});
    // 取值越界、未知种类与未知参数在到达宿主前被拒绝。
    auto badRace = args; badRace["race"] = "Naga";
    QVERIFY(!f.tools->Invoke("game_ui_view_set", badRace).value("ok").toBool());
    auto badSize = args; badSize["width"] = 99999;
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_render", badSize)), QString("GAME_UI_ARGUMENT"));
    auto tooMany = args; QJsonArray roots; for (int i = 0; i < 17; ++i) roots.append(QStringLiteral("R%1").arg(i));
    tooMany["add_roots"] = roots;
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_view_set", tooMany)), QString("GAME_UI_ARGUMENT"));
    auto unknown = args; unknown["path"] = "injected";
    QVERIFY(!f.tools->Invoke("game_ui_nodes_query", unknown).value("ok").toBool());
    auto stale = args; stale["expected_revision"] = "9999";
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_view_read", stale)), QString("LIVE_EDITOR_REVISION_CONFLICT"));
    QCOMPARE(received.size(), qsizetype{1});
    // 模拟输入：数值、布尔与文字可用，嵌套对象、超长函数名在到达宿主前被拒绝。
    auto simulate = args; simulate["function"] = "UpdateBar";
    simulate["values"] = QJsonObject{{"health", 0.5}, {"if:12", true}, {"name", "Boss"}};
    QVERIFY(f.tools->Invoke("game_ui_simulate", simulate).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{2});
    auto nested = args; nested["values"] = QJsonObject{{"health", QJsonObject{}}};
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_simulate", nested)), QString("GAME_UI_ARGUMENT"));
    auto longName = args; longName["function"] = QString(300, QLatin1Char('F'));
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_simulate", longName)), QString("GAME_UI_ARGUMENT"));
    QCOMPARE(received.size(), qsizetype{2});
    // 界面描述预览：编号必填，显示隐藏须为布尔。
    auto preview = args; preview["description"] = "CastBar"; preview["state"] = ""; preview["values"] = QJsonObject{{"hp[2]", 0.5}};
    QVERIFY(f.tools->Invoke("game_ui_preview_set", preview).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{3});
    QVERIFY(!f.tools->Invoke("game_ui_preview_set", args).value("ok").toBool());
    auto badHidden = args; badHidden["show_hidden"] = "yes";
    QVERIFY(!f.tools->Invoke("game_ui_view_set", badHidden).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{3});
    // 编写：edits 须是编辑对象数组。
    auto edit = args; edit["description"] = "CastBar";
    edit["edits"] = QJsonArray{QJsonObject{{"op", "remove_frame"}, {"frame", "fill"}}};
    QVERIFY(f.tools->Invoke("game_ui_description_edit", edit).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{4});
    auto badEdits = edit; badEdits["edits"] = QJsonArray{"remove fill"};
    QVERIFY(!f.tools->Invoke("game_ui_description_edit", badEdits).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{4});
    // 生成触发器：编号必填，preview 须为布尔。
    auto triggers = args; triggers["description"] = "CastBar"; triggers["preview"] = true;
    QVERIFY(f.tools->Invoke("game_ui_description_triggers", triggers).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{5});
    auto badPreview = triggers; badPreview["preview"] = "yes";
    QVERIFY(!f.tools->Invoke("game_ui_description_triggers", badPreview).value("ok").toBool());
    QVERIFY(!f.tools->Invoke("game_ui_description_triggers", args).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{5});
    // 界面模拟：action 只能取列出的值，steps 与 wait_ms 有范围。
    auto simulation = args; simulation["action"] = "step"; simulation["description"] = "CastBar"; simulation["steps"] = 3;
    QVERIFY(f.tools->Invoke("game_ui_trigger_simulation", simulation).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{6});
    auto badAction = simulation; badAction["action"] = "run";
    QVERIFY(!f.tools->Invoke("game_ui_trigger_simulation", badAction).value("ok").toBool());
    auto badSteps = simulation; badSteps["steps"] = 0;
    QCOMPARE(ErrorCode(f.tools->Invoke("game_ui_trigger_simulation", badSteps)), QString("GAME_UI_ARGUMENT"));
    QVERIFY(!f.tools->Invoke("game_ui_trigger_simulation", args).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{6});
    // 内置界面系统已改成示例（examples/game-ui），编辑器不再提供生成工具与起始模板。
    for (const auto* removed : {"game_ui_cast_system", "game_ui_party_system", "game_ui_target_system", "game_ui_meter_system",
             "game_ui_action_bar_system", "game_ui_talent_system"}) {
        QVERIFY(!Names(f.tools->Tools()).contains(removed));
        QVERIFY(!f.tools->Invoke(removed, args).value("ok").toBool());
    }
    auto withPreset = args; withPreset["description"] = "Bar"; withPreset["preset"] = "castbar";
    QVERIFY(!f.tools->Invoke("game_ui_description_create", withPreset).value("ok").toBool());
    QCOMPARE(received.size(), qsizetype{6});
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_view_set"));
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_simulate"));
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_preview_set"));
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_description_edit"));
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_description_triggers"));
    QVERIFY(!Names(f.tools->Tools()).contains("game_ui_trigger_simulation"));
    QVERIFY(Names(f.tools->Tools()).contains("game_ui_description_read"));
    QVERIFY(Names(f.tools->Tools()).contains("game_ui_render"));
}

#include "TerrainBlockLiveCases.h"
#include "LiveTerrainResizeCases.h"
void LiveEditorToolsTests::RuntimeToolsRequireExplicitConsentAndShareUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const auto initial = f.tools->Invoke("map_runtime_query", {});
    QVERIFY2(initial.value("ok").toBool(), qPrintable(Explain(initial)));
    QVERIFY(!initial.value("approved").toBool()); QVERIFY(initial.value("packages").toArray().isEmpty());
    const auto oldRevision = f.project.editRevision_.Revision();
    const auto original = f.Fingerprint();
    const auto make = [&](const QJsonObject& query, const char* mode) {
        return QJsonObject{{"project_identity", query.value("project_identity")},
            {"expected_revision", query.value("revision")}, {"mode", mode}};
    };
    auto enable = make(initial, "enabled"); enable.insert("package_sha256", QString(64, 'a'));
    QCOMPARE(ErrorCode(f.tools->Invoke("map_runtime_set", enable)), QString("RUNTIME_CONSENT_REQUIRED"));
    enable.insert("approve_native_runtime", true);
    QCOMPARE(ErrorCode(Mcp::InvokeMapRuntimeTool("map_runtime_set", enable, {})), QString("RUNTIME_LIVE_HOST_REQUIRED"));
    QCOMPARE(ErrorCode(f.tools->Invoke("map_runtime_set", enable)), QString("RUNTIME_PACKAGE_MISSING"));
    auto injected = enable; injected.insert("package_path", "untrusted");
    QVERIFY(!Mcp::InvokeMapRuntimeTool("map_runtime_set", injected, {}).value("ok").toBool());
    auto invalid = enable; invalid.insert("expected_revision", "18446744073709551616");
    QCOMPARE(ErrorCode(Mcp::InvokeMapRuntimeTool("map_runtime_set", invalid, {})), QString("RUNTIME_ARGUMENT_INVALID"));
    for (const auto revision : {"01", "1\n", "-1", "1.0"}) {
        invalid = enable; invalid.insert("expected_revision", revision);
        QCOMPARE(ErrorCode(Mcp::InvokeMapRuntimeTool("map_runtime_set", invalid, {})), QString("RUNTIME_ARGUMENT_INVALID"));
    }
    invalid = enable; invalid.insert("package_sha256", QString(64, 'a') + "\n");
    QCOMPARE(ErrorCode(Mcp::InvokeMapRuntimeTool("map_runtime_set", invalid, {})), QString("RUNTIME_ARGUMENT_INVALID"));
    QCOMPARE(f.project.editRevision_.Revision(), oldRevision); QCOMPARE(f.project.triggerUndoStack_->count(), 0);

    const auto root = std::filesystem::path(f.outputs.filePath("runtime-package").toStdWString());
    QJsonArray files;
    const auto add = [&](const char* path, const char* member, const char* role, const QByteArray& bytes) {
        QVERIFY(WriteFile(QString::fromStdWString((root / path).wstring()), bytes));
        const auto sha = Sha256File(root / path); QVERIFY(sha.Succeeded());
        files.append(QJsonObject{{"path", path}, {"member", member}, {"role", role}, {"size", bytes.size()},
            {"sha256", QString::fromStdString(*sha.value)}});
    };
    add("boot.blob", "warvk_loader_raw.ai", "bootstrap", "original opaque fixture");
    add("payload.blob", "warvk_runtime.blp", "payload", "original payload fixture");
    add("notice.txt", "audastra/runtime/licenses/fixture.txt", "notice", "original notice fixture");
    add("client.j", "", "client", "original synthetic client");
    const QJsonObject manifest{{"schema", 1}, {"id", "original-test"}, {"version", "1.0.0"}, {"abi", 0x20000},
        {"scope", "internal"}, {"gameSha256", "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a"}, {"files", files}};
    QVERIFY(WriteFile(QString::fromStdWString((root / "manifest.json").wstring()), QJsonDocument(manifest).toJson(QJsonDocument::Compact)));
    const auto identity = Sha256File(root / "manifest.json"); QVERIFY(identity.Succeeded());
    f.project.runtimePackageSources_ = {{root, *identity.value, false}};
    const auto available = f.tools->Invoke("map_runtime_query", {}); QVERIFY(available.value("ok").toBool());
    QVERIFY(!available.value("approved").toBool());
    const auto package = available.value("packages").toArray().first().toObject();
    QVERIFY(package.value("internal_only").toBool()); QVERIFY(package.value("available").toBool());
    QVERIFY(!package.contains("root")); QVERIFY(!package.contains("path"));
    enable.insert("package_sha256", QString::fromStdString(*identity.value));
    const auto enabled = f.tools->Invoke("map_runtime_set", enable);
    QVERIFY2(enabled.value("ok").toBool(), qPrintable(Explain(enabled))); QVERIFY(enabled.value("approved").toBool());
    QCOMPARE(f.project.editRevision_.Revision(), oldRevision + 1); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    QCOMPARE(f.Fingerprint(), original);
    QVERIFY(!f.tools->Invoke("map_runtime_set", enable).value("ok").toBool());
    QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    auto disable = make(enabled, "disabled");
    const auto disabled = f.tools->Invoke("map_runtime_set", disable);
    QVERIFY(disabled.value("ok").toBool()); QVERIFY(!disabled.value("approved").toBool());
    f.project.triggerUndoStack_->undo();
    QVERIFY(f.tools->Invoke("map_runtime_query", {}).value("approved").toBool());
    f.project.triggerUndoStack_->undo();
    QVERIFY(!f.tools->Invoke("map_runtime_query", {}).value("approved").toBool());
    QVERIFY(f.project.CaptureAssets().imports.empty());
    Mcp::ToolPolicy policy; policy.allowEdit = false; f.tools->SetPolicy(policy);
    QVERIFY(!Names(f.tools->Tools()).contains("map_runtime_set")); QVERIFY(Names(f.tools->Tools()).contains("map_runtime_query"));
    const auto denied = f.tools->Invoke("map_runtime_set", make(f.tools->Invoke("editor_state", {}), "disabled"));
    QVERIFY(!denied.value("ok").toBool());
}

QTEST_GUILESS_MAIN(LiveEditorToolsTests)
#include "LiveEditorToolsTests.moc"

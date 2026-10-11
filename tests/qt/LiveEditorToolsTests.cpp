#include "LiveEditorTools.h"
#include "EditorProject.h"
#include "Commands/TriggerCommands.h"
#include "Commands/ObjectSnapshotCommand.h"
#include "McpObjectTools.h"
#include "McpVariableTools.h"
#include "McpMapDataTools.h"
#include "McpImportTools.h"
#include "McpObjectResourceTools.h"
#include "McpPlacementTools.h"
#include "McpTerrainEditTools.h"
#include "McpCodeCheckTools.h"
#include "McpCampaignTools.h"
#include "McpProjectSearchTools.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "../services/ObjectCommandsFixture.h"
#include "../MapDataFixtures.h"

#include "War3MapCore/Hash.h"
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
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <memory>

using namespace War3Map;
using namespace War3Map::Trigger;
using War3Map::QtApp::LiveEditorTools;
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

// 原创、经 WTG/WCT 序列化与解析建立 seal 的小样例；不读取作者地图。
// 外部 UI 与源目录分别冻结，便于区分定义变化与地图来源变化。
struct Fixture {
    QTemporaryDir source, ui, outputs;
    EditorProject project;
    std::unique_ptr<LiveEditorTools> tools;
    QString error;
    StableId trigger = 0;

    bool Initialize(bool selectedLayers = false) {
        if (!source.isValid() || !ui.isValid() || !outputs.isValid()) return Fail("temporary directories");
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
    void InstallObjects(bool missing = false) {
        Tests::ObjectCommandsFixture fixture;
        project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(fixture.standard));
        project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(fixture.schema));
        for (std::size_t i = 0; i < fixture.documents.size(); ++i) {
            if (!missing) project.objectDocuments_[i] = std::move(fixture.documents[i]);
            project.objectUndoStacks_[i] = new QUndoStack(&project);
            QObject::connect(project.objectUndoStacks_[i], &QUndoStack::indexChanged, &project,
                [this, i] { project.ObserveObjectEdit(static_cast<int>(i)); });
        }
        project.ResetEditTracking();
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
    void CampaignToolsShareSchemasAndPolicy();
    void BatchKeepsDocumentAndExistingUndoCommands();
    void StaleAndInvalidBatchesAreAtomic();
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
    void SettingsAndStringsKeepOriginalGuiUndoAndPolicy();
    void PlacementToolsKeepAuthorEditsAndOriginalUndo();
    void TerrainToolsKeepSessionPreviewAndOriginalUndo();
    void ImportMetadataUsesSourceAndCurrentAssetOverlay();
    void ProjectSearchUsesInjectedOwnedPagesAndStrictRevision();
    void ProjectSearchCallbackReplacementStopsOldRequest();
    void ProjectSearchOwnerDeletionKeepsInvocationAlive();
};

void LiveEditorToolsTests::OnlyLiveToolsAndActualCatalogAreExposed() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    const QStringList expected{"code.check", "create_campaign", "edit_campaign", "edit_terrain_stroke",
        "editor_edit_trigger_graph", "editor_get_ui_catalog", "editor_query_trigger_graph", "editor_save_map", "editor_state",
        "entities_copy", "entities_query", "entities_rotate", "entities_set_fields",
        "export_campaign_member",
        "feedback.draft", "feedback.list", "feedback.status",
        "get_terrain_edit_state",
        "imports_get", "imports_list",
        "map_settings_get", "map_settings_set",
        "object_resource_inspect", "object_resource_query", "object_resource_state",
        "objects_create", "objects_delete", "objects_field_options", "objects_get", "objects_list", "objects_set_fields",
        "open_campaign", "project_search", "query_campaign", "query_campaign_objects", "query_terrain_points", "redo_campaign", "save_campaign",
        "sounds_get", "sounds_list", "sounds_update",
        "strings_query", "strings_set",
        "undo_campaign", "update.status",
        "variables_create", "variables_delete", "variables_get", "variables_list", "variables_rename", "variables_update"};
    QCOMPARE(Names(f.tools->Tools()), expected);
    QCOMPARE(f.tools->RegisteredToolNames(), expected);
    QCOMPARE(expected.size(), qsizetype{48});
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
    QTRY_VERIFY_WITH_TIMEOUT((completed = f.tools->Invoke("code.check", request)).value("state").toString() == QStringLiteral("complete"), 2000);
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
            ++matches; QCOMPARE(tool.value("inputSchema").toObject(), definition.inputSchema);
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
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); f.InstallObjects();
    QVERIFY(f.project.editTrackingReady_ && !f.project.editTrackingFailed_);
    for (const auto& definition : Mcp::ObjectToolDefinitions()) {
        bool found = false;
        for (const auto& value : f.tools->Tools()) {
            const auto tool = value.toObject();
            if (tool.value("name") != definition.name) continue;
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), definition.inputSchema);
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
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error)); f.InstallObjects(true);
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
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), definition.inputSchema);
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
            QCOMPARE(entry.toObject().value("inputSchema").toObject(), definition.inputSchema); found = true;
        }
        QVERIFY(found);
    }
    auto request = f.Context(); request.insert("terrain_binding", state.value("terrain_binding"));
    request.insert("dry_run", true);
    request.insert("brush", QJsonObject{{"kind", "raise"}, {"size", 1}, {"strength", 16}});
    request.insert("points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}});
    const auto context = f.Context();
    const auto preview = f.tools->Invoke("edit_terrain_stroke", request);
    QVERIFY2(preview.value("ok").toBool(), qPrintable(Explain(preview)));
    QVERIFY(!preview.value("applied").toBool()); QCOMPARE(undo->count(), 0); QCOMPARE(session->Revision(), std::uint64_t{0});
    QCOMPARE(session->Terrain().get(), originalTerrain.get()); QCOMPARE(f.Context(), context);
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
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), definition.inputSchema);
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
            found = true; QCOMPARE(tool.value("inputSchema").toObject(), definition.inputSchema);
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
        QCOMPARE(found->toObject().value("inputSchema").toObject(), definition.inputSchema);
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

QTEST_GUILESS_MAIN(LiveEditorToolsTests)
#include "LiveEditorToolsTests.moc"

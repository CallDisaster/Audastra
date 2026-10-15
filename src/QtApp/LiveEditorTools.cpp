#include "LiveEditorTools.h"
#include "LiveEditorAuxiliaryTools.h"
#include "McpLuaTestTools.h"
#include "McpLuaFileTools.h"
#include "McpWorldViewportTools.h"
#include "McpCampaignTools.h"
#include "LiveObjectTools.h"
#include "EditorProject.h"
#include "ProjectLuaModules.h"
#include "ObjectNames.h"
#include "McpResources.h"
#include "McpToolInternal.h"
#include "McpTriggerGraphJson.h"
#include "McpObjectTools.h"
#include "McpVariableTools.h"
#include "McpMapDataTools.h"
#include "McpImportTools.h"
#include "McpObjectResourceTools.h"
#include "LiveObjectResourceTools.h"
#include "McpPlacementTools.h"
#include "McpAuxiliaryTools.h"
#include "McpTerrainEditTools.h"
#include "McpProductTools.h"
#include "McpCodeCheckTools.h"
#include "McpProjectSearchTools.h"
#include "Commands/MapDataCommands.h"
#include "Commands/TriggerCommands.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Workspace.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QScopedValueRollback>
#include <QThread>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>

#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

namespace War3Map::QtApp {
namespace {
using QtProject::EditorProject;
using Trigger::TriggerDocument;

QJsonObject StringSchema() { return {{"type", "string"}}; }
QJsonObject IdentityProperties() {
    return {{"project_identity", StringSchema()},
        {"expected_revision", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}}}};
}
QJsonObject Schema(QJsonObject properties, QJsonArray required = {}) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject Binding(const Trigger::TriggerGraphBinding& binding) {
    QJsonObject result{{"source_wtg_sha256", QString::fromStdString(binding.sourceWtgSha256)},
        {"source_wct_sha256", QString::fromStdString(binding.sourceWctSha256)},
        {"ui_catalog_sha256", QString::fromStdString(binding.uiCatalogSha256)},
        {"current_wtg_sha256", QString::fromStdString(binding.currentWtgSha256)},
        {"current_wct_sha256", QString::fromStdString(binding.currentWctSha256)}};
    if (!binding.mapObjectReferencesSha256.empty())
        result.insert("map_object_references_sha256", QString::fromStdString(binding.mapObjectReferencesSha256));
    return result;
}

QJsonValue DisplayText(const std::string& text) {
    if (text.size() <= 1024) return QString::fromStdString(text);
    const auto sha = Sha256(std::as_bytes(std::span(text.data(), text.size())));
    if (!sha.Succeeded()) throw std::runtime_error("无法捕获改动文字的身份。");
    return QJsonObject{{"preview", QString::fromUtf8(text.data(), 1024)}, {"bytes", QString::number(text.size())},
        {"sha256", QString::fromStdString(*sha.value)}, {"truncated", true}};
}

QJsonObject Record(const Trigger::CategoryRecord& record) {
    return {{"kind", "category"}, {"id", QString::number(record.id)}, {"name", DisplayText(record.name.text)},
        {"category_id", record.categoryId}, {"comment", record.comment}};
}
QJsonObject Record(const Trigger::VariableRecord& record) {
    return {{"kind", "variable"}, {"id", QString::number(record.id)}, {"name", DisplayText(record.name.text)},
        {"type", DisplayText(record.typeName.text)}, {"is_array", record.isArray}, {"array_size", record.size},
        {"initialized", record.hasDefault}, {"value", DisplayText(record.value.text)}, {"unknown", record.unknown}};
}
QJsonValue Indices(const std::vector<std::uint32_t>& values) {
    if (values.size() > 256) {
        const auto sha = Sha256(std::as_bytes(std::span(values)));
        if (!sha.Succeeded()) throw std::runtime_error("无法捕获改动容器的身份。");
        return QJsonObject{{"count", QString::number(values.size())}, {"sha256", QString::fromStdString(*sha.value)}, {"truncated", true}};
    }
    QJsonArray result; for (const auto value : values) result.append(static_cast<qint64>(value)); return result;
}
QJsonObject Record(const Trigger::TriggerRecord& record) {
    // 作者代码不复制到工具日志；整批核心仍逐字节保留已有 WCT。
    return {{"kind", "trigger"}, {"id", QString::number(record.id)}, {"name", DisplayText(record.name.text)},
        {"description", DisplayText(record.description.text)}, {"category_id", record.categoryId},
        {"enabled", record.enabled}, {"initially_closed", record.initiallyClosed}, {"run_on_init", record.runOnInit},
        {"type", record.type}, {"custom_text", record.customTextFlag}, {"ecas", Indices(record.ecas)}};
}
QJsonObject Record(const Trigger::EcaNode& record) {
    return {{"kind", "eca"}, {"id", QString::number(record.id)}, {"name", DisplayText(record.name.text)},
        {"trigger_id", QString::number(record.sourceTriggerId)}, {"type", record.type}, {"enabled", record.enabled},
        {"branch", record.childId ? QJsonValue(*record.childId) : QJsonValue(QJsonValue::Null)},
        {"arguments", Indices(record.arguments)}, {"children", Indices(record.children)}};
}
QJsonObject Record(const Trigger::ArgumentNode& record) {
    return {{"kind", "argument"}, {"id", QString::number(record.id)}, {"value", DisplayText(record.value.text)},
        {"type", record.type}, {"insert_call", record.insertCall}, {"insert_index", record.insertIndex},
        {"call", record.callEca ? QJsonValue(static_cast<qint64>(*record.callEca)) : QJsonValue(QJsonValue::Null)},
        {"index", record.indexArgument ? QJsonValue(static_cast<qint64>(*record.indexArgument)) : QJsonValue(QJsonValue::Null)}};
}

struct RecordChanges {
    qint64 added = 0, removed = 0, changed = 0, triggerNodes = 0;
    qsizetype exampleBytes = 0;
    QJsonArray examples;
    template<class T> void Compare(const std::vector<T>& before, const std::vector<T>& after, bool nodes) {
        std::map<StableId, const T*> old;
        for (const auto& record : before) old.emplace(record.id, &record);
        for (const auto& record : after) {
            const auto found = old.find(record.id);
            const auto next = Record(record);
            if (found == old.end()) {
                ++added; if (nodes) ++triggerNodes;
                Example(QJsonValue(QJsonValue::Null), next);
            } else {
                const auto previous = Record(*found->second);
                if (previous != next) {
                    ++changed; if (nodes) ++triggerNodes;
                    Example(previous, next);
                }
                old.erase(found);
            }
        }
        for (const auto& [id, record] : old) {
            (void)id;
            ++removed; if (nodes) ++triggerNodes;
            Example(Record(*record), QJsonValue(QJsonValue::Null));
        }
    }
    void Example(QJsonValue before, QJsonValue after) {
        if (examples.size() >= 32) return;
        const QJsonObject example{{"before", before}, {"after", after}};
        const auto bytes = QJsonDocument(example).toJson(QJsonDocument::Compact).size();
        if (bytes > 64 * 1024 - exampleBytes) return;
        exampleBytes += bytes;
        examples.append(example);
    }
};

std::map<StableId, StableId> ArgumentOwners(const TriggerDocument& document) {
    std::map<StableId, StableId> result;
    for (const auto& eca : document.ecaNodes) {
        for (auto index : eca.arguments) {
            // 数组下标也是参数节点；嵌套调用的参数由各自 ECA 的真实归属取得。
            // 不根据参数 ID 或位置猜测触发器，异常/歧义归属不给跳转目标。
            for (std::size_t depth = 0; depth <= 128 && index < document.argumentNodes.size(); ++depth) {
                const auto& argument = document.argumentNodes[index];
                const auto [found, inserted] = result.emplace(argument.id, eca.sourceTriggerId);
                if (!inserted && found->second != eca.sourceTriggerId) found->second = 0;
                if (!argument.indexArgument) break;
                index = *argument.indexArgument;
            }
        }
    }
    return result;
}

QString DisplayValue(const QJsonValue& value) {
    return value.isString() ? value.toString() : value.toObject().value("preview").toString();
}

QJsonArray ChangeItems(const RecordChanges& changes, const TriggerDocument& before, const TriggerDocument& after) {
    const bool hasArguments = std::any_of(changes.examples.begin(), changes.examples.end(), [](const QJsonValue& value) {
        const auto example = value.toObject();
        const auto record = example.value(example.value("after").isObject() ? "after" : "before").toObject();
        return record.value("kind") == QStringLiteral("argument");
    });
    const auto oldOwners = hasArguments ? ArgumentOwners(before) : std::map<StableId, StableId>{};
    const auto newOwners = hasArguments ? ArgumentOwners(after) : std::map<StableId, StableId>{};
    std::map<StableId, QString> triggerNames;
    for (const auto& trigger : after.triggers) triggerNames.emplace(trigger.id, DisplayValue(DisplayText(trigger.name.text)));
    QJsonArray result;
    for (const auto& exampleValue : changes.examples) {
        const auto example = exampleValue.toObject();
        const bool present = example.value("after").isObject();
        const auto record = example.value(present ? "after" : "before").toObject();
        const auto recordKind = record.value("kind").toString();
        const auto recordId = record.value("id").toString();
        const auto id = static_cast<StableId>(recordId.toULongLong());
        QString kind = recordKind, objectId = present ? recordId : QString{};
        QString title = DisplayValue(record.value("name"));
        if (title.isEmpty()) title = DisplayValue(record.value("value"));
        if (title.isEmpty()) title = recordKind + QLatin1Char(' ') + recordId;
        QString target = title;
        StableId owner = 0;
        if (recordKind == QStringLiteral("eca")) owner = static_cast<StableId>(record.value("trigger_id").toString().toULongLong());
        if (recordKind == QStringLiteral("argument")) {
            const auto& owners = present ? newOwners : oldOwners;
            if (const auto found = owners.find(id); found != owners.end()) owner = found->second;
        }
        if (recordKind == QStringLiteral("eca") || recordKind == QStringLiteral("argument")) {
            objectId.clear();
            if (const auto trigger = triggerNames.find(owner); owner && trigger != triggerNames.end()) {
                kind = QStringLiteral("trigger"); objectId = QString::number(owner);
                target = trigger->second + QStringLiteral(" / ") + title;
            }
        }
        result.append(QJsonObject{{"kind", kind}, {"object_id", objectId}, {"record_kind", recordKind}, {"record_id", recordId},
            {"source_trigger_id", owner ? QString::number(owner) : QString{}}, {"title", title}, {"target", target},
            {"before", example.value("before")}, {"after", example.value("after")}});
    }
    return result;
}


const char* RecordKind(Trigger::GraphRecordKind kind) {
    switch (kind) {
    case Trigger::GraphRecordKind::Category: return "category";
    case Trigger::GraphRecordKind::Variable: return "variable";
    case Trigger::GraphRecordKind::Trigger: return "trigger";
    case Trigger::GraphRecordKind::Eca: return "eca";
    case Trigger::GraphRecordKind::Argument: return "argument";
    }
    return "unknown";
}
} // namespace

class LiveEditorTools::Impl {
public:
    Impl(EditorProject& project, LiveEditorTools& owner) : project_(project), owner_(owner), ownerHandle_(&owner) {
        objectResources_ = std::make_unique<LiveObjectResourceTools>(project_);
        RegisterTools();
        QObject::connect(&project_, &EditorProject::TriggerDocumentChanged, &owner_, [this] { emit owner_.Changed(); });
        QObject::connect(&project_, &EditorProject::ObjectDocumentsReplaced, &owner_, [this] { emit owner_.Changed(); });
        QObject::connect(&project_, &EditorProject::MapDocumentChanged, &owner_, [this] { emit owner_.Changed(); });
        QObject::connect(&project_, &EditorProject::SourceStateChanged, &owner_, [this] { emit owner_.Changed(); });
    }

    Mcp::McpServer& Server() noexcept { return server_; }
    void SetSaveHandler(std::function<bool(const std::filesystem::path&, bool)> handler) { save_ = std::move(handler); }
    void SetLuaTestHandler(std::function<QJsonObject(bool)> handler) { luaTest_ = std::move(handler); }
    void SetMapTestHandler(std::function<QString()> handler) { mapTest_ = std::move(handler); }
    void SetWorldViewportHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
        worldViewport_ = std::move(handler);
    }
    void SetBeforeWrite(std::function<bool()> handler) { beforeWrite_ = std::move(handler); }
    void SetProjectSearchQuery(ProjectSearchRevisionQuery revision, ProjectSearchPageQuery query) {
        // 每次重绑定切换不可复用的 token；执行中的 callback 有自己的拥有副本。
        auto token = std::make_shared<const int>(0);
        projectSearchRevision_ = std::move(revision); projectSearchQuery_ = std::move(query);
        projectSearchToken_ = std::move(token);
    }
    void SetPolicy(Mcp::ToolPolicy policy) { server_.SetToolPolicy(std::move(policy)); }
    QStringList RegisteredToolNames() const { return registeredNames_; }

    QJsonObject Invoke(const QString& name, const QJsonObject& arguments) {
        if (QThread::currentThread() != owner_.thread()) return Mcp::Failure("LIVE_EDITOR_THREAD", "活动地图工具必须在编辑器线程执行。");
        const auto response = server_.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1},
            {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}}).toJson(QJsonDocument::Compact));
        if (!response) return Mcp::Failure("LIVE_EDITOR_RESPONSE_MISSING", "工具调用未返回结果。");
        if (response->error) {
            const auto data = response->error->value("data").toObject();
            const auto code = data.value("code").toString(QStringLiteral("LIVE_EDITOR_RPC_ERROR"));
            return Mcp::Failure(code.toStdString(), response->error->value("message").toString().toStdString());
        }
        const auto content = response->result.toObject().value("content").toArray();
        if (content.isEmpty()) return Mcp::Failure("LIVE_EDITOR_RESPONSE_MISSING", "工具结果缺少结构化正文。");
        const auto parsed = QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8());
        if (!parsed.isObject()) return Mcp::Failure("LIVE_EDITOR_RESPONSE_INVALID", "工具结果不是结构化对象。");
        auto body = parsed.object();
        for (const auto& block : content) {
            const auto image = block.toObject();
            if (image.value("type") == QJsonValue("image") && image.value("mimeType") == QJsonValue("image/png"))
                body.insert("image", QJsonObject{{"mime_type", "image/png"}, {"encoding", "base64"}, {"data", image.value("data")}});
        }
        return body;
    }

    QJsonArray Tools() {
        const auto response = server_.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
        return response ? response->result.toObject().value("tools").toArray() : QJsonArray{};
    }

private:
    void SyncIdentity() {
        const auto root = project_.projectRoot_.lexically_normal();
        const auto archive = project_.sourceArchivePath_.lexically_normal();
        const auto uiRoot = project_.ResolveUiDataRoot().lexically_normal();
        const auto generation = project_.editRevision_.Generation();
        if (projectIdentity_.isEmpty() || root != root_ || archive != archive_ || generation != generation_ ||
            project_.triggerDocument_.get() != document_ || project_.uiCatalog_.get() != catalog_ || uiRoot != uiRoot_) {
            projectIdentity_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
            root_ = root; archive_ = archive; uiRoot_ = uiRoot; generation_ = generation;
            document_ = project_.triggerDocument_.get(); catalog_ = project_.uiCatalog_.get();
            configSha_.clear();
            if (!uiRoot_.empty()) {
                const auto sha = Sha256File(Trigger::UiCatalog::ResolveUiRoot(uiRoot_) / "config");
                if (sha.Succeeded()) configSha_ = *sha.value;
            }
        }
    }

    QJsonObject State() {
        SyncIdentity();
        const bool opened = project_.archiveSession_ != nullptr || project_.directorySnapshot_ != nullptr;
        const auto sourceSha = project_.archiveSession_ ? project_.archiveSession_->SourceSha256() :
            project_.directorySnapshot_ ? project_.directorySnapshot_->Snapshot().aggregateSha256 : std::string{};
        return {{"ok", true}, {"scope", "gui-live-document"}, {"project_identity", projectIdentity_},
            {"document_generation", QString::number(generation_)}, {"revision", QString::number(project_.editRevision_.Revision())},
            {"opened", opened}, {"busy", busy_ || project_.savingMap_},
            {"edit_tracking_ready", project_.editTrackingReady_ && !project_.editTrackingFailed_},
            {"trigger_tools_ready", project_.triggerDocument_ != nullptr && project_.uiCatalog_ != nullptr},
            {"map", QString::fromStdWString((archive_.empty() ? root_ : archive_).wstring())},
            {"source_sha256", QString::fromStdString(sourceSha)},
            {"source_change_suspected", project_.sourceChangeSuspected_},
            {"ui_catalog_sha256", catalog_ ? QString::fromStdString(catalog_->Sha256()) : QString{}},
            {"source_checked_in_this_call", false}};
    }

    QJsonObject CheckContext(const QJsonObject& arguments, bool triggers, bool verifySources) {
        SyncIdentity();
        if (arguments.value("project_identity").toString() != projectIdentity_ ||
            arguments.value("expected_revision").toString() != QString::number(project_.editRevision_.Revision()))
            return Mcp::Failure("LIVE_EDITOR_REVISION_CONFLICT", "活动工程或编辑修订已变化；重新读取 editor_state 和当前图。");
        if (!project_.editTrackingReady_ || project_.editTrackingFailed_ ||
            (!project_.archiveSession_ && !project_.directorySnapshot_))
            return Mcp::Failure("LIVE_EDITOR_NOT_READY", "活动地图缺少可信来源快照或编辑修订。");
        if (triggers && (!project_.triggerDocument_ || !project_.uiCatalog_))
            return Mcp::Failure("LIVE_EDITOR_TRIGGERS_UNAVAILABLE", "当前地图未加载完整的触发器和 UI 定义。");
        if (verifySources) {
            const auto sources = project_.CheckProjectSources();
            if (!sources.Succeeded()) return Mcp::Failure(sources);
            if (project_.uiCatalog_) {
                const auto config = Sha256File(Trigger::UiCatalog::ResolveUiRoot(uiRoot_) / "config");
                if (!config.Succeeded() || configSha_.empty() || *config.value != configSha_)
                    return Mcp::Failure("LIVE_EDITOR_UI_SOURCE_CHANGED", "UI config 来源已变化；重新加载定义后再编辑。");
                // Load 和 LoadLayers 的 config 字节不同：只比较真实对应的 Catalog，
                // 不把用户选定的有效层强行改成磁盘默认层。
                const auto configured = Trigger::UiCatalog::Load(uiRoot_);
                const bool direct = configured.Succeeded() && configured.value->Sha256() == project_.uiCatalog_->Sha256();
                if (!direct) {
                    const auto effective = Trigger::UiCatalog::LoadLayers(uiRoot_, project_.uiCatalog_->Layers());
                    if (!effective.Succeeded() || effective.value->Sha256() != project_.uiCatalog_->Sha256())
                        return Mcp::Failure("LIVE_EDITOR_UI_SOURCE_CHANGED", "生效 UI 定义或遮蔽关系已变化；不能使用旧签名写入。");
                }
            }
        }
        return {{"ok", true}};
    }

    QJsonObject Write(const QJsonObject& arguments, bool triggers, const std::function<QJsonObject()>& action) {
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图保存或另一个写命令正在执行。");
        QScopedValueRollback<bool> lease(busy_, true);
        if (beforeWrite_ && !beforeWrite_()) return Mcp::Failure("LIVE_EDITOR_DRAFT_REJECTED", "当前界面草稿未能提交；地图未修改。");
        const auto check = CheckContext(arguments, triggers, true);
        if (!check.value("ok").toBool()) return check;
        return action();
    }

    QJsonObject Query(const QJsonObject& arguments) {
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行；稍后重新查询。");
        const auto check = CheckContext(arguments, true, false);
        if (!check.value("ok").toBool()) return check;
        auto request = arguments;
        request.remove("project_identity"); request.remove("expected_revision");
        const auto references = project_.CaptureTriggerMapObjectReferences();
        if (request.value("table") == QStringLiteral("map_objects") && !references.Succeeded()) return Mcp::Failure(references);
        const auto* objects = references.Succeeded() && references.value && !(*references.value)->References().empty()
            ? references.value->get() : nullptr;
        auto result = Mcp::QueryGraphDocument(*project_.triggerDocument_, *project_.uiCatalog_, objects, request,
            [this](std::string_view raw, std::string_view type) { return ResolveProjectName(project_, raw, type); });
        result.insert("project_identity", projectIdentity_);
        result.insert("revision", QString::number(project_.editRevision_.Revision()));
        result.insert("document_generation", QString::number(generation_));
        result.insert("content_scope", "gui-visible-document");
        result.insert("map_object_reference_diagnostics", Mcp::DiagnosticsJson(references.diagnostics).value("diagnostics"));
        return result;
    }

    Result<Trigger::TriggerGraphEditReceipt> ApplyGraphBatch(const Trigger::TriggerGraphEditBatch& batch,
        const Mcp::VariablePrepublishValidator& prepublish = {}, QJsonObject* changesResult = nullptr) {
        using Receipt = Trigger::TriggerGraphEditReceipt;
        if (!project_.triggerUndoStack_)
            return Result<Receipt>::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE", "当前工程没有共用触发器撤销栈。");
        const auto references = project_.CaptureTriggerMapObjectReferences();
        if (!batch.expected.mapObjectReferencesSha256.empty() && !references.Succeeded()) {
            Result<Receipt> failure; failure.diagnostics = references.diagnostics; return failure;
        }
        const auto* objects = references.Succeeded() && references.value && !(*references.value)->References().empty()
            ? references.value->get() : nullptr;
        TriggerDocument before(*project_.triggerDocument_);
        TriggerDocument candidate(before);
        auto edited = Trigger::ApplyTriggerGraphEditBatch(candidate, *project_.uiCatalog_, batch, {}, objects);
        if (!edited.Succeeded()) return edited;
        RecordChanges changes;
        changes.Compare(before.categories, candidate.categories, false);
        changes.Compare(before.variables, candidate.variables, false);
        changes.Compare(before.triggers, candidate.triggers, true);
        changes.Compare(before.ecaNodes, candidate.ecaNodes, true);
        changes.Compare(before.argumentNodes, candidate.argumentNodes, true);
        const QJsonObject payload{{"trigger_nodes", changes.triggerNodes}, {"added_records", changes.added},
            {"removed_records", changes.removed}, {"changed_records", changes.changed}, {"examples", changes.examples},
            {"items", ChangeItems(changes, before, candidate)},
            {"examples_complete", changes.added + changes.removed + changes.changed == changes.examples.size()}};
        if (prepublish) {
            const auto checked = prepublish(before, candidate, *edited.value, payload);
            if (!checked.Succeeded()) {
                Result<Receipt> failure; failure.diagnostics = checked.diagnostics; return failure;
            }
        }
        if (edited.value->before.currentWtgSha256 != edited.value->after.currentWtgSha256 ||
            edited.value->before.currentWctSha256 != edited.value->after.currentWctSha256) {
            auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(
                project_, std::move(before), std::move(candidate), QObject::tr("AI 编辑触发器"));
            if (!command->Ready()) return Result<Receipt>::Failure("QtApp.Live", "TRIGGER_GRAPH_UNDO_SOURCE_STALE",
                "来源通知、会话或函数目录已变化；未发布旧事务、未添加撤销命令。");
            const auto published = command->FirstPublication();
            project_.triggerUndoStack_->push(command.release());
            if (!*published) return Result<Receipt>::Failure("QtApp.Live", "TRIGGER_GRAPH_PUBLISH_FAILED",
                "完整事务未能发布；未返回成功编辑回执。");
        }
        if (changesResult) *changesResult = payload;
        return edited;
    }

    QJsonObject Edit(const QJsonObject& arguments) {
        return Write(arguments, true, [&] {
            auto request = arguments;
            request.remove("project_identity"); request.remove("expected_revision");
            const auto decoded = Mcp::DecodeGraphBatch(request);
            if (!decoded.Succeeded()) return Mcp::Failure(decoded);
            QJsonObject changes;
            const auto edited = ApplyGraphBatch(*decoded.value, {}, &changes);
            if (!edited.Succeeded()) return Mcp::Failure(edited);
            QJsonObject created;
            for (const auto& [key, ref] : edited.value->created) created.insert(QString::fromStdString(key),
                QJsonObject{{"kind", RecordKind(ref.kind)}, {"id", QString::number(ref.id)}});
            return QJsonObject{{"ok", true}, {"scope", "gui-visible-document"}, {"project_identity", projectIdentity_},
                {"revision", QString::number(project_.editRevision_.Revision())}, {"before", Binding(edited.value->before)},
                {"binding", Binding(edited.value->after)}, {"applied_operations", QString::number(edited.value->appliedOperations)},
                {"created", created}, {"saved", false}, {"runtime_verified", false}, {"changes", changes}};
        });
    }

    QJsonObject VariableTool(const QString& name, const QJsonObject& arguments, bool write) {
        const auto invoke = [&] {
            Mcp::VariableToolHost host;
            host.identity = [this] { return State(); };
            host.context = [this, write]() -> Result<Mcp::VariableToolContext> {
                Mcp::VariableToolContext context;
                context.document = project_.triggerDocument_.get();
                context.catalog = project_.uiCatalog_.get();
                const auto references = project_.CaptureTriggerMapObjectReferences();
                // 与原 Query/ApplyGraphBatch 使用同一 binding 输入；空资格目录也有 SHA，
                // 但 GUI 原图命令不传空目录，不能由 facade 单独把它算入预期身份。
                if (references.Succeeded() && references.value && !(*references.value)->References().empty())
                    context.mapObjects = *references.value;
                if (!write) context.nameResolver = [this](std::string_view raw, std::string_view type) {
                    return ResolveProjectName(project_, raw, type);
                };
                return Result<Mcp::VariableToolContext>::Success(std::move(context));
            };
            host.apply = [this](const Trigger::TriggerGraphEditBatch& batch, const Mcp::VariablePrepublishValidator& guard) {
                return ApplyGraphBatch(batch, guard);
            };
            return Mcp::InvokeVariableTool(name, arguments, host);
        };
        if (write) return Write(arguments, true, invoke);
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行；稍后重新查询。");
        SyncIdentity();
        if (!project_.editTrackingReady_ || project_.editTrackingFailed_ ||
            (!project_.archiveSession_ && !project_.directorySnapshot_))
            return Mcp::Failure("LIVE_EDITOR_NOT_READY", "活动地图缺少可信来源快照或编辑修订。");
        if (!project_.triggerDocument_ || !project_.uiCatalog_)
            return Mcp::Failure("LIVE_EDITOR_TRIGGERS_UNAVAILABLE", "当前地图未加载完整的触发器和 UI 定义。");
        return invoke();
    }

    Result<Mcp::MapDataToolContext> VisibleMapDataContext() {
        if (!project_.mapDocument_ || !project_.mapEdits_)
            return Result<Mcp::MapDataToolContext>::Failure("QtApp.Live", "LIVE_EDITOR_MAP_DATA_UNAVAILABLE",
                "当前地图没有共用地图文档和原编辑会话。");
        auto visible = MapData::ProjectMapData(*project_.mapDocument_, *project_.mapEdits_);
        if (!visible.Succeeded()) return {{}, visible.diagnostics};
        Mcp::MapDataToolContext context;
        context.original = std::make_shared<const MapDocument>(*project_.mapDocument_);
        context.document = std::make_shared<const MapDocument>(std::move(*visible.value));
        const auto identity = State();
        context.binding = {projectIdentity_.toStdString(), generation_, project_.editRevision_.Revision(),
            identity.value("source_sha256").toString().toStdString()};
        context.hasOpaqueArchiveMembers = project_.archiveSession_ && project_.archiveSession_->HasOpaqueMembers();
        context.includesUnsavedUiRevision = project_.mapEdits_->IsDirty();
        return Result<Mcp::MapDataToolContext>::Success(std::move(context));
    }

    QJsonObject ProjectSearchTool(const QString& name, const QJsonObject& arguments) {
        if (!projectSearchRevision_ || !projectSearchQuery_)
            return Mcp::Failure("PROJECT_SEARCH_HOST_UNAVAILABLE", "没有绑定活动窗口的共用全局搜索查询。");
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        bool sourceChecked = false;
        if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
            // 文件监视只给出“可能变化”；与界面“检查来源”共用同一只读核对，通过即清除提示继续搜索。
            const auto sources = project_.CheckProjectSources();
            if (!sources.Succeeded()) {
                auto status = sources;
                status.diagnostics.insert(status.diagnostics.begin(), Diagnostic{DiagnosticSeverity::Error, "MCP",
                    "PROJECT_SEARCH_SOURCE_CHANGED", "来源核对未通过：地图、工作区或工程文件与打开时不一致；需作者确认或重新打开后再搜索。",
                    std::nullopt});
                return Mcp::Failure(status);
            }
            sourceChecked = true;
        }
        const auto owner = ownerHandle_;
        const auto token = projectSearchToken_;
        const auto revisionCallback = projectSearchRevision_;
        const auto queryCallback = projectSearchQuery_;
        const auto valid = [this, owner, token] { return owner && projectSearchToken_ == token; };
        Mcp::ProjectSearchToolHost host;
        host.identity = [this, valid, revisionCallback] {
            if (!valid()) return QJsonObject{};
            auto identity = State();
            const auto revision = revisionCallback();
            if (!valid()) return QJsonObject{};
            identity.insert("context_revision", QString::fromStdString(revision));
            return identity;
        };
        host.query = [valid, queryCallback](std::string_view revision, std::string_view query,
            const Services::ProjectSearchOptions& options, std::size_t pageLimit,
            const Services::ProjectSearchCursor* cursor) {
            using Page = Result<Services::ProjectSearchPage>;
            if (!valid()) return Page::Failure("QtApp.Live", "PROJECT_SEARCH_HOST_CHANGED", "搜索宿主或接口已变化。");
            auto page = queryCallback(revision, query, options, pageLimit, cursor);
            if (!valid()) return Page::Failure("QtApp.Live", "PROJECT_SEARCH_HOST_CHANGED", "搜索期间宿主或接口已变化。");
            return page;
        };
        auto result = Mcp::InvokeProjectSearchTool(name, arguments, host);
        if (!valid()) return Mcp::Failure("PROJECT_SEARCH_HOST_CHANGED", "搜索期间宿主或接口已变化。");
        result.insert("scope", "gui-live-document"); result.insert("source_checked", sourceChecked);
        return result;
    }

    QJsonObject ImportTool(const QString& name, const QJsonObject& arguments) {
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        SyncIdentity();
        if (!project_.editTrackingReady_ || project_.editTrackingFailed_ ||
            (!project_.archiveSession_ && !project_.directorySnapshot_))
            return Mcp::Failure("LIVE_EDITOR_NOT_READY", "活动地图缺少可信来源快照或编辑修订。");
        Mcp::ImportToolHost host;
        host.identity = [this] { return State(); };
        host.context = [this]() -> Result<std::shared_ptr<const Services::ImportInventorySnapshot>> {
            const auto identity = State();
            const auto revision = identity.value("revision").toString().toStdString();
            Services::ImportInventoryBinding binding{projectIdentity_.toStdString(), generation_, revision,
                identity.value("source_sha256").toString().toStdString(), revision, "gui-current-path-overlay"};
            Services::ImportInventoryOverlay overlay;
            overlay.includesUnsavedChanges = project_.HasPendingAssetEdits();
            for (const auto& edit : project_.assetEdits_)
                overlay.pathEdits.push_back({edit.from.toStdString(), edit.to.toStdString()});
            for (const auto& pending : project_.pendingImports_) {
                std::optional<std::uint64_t> size;
                if (pending.size >= 0) size = static_cast<std::uint64_t>(pending.size);
                overlay.pending.push_back({pending.archivePath.toStdString(), size});
            }
            if (project_.archiveSession_)
                return Services::CaptureImportInventory(*project_.archiveSession_, std::move(binding), std::move(overlay));
            return Services::CaptureImportInventory(*project_.directorySnapshot_, std::move(binding), std::move(overlay));
        };
        return Mcp::InvokeImportTool(name, arguments, host);
    }

    QJsonObject MapDataTool(const QString& name, const QJsonObject& arguments, bool write) {
        const auto invoke = [&] {
            Mcp::MapDataToolHost host;
            host.identity = [this] { return State(); };
            host.context = [this] { return VisibleMapDataContext(); };
            host.apply = [this](const MapData::PreparedMapDataFields& plan,
                const Mcp::MapDataPrepublishValidator& validate) -> Result<MapData::MapDataEditSession::EditReceipt> {
                using Receipt = MapData::MapDataEditSession::EditReceipt;
                if (!project_.mapEdits_ || !project_.mapDataUndoStack_)
                    return Result<Receipt>::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE",
                        "当前地图没有共用地图数据撤销栈。");
                const auto applied = project_.mapEdits_->ApplyPreparedFields(plan,
                    [&](const auto& before, const auto& after, const auto& receipt) {
                        return validate ? validate(before, after, receipt, {}) : Status::Ok();
                    });
                if (!applied.Succeeded()) return {{}, applied.diagnostics};
                const auto receipt = project_.mapEdits_->LastEdit();
                QtProject::PushMapDataEdit(project_, QObject::tr("AI 修改地图字段"), "MCP 修改原生地图字段");
                (void)project_.ObserveMapDataEdit("MCP 修改原生地图字段");
                emit project_.MapDocumentChanged();
                return Result<Receipt>::Success(receipt);
            };
            return Mcp::InvokeMapDataTool(name, arguments, host);
        };
        if (write) return Write(arguments, false, invoke);
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        auto bound = arguments;
        if (!bound.contains("project_identity") && !bound.contains("expected_revision")) {
            const auto identity = State();
            bound.insert("project_identity", identity.value("project_identity"));
            bound.insert("expected_revision", identity.value("revision"));
        }
        const auto checked = CheckContext(bound, false, true);
        if (!checked.value("ok").toBool()) return checked;
        return invoke();
    }

    QJsonObject PlacementTool(const QString& name, const QJsonObject& arguments) {
        const auto invoke = [&] {
            Mcp::PlacementToolHost host;
            host.identity = [this] { return State(); };
            host.context = [this] { return VisibleMapDataContext(); };
            host.objectContext = MakeLiveObjectContextProvider(project_, State());
            host.terrainPoint = [this](const MapData::TerrainStrokePoint& point) -> Result<MapData::TerrainPointSample> {
                if (!project_.mapEdits_) return Result<MapData::TerrainPointSample>::Failure("QtApp.Live",
                    "ENTITY_CREATE_HOST_UNAVAILABLE", "当前地图没有可用的地形会话。");
                return project_.mapEdits_->InspectTerrainPoint(point);
            };
            host.apply = [this](const MapData::PreparedMapDataPlacement& plan,
                const Mcp::PlacementPrepublishValidator& validate) -> Result<MapData::MapDataEditSession::EditReceipt> {
                using Receipt = MapData::MapDataEditSession::EditReceipt;
                if (!project_.mapEdits_ || !project_.mapDataUndoStack_)
                    return Result<Receipt>::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE",
                        "当前地图没有共用地图数据撤销栈。");
                const auto applied = project_.mapEdits_->ApplyPreparedPlacement(plan,
                    [&](const auto& before, const auto& after, const auto& receipt) {
                        return validate ? validate(before, after, receipt, {}) : Status::Ok();
                    });
                if (!applied.Succeeded()) return {{}, applied.diagnostics};
                const auto receipt = project_.mapEdits_->LastEdit();
                QtProject::PushMapDataEdit(project_, QObject::tr("AI 修改预放实体"), "MCP 共用预放实体命令");
                (void)project_.ObserveMapDataEdit("MCP 共用预放实体命令");
                emit project_.MapDocumentChanged();
                return Result<Receipt>::Success(receipt);
            };
            return Mcp::InvokePlacementTool(name, arguments, host);
        };
        if (arguments.contains("dry_run") && !arguments.value("dry_run").isBool())
            return Mcp::Failure("ENTITY_ARGUMENT_TYPE", "dry_run 须为布尔值。");
        if (!arguments.value("dry_run").toBool()) return Write(arguments, false, invoke);
        // 预览只读当前已提交到工程的 UI 状态，不能因提交草稿而改变修订或撤销历史。
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        const auto checked = CheckContext(arguments, false, true);
        if (!checked.value("ok").toBool()) return checked;
        return invoke();
    }

    QJsonObject AuxiliaryTool(const QString& name, const QJsonObject& arguments) {
        if (arguments.contains("dry_run") && !arguments.value("dry_run").isBool())
            return Mcp::Failure("AUX_ARGUMENT_TYPE", "dry_run 须为布尔值。");
        const auto invoke = [&] {
            return InvokeLiveAuxiliaryTool(project_, name, arguments, [this] { return State(); },
                [this] { return VisibleMapDataContext(); });
        };
        if (!arguments.value("dry_run").toBool()) return Write(arguments, false, invoke);
        // 与实际命令使用同一来源检查；预览不提交 UI 草稿或取得写租约。
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        const auto checked = CheckContext(arguments, false, true);
        if (!checked.value("ok").toBool()) return checked;
        return invoke();
    }

    QJsonObject TerrainTool(const QString& name, const QJsonObject& arguments, bool write) {
        const auto context = [this]() -> Result<Mcp::TerrainEditToolContext> {
            if (!project_.mapEdits_ || !project_.mapEdits_->Terrain())
                return Result<Mcp::TerrainEditToolContext>::Failure("QtApp.Live", "LIVE_EDITOR_TERRAIN_UNAVAILABLE",
                    "当前地图没有可读取的原地形会话。");
            const auto identity = State();
            return Result<Mcp::TerrainEditToolContext>::Success({project_.mapEdits_.get(),
                {projectIdentity_.toStdString(), generation_, project_.editRevision_.Revision(),
                    identity.value("source_sha256").toString().toStdString()}, false, project_.mapEdits_->IsDirty()});
        };
        const auto invoke = [&] {
            Mcp::TerrainEditToolHost host;
            host.identity = [this] { return State(); };
            host.context = context;
            host.apply = [this, &context](const Services::PreparedTerrainEdit& plan,
                const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& validate)
                -> Result<MapData::TerrainStrokeResult> {
                if (!project_.mapEdits_ || !project_.mapDataUndoStack_)
                    return Result<MapData::TerrainStrokeResult>::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE",
                        "当前地图没有共用地图数据撤销栈。");
                const auto current = context();
                if (!current.Succeeded()) return {{}, current.diagnostics};
                const auto applied = Services::ApplyTerrainEdit(*project_.mapEdits_, current.value->document, plan, validate);
                if (!applied.Succeeded()) return applied;
                (void)project_.ObserveMapDataEdit("MCP 修改地形");
                // 原世界页监听此撤销栈的 indexChanged，按同一地形快照增量重建；不发整图重新加载信号。
                QtProject::PushMapDataEdit(project_, QObject::tr("AI 地形笔刷"), "MCP 修改地形");
                emit owner_.Changed();
                return applied;
            };
            return Mcp::InvokeTerrainEditTool(name, arguments, host);
        };
        // 预览不触发界面草稿提交；dry_run 无效时也不能先写入再由 façade 拒绝。
        const auto dryRun = arguments.value("dry_run");
        if (write && dryRun.isBool() && !dryRun.toBool()) return Write(arguments, false, invoke);
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        auto bound = arguments;
        if (!bound.contains("project_identity") && !bound.contains("expected_revision")) {
            const auto identity = State();
            bound.insert("project_identity", identity.value("project_identity"));
            bound.insert("expected_revision", identity.value("revision"));
        }
        const auto checked = CheckContext(bound, false, true);
        if (!checked.value("ok").toBool()) return checked;
        return invoke();
    }

    QJsonObject Save(const QJsonObject& arguments) {
        return Write(arguments, false, [&] {
            if (!save_) return Mcp::Failure("LIVE_EDITOR_SAVE_UNAVAILABLE", "宿主尚未接通原地图保存服务。");
            const bool inPlace = arguments.value("in_place").toBool(false);
            const auto pathText = arguments.value("path").toString();
            if (pathText.contains(QChar::Null)) return Mcp::Failure("LIVE_EDITOR_SAVE_PATH", "保存路径不能包含 NUL。");
            auto path = std::filesystem::path(pathText.toStdWString());
            std::error_code error;
            if (inPlace) {
                if (!project_.archiveSession_ || project_.sourceArchivePath_.empty())
                    return Mcp::Failure("LIVE_EDITOR_SAVE_PATH", "当前地图没有可写回的已绑定归档来源。");
                if (!path.empty() && (!std::filesystem::equivalent(path, project_.sourceArchivePath_, error) || error))
                    return Mcp::Failure("LIVE_EDITOR_SAVE_PATH", "写回路径必须是当前打开的地图。");
                path = project_.sourceArchivePath_;
            } else {
                const auto extension = QString::fromStdWString(path.extension().wstring()).toLower();
                if (!path.is_absolute() || (extension != QStringLiteral(".w3x") && extension != QStringLiteral(".w3m")))
                    return Mcp::Failure("LIVE_EDITOR_SAVE_PATH", "另存地图须使用完整的 .w3x/.w3m 路径。");
                if (std::filesystem::exists(path, error) || error)
                    return Mcp::Failure("LIVE_EDITOR_SAVE_PATH", "另存目标已存在或无法确认；请使用新的独立路径。");
            }
            QJsonValue previous(QJsonValue::Null);
            if (inPlace) {
                const auto original = Sha256File(path);
                if (!original.Succeeded()) return Mcp::Failure(original);
                previous = QJsonObject{{"path", QString::fromStdWString(path.wstring())}, {"sha256", QString::fromStdString(*original.value)}};
            }
            const bool succeeded = save_(path, inPlace);
            if (!succeeded) return Mcp::Failure("LIVE_EDITOR_SAVE_REJECTED", "原地图保存服务拒绝写出；查看编辑器诊断。");
            const auto sha = Sha256File(path);
            if (!sha.Succeeded()) return Mcp::Failure("LIVE_EDITOR_SAVE_OUTPUT_MISSING", "宿主报告成功，但无法读取输出身份。");
            SyncIdentity();
            emit owner_.Changed();
            const QJsonObject output{{"path", QString::fromStdWString(path.wstring())}, {"sha256", QString::fromStdString(*sha.value)}};
            const QJsonObject item{{"kind", "file"}, {"object_id", ""}, {"title", QString::fromStdWString(path.filename().wstring())},
                {"target", QString::fromStdWString(path.wstring())}, {"before", previous}, {"after", output}};
            return QJsonObject{{"ok", true}, {"saved", true}, {"in_place", inPlace},
                {"path", QString::fromStdWString(path.wstring())}, {"sha256", QString::fromStdString(*sha.value)},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"save_evidence", "host-common-save-handler"}, {"game_verified", false},
                {"changes", QJsonObject{{"files", 1}, {"items", QJsonArray{item}}}}};
        });
    }

    // 与代码编辑器相同：Lua 检查另做 jass.* 用法与 require 检查；current 不计入模块。
    Services::CodeCheckLuaSources LuaCheckSources(const QString& current) {
        Services::CodeCheckLuaSources sources;
        sources.catalog = project_.LuaRuntimeCatalog();
        const auto modules = QtProject::CaptureLuaModules(project_, current);
        sources.modules.insert(modules->begin(), modules->end());
        return sources;
    }

    // 参数已由 InvokeLuaFileTool 校验；写入已在 Write 门内。
    QJsonObject LuaFile(const QString& name, const QJsonObject& arguments) {
        const auto path = arguments.value("path").toString();
        if (name == "lua_files_list") {
            QJsonArray files;
            for (const auto& entry : QtProject::ListLuaScripts(project_))
                files.append(QJsonObject{{"path", entry.path}, {"state", entry.state}, {"size", entry.size}});
            return QJsonObject{{"ok", true}, {"files", files}, {"revision", QString::number(project_.editRevision_.Revision())}};
        }
        if (name == "lua_file_read") {
            auto read = QtProject::ReadLuaScript(project_, path);
            if (!read.Succeeded()) return Mcp::Failure(read);
            return QJsonObject{{"ok", true}, {"path", path}, {"text", read.value->text},
                {"sha256", QString::fromStdString(read.value->sha256)}, {"revision", QString::number(project_.editRevision_.Revision())}};
        }
        if (name == "lua_file_write") {
            const auto before = QtProject::ReadLuaScript(project_, path);
            const QJsonValue previous = before.Succeeded() ? QJsonValue(QString::fromStdString(before.value->sha256)) : QJsonValue(QJsonValue::Null);
            const auto rejected = QtProject::WriteLuaScript(project_, path, arguments.value("text").toString(),
                arguments.value("expected_sha256").toString().toStdString(), QObject::tr("AI 修改 Lua：%1").arg(path));
            if (!rejected.isEmpty()) return Mcp::Failure("LUA_FILE_WRITE_REJECTED", rejected.toStdString());
            const auto after = QtProject::ReadLuaScript(project_, path);
            if (!after.Succeeded()) return Mcp::Failure(after);
            SyncIdentity();
            emit owner_.Changed();
            const auto sha = QString::fromStdString(after.value->sha256);
            const QJsonObject item{{"kind", "lua"}, {"object_id", ""}, {"title", path}, {"target", path},
                {"before", previous}, {"after", sha}};
            return QJsonObject{{"ok", true}, {"path", path}, {"sha256", sha}, {"created", previous.isNull()}, {"saved", false},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"changes", QJsonObject{{"files", 1}, {"items", QJsonArray{item}}}}};
        }
        if (!mapTest_) return Mcp::Failure("MAP_TEST_UNAVAILABLE", "宿主尚未接通地图测试。");
        const auto rejected = mapTest_();
        if (!rejected.isEmpty()) return Mcp::Failure("MAP_TEST_REJECTED", rejected.toStdString());
        return QJsonObject{{"ok", true}, {"started", true}, {"game_verified", false}};
    }

    QJsonObject Catalog(const QJsonObject& arguments) {
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "当前地图正在写入。");
        const auto check = CheckContext(arguments, true, false);
        if (!check.value("ok").toBool()) return check;
        const auto kindText = arguments.value("kind").toString(QStringLiteral("action"));
        Trigger::UiDefinitionKind kind = Trigger::UiDefinitionKind::Unknown;
        if (kindText == QStringLiteral("event")) kind = Trigger::UiDefinitionKind::Event;
        if (kindText == QStringLiteral("condition")) kind = Trigger::UiDefinitionKind::Condition;
        if (kindText == QStringLiteral("action")) kind = Trigger::UiDefinitionKind::Action;
        if (kindText == QStringLiteral("call")) kind = Trigger::UiDefinitionKind::Call;
        if (kind == Trigger::UiDefinitionKind::Unknown) return Mcp::Failure("LIVE_EDITOR_CATALOG_KIND", "仅支持 event/condition/action/call。");
        const int start = arguments.value("start").toInt(0), limit = arguments.value("limit").toInt(64);
        const QString search = arguments.value("search").toString();
        if (start < 0 || limit < 1 || limit > 128 || search.size() > 256)
            return Mcp::Failure("LIVE_EDITOR_QUERY_BUDGET", "目录分页要求 start>=0、limit=1..128、search<=256字符。");
        QJsonArray rows;
        qint64 matched = 0;
        for (const auto* definition : project_.uiCatalog_->Definitions(kind)) {
            const auto name = QString::fromStdString(definition->name);
            if (!name.contains(search, Qt::CaseInsensitive)) continue;
            const auto index = matched++;
            if (index < start || index >= static_cast<qint64>(start) + limit) continue;
            QJsonArray parameters;
            for (const auto& argument : definition->Active().arguments)
                parameters.append(QJsonObject{{"type", QString::fromStdString(argument.type)}, {"default", QString::fromStdString(argument.defaultValue)}});
            rows.append(QJsonObject{{"name", name}, {"signature", QString::fromStdString(definition->Signature())},
                {"layer", QString::fromStdString(definition->Active().layer)}, {"arguments", parameters}});
        }
        return {{"ok", true}, {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
            {"ui_catalog_sha256", QString::fromStdString(project_.uiCatalog_->Sha256())}, {"rows", rows},
            {"total", QString::number(matched)}, {"eof", static_cast<qint64>(start) + rows.size() >= matched}};
    }

    void AddTool(QString name, QString title, Mcp::ToolPermission permission, QJsonObject schema,
        std::function<QJsonObject(const QJsonObject&)> handler, std::size_t maxRequestIdBytes = 0,
        std::size_t maxResponseBytes = 1024 * 1024, bool pngImageResult = false) {
        registeredNames_.append(name);
        Mcp::ToolDefinition tool;
        tool.name = name; tool.description = title; tool.permission = permission; tool.inputSchema = std::move(schema);
        tool.maxResponseBytes = maxResponseBytes;
        tool.maxRequestIdBytes = maxRequestIdBytes;
        tool.pngImageResult = pngImageResult;
        tool.handler = [this, name = std::move(name), title = std::move(title), handler = std::move(handler)](const QJsonObject& arguments) -> QJsonValue {
            const auto owner = ownerHandle_;
            const auto invoke = handler;
            const auto toolName = name; const auto toolTitle = title;
            if (!owner) return Mcp::Failure("LIVE_EDITOR_HOST_UNAVAILABLE", "活动地图工具宿主已关闭。");
            if (QThread::currentThread() != owner->thread()) return Mcp::Failure("LIVE_EDITOR_THREAD", "活动地图工具必须在编辑器线程执行。");
            const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            emit owner->Activity(QJsonObject{{"type", "tool_start"}, {"id", id}, {"tool_name", toolName}, {"title", toolTitle}});
            if (!owner) return Mcp::Failure("LIVE_EDITOR_HOST_UNAVAILABLE", "活动地图工具宿主已关闭。");
            QElapsedTimer timer; timer.start();
            QJsonObject result;
            try { result = invoke(arguments); }
            catch (const std::exception& failure) { result = Mcp::Failure("LIVE_EDITOR_EXCEPTION", failure.what()); }
            if (!owner) return result;
            // PNG仅交付请求方；活动日志不再次复制Base64正文。
            auto details = result;
            if (details.contains("image")) {
                details.remove("image");
                details.insert("image_returned", true);
            }
            emit owner->Activity(QJsonObject{{"type", "tool_end"}, {"id", id}, {"tool_name", toolName}, {"title", toolTitle},
                {"success", result.value("ok").toBool()}, {"is_error", !result.value("ok").toBool()},
                {"elapsed_ms", timer.elapsed()}, {"details", details}});
            if (owner && result.value("ok").toBool() && result.value("changes").isObject()) {
                auto changes = result.value("changes").toObject();
                changes.insert("type", "changes"); changes.insert("tool_call_id", id);
                emit owner->Activity(changes);
            }
            return result;
        };
        server_.RegisterTool(std::move(tool));
    }

    void RegisterTools() {
        for (auto definition : Mcp::WorldViewportToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) {
                    const auto check = CheckContext(args, false, false);
                    if (!check.value("ok").toBool()) return check;
                    return Mcp::InvokeWorldViewportTool(name, args, {worldViewport_});
                }, definition.maxRequestIdBytes, definition.maxResponseBytes, definition.pngImageResult);
        }
        for (auto definition : Mcp::LuaTestToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) {
                    const auto check = CheckContext(args, true, name == "lua_test_hot_reload");
                    if (!check.value("ok").toBool()) return check;
                    return Mcp::InvokeLuaTestTool(name, args, {luaTest_});
                }, definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::LuaFileToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) -> QJsonObject {
                    const Mcp::LuaFileToolHost host{[this](const QString& tool, const QJsonObject& arguments) { return LuaFile(tool, arguments); }};
                    // 写入走共同写门：先提交界面草稿，再核对工程身份与修订。
                    if (name == "lua_file_write")
                        return Write(args, false, [&] { return Mcp::InvokeLuaFileTool(name, args, host); });
                    if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图保存或另一个写命令正在执行。");
                    const auto check = CheckContext(args, false, false);
                    if (!check.value("ok").toBool()) return check;
                    return Mcp::InvokeLuaFileTool(name, args, host);
                }, definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::CampaignToolDefinitions(server_)) {
            AddTool(definition.name, definition.description, definition.permission, definition.inputSchema,
                [handler = definition.handler](const QJsonObject& arguments) { return handler(arguments).toObject(); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::ProjectSearchToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& arguments) { return ProjectSearchTool(name, arguments); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        const auto productHost = Mcp::DefaultProductToolHost();
        for (auto definition : Mcp::ProductToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [name, productHost](const QJsonObject& arguments) { return Mcp::InvokeProductTool(name, arguments, productHost); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        AddTool("editor_state", "读取当前编辑器地图与修订", Mcp::ToolPermission::Query, Schema({}),
            [this](const QJsonObject&) { return State(); });
        {
            auto definition = Mcp::CodeCheckToolDefinition();
            auto schema = definition.inputSchema;
            auto properties = schema.value("properties").toObject();
            properties.insert("expected_revision", IdentityProperties().value("expected_revision"));
            schema.insert("properties", properties);
            AddTool(definition.name, definition.description, definition.permission, schema,
                [this](const QJsonObject& arguments) {
                    const auto checked = CheckContext(arguments, arguments.value("source_kind").toString() == "block", false);
                    if (!checked.value("ok").toBool()) return checked;
                    Mcp::CodeCheckToolHost host;
                    host.currentBinding = [this] { return project_.CaptureCodeCheckSnapshot()->binding; };
                    host.capture = [this](Mcp::CodeCheckToolInput input) -> Result<Mcp::CodeCheckToolWork> {
                        const auto snapshot = project_.CaptureCodeCheckSnapshot();
                        if (!input.block) {
                            Services::CodeCheckRequest request;
                            request.source = {input.sourceName, std::move(input.text)}; request.language = input.language;
                            request.generation = input.generation; request.context.binding = snapshot->binding;
                            request.context.reason = "独立文本未放入地图作者代码块，仅检查正文语法。";
                            // 地图 Lua 文件用同名路径检查时，以本次正文替代地图里的旧内容。
                            auto luaSources = request.language == Services::CodeLanguage::Lua53
                                ? LuaCheckSources(QString::fromStdString(input.sourceName)) : Services::CodeCheckLuaSources{};
                            return Result<Mcp::CodeCheckToolWork>::Success({snapshot->binding, input.sourceName,
                                [snapshot, request = std::move(request), luaSources = std::move(luaSources)](std::stop_token cancel) {
                                    return request.language == Services::CodeLanguage::Lua53
                                        ? Services::CheckProjectLuaSource(snapshot, request, cancel, luaSources)
                                        : Services::CheckCode(request, cancel);
                                }});
                        }
                        if (!snapshot->triggers || !snapshot->catalog) return Result<Mcp::CodeCheckToolWork>::Failure(
                            "MCP", "CODE_CHECK_BLOCK_UNAVAILABLE", "当前地图没有冻结的作者代码来源。");
                        const Trigger::RawText* text = nullptr;
                        if (input.target.kind == Trigger::CodeBlockKind::GlobalHeader) text = &snapshot->triggers->wct.customCode;
                        else for (const auto& trigger : snapshot->triggers->triggers)
                            if (trigger.id == input.target.triggerId && trigger.customTextFlag != 0) { text = &trigger.customText; break; }
                        if (!text) return Result<Mcp::CodeCheckToolWork>::Failure("MCP", "CODE_CHECK_BLOCK_MISSING", "作者代码块不存在或仍为GUI触发器。");
                        const bool lua = snapshot->map && snapshot->map->Settings() && snapshot->map->Settings()->scriptLanguage == 1;
                        if (lua != (input.language == Services::CodeLanguage::Lua53)) return Result<Mcp::CodeCheckToolWork>::Failure(
                            "MCP", "CODE_CHECK_LANGUAGE_MISMATCH", "代码块语言与当前地图语言不同；未把JASS当Lua或反向检查。");
                        if (!input.hasText) input.text = Trigger::TriggerCodeEditor::NormalizedText(*text);
                        auto cache = codeCheckCache_;
                        const auto key = Services::CodeCheckTargetSourceName(input.target);
                        // 与代码编辑器相同：Lua 代码块另做 jass.* 用法与 require 检查。
                        auto luaSources = lua ? LuaCheckSources({}) : Services::CodeCheckLuaSources{};
                        return Result<Mcp::CodeCheckToolWork>::Success({snapshot->binding, key,
                            [snapshot, cache, input = std::move(input), luaSources = std::move(luaSources)](std::stop_token cancel) {
                                return Services::CheckProjectCode(snapshot, *cache, input.target, input.text,
                                    input.language, input.generation, cancel, luaSources);
                            }});
                    };
                    return codeChecks_.Invoke(arguments, host);
                }, definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::ImportToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& arguments) { return ImportTool(name, arguments); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::ObjectResourceToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& arguments) {
                    if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
                    return objectResources_->Invoke(name, arguments, [this] { return State(); });
                }, definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::VariableToolDefinitions()) {
            const auto name = definition.name;
            const bool write = definition.permission == Mcp::ToolPermission::Edit;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name, write](const QJsonObject& arguments) { return VariableTool(name, arguments, write); },
                definition.maxRequestIdBytes);
        }
        for (auto definition : Mcp::MapDataToolDefinitions()) {
            const auto name = definition.name;
            const bool write = definition.permission == Mcp::ToolPermission::Edit;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name, write](const QJsonObject& arguments) { return MapDataTool(name, arguments, write); },
                definition.maxRequestIdBytes);
        }
        for (auto definition : Mcp::PlacementToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& arguments) { return PlacementTool(name, arguments); },
                definition.maxRequestIdBytes);
        }
        for (auto definition : Mcp::AuxiliaryToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& arguments) { return AuxiliaryTool(name, arguments); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::TerrainEditToolDefinitions()) {
            const auto name = definition.name;
            const bool write = definition.permission == Mcp::ToolPermission::Edit;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name, write](const QJsonObject& arguments) { return TerrainTool(name, arguments, write); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::ObjectToolDefinitions()) {
            const auto name = definition.name;
            const bool write = definition.permission == Mcp::ToolPermission::Edit;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name, write](const QJsonObject& arguments) {
                    const auto invoke = [&] {
                        return InvokeLiveObjectTool(project_, name, arguments, State());
                    };
                    if (write) return Write(arguments, false, invoke);
                    if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "当前地图正在执行写任务。");
                    SyncIdentity();
                    if (!project_.editTrackingReady_ || project_.editTrackingFailed_ ||
                        (!project_.archiveSession_ && !project_.directorySnapshot_))
                        return Mcp::Failure("LIVE_EDITOR_NOT_READY", "活动地图缺少可信来源快照或编辑修订。");
                    return invoke();
                });
        }
        auto query = IdentityProperties();
        query.insert("table", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"categories", "variables", "triggers", "ecas", "arguments", "map_objects"}}});
        query.insert("schema_version", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{1}}});
        query.insert("start", QJsonObject{{"type", "integer"}, {"minimum", 0}});
        query.insert("limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}});
        query.insert("argument_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 16},
            {"items", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}}});
        query.insert("variable_ids", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 16},
            {"uniqueItems", true}, {"items", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}}}});
        query.insert("include_replacement_guard", QJsonObject{{"type", "boolean"}});
        query.insert("functions", Mcp::EcaFunctionFilterSchema());
        query.insert("ui_layer", QJsonObject{{"type", "string"}, {"minLength", 1}});
        AddTool("editor_query_trigger_graph",
            "查询作者当前可见的触发器（T）结构；ecas 表可用 functions（精确函数名）或 ui_layer（函数库层，如 WarVK）筛选", Mcp::ToolPermission::Query,
            Schema(query, {"project_identity", "expected_revision", "table"}), [this](const QJsonObject& arguments) { return Query(arguments); });
        auto edit = Mcp::GraphOperationSchema();
        auto properties = edit.value("properties").toObject();
        const auto identity = IdentityProperties();
        for (auto it = identity.begin(); it != identity.end(); ++it) properties.insert(it.key(), it.value());
        edit.insert("properties", properties);
        edit.insert("required", QJsonArray{"project_identity", "expected_revision", "expected", "operations"});
        AddTool("editor_edit_trigger_graph", "在共用撤销栈中整批编辑触发器（T）", Mcp::ToolPermission::Edit, edit,
            [this](const QJsonObject& arguments) { return Edit(arguments); });
        auto save = IdentityProperties(); save.insert("path", StringSchema()); save.insert("in_place", QJsonObject{{"type", "boolean"}, {"default", false}});
        AddTool("editor_save_map", "通过原编辑器地图保存服务写回或另存", Mcp::ToolPermission::Save,
            Schema(save, {"project_identity", "expected_revision"}), [this](const QJsonObject& arguments) { return Save(arguments); });
        auto catalog = IdentityProperties();
        catalog.insert("kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"event", "condition", "action", "call"}}});
        catalog.insert("search", StringSchema()); catalog.insert("start", QJsonObject{{"type", "integer"}});
        catalog.insert("limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}});
        AddTool("editor_get_ui_catalog", "查询当前地图实际生效的 T 函数签名", Mcp::ToolPermission::Query,
            Schema(catalog, {"project_identity", "expected_revision"}), [this](const QJsonObject& arguments) { return Catalog(arguments); });
        registeredNames_.sort();
    }

    EditorProject& project_;
    LiveEditorTools& owner_;
    QPointer<LiveEditorTools> ownerHandle_;
    Mcp::McpServer server_;
    std::function<bool(const std::filesystem::path&, bool)> save_;
    std::function<bool()> beforeWrite_;
    std::function<QJsonObject(bool)> luaTest_;
    std::function<QString()> mapTest_;
    std::function<QJsonObject(const QString&, const QJsonObject&)> worldViewport_;
    ProjectSearchRevisionQuery projectSearchRevision_;
    ProjectSearchPageQuery projectSearchQuery_;
    std::shared_ptr<const int> projectSearchToken_;
    QStringList registeredNames_;
    bool busy_ = false;
    QString projectIdentity_;
    std::filesystem::path root_, archive_, uiRoot_;
    std::uint64_t generation_ = 0;
    TriggerDocument* document_ = nullptr;
    Trigger::UiCatalog* catalog_ = nullptr;
    std::string configSha_;
    Mcp::CodeCheckToolSession codeChecks_;
    std::unique_ptr<LiveObjectResourceTools> objectResources_;
    std::shared_ptr<Services::CodeCheckProjectCache> codeCheckCache_ = std::make_shared<Services::CodeCheckProjectCache>();
};

LiveEditorTools::LiveEditorTools(QtProject::EditorProject& project, QObject* parent)
    : QObject(parent), impl_(std::make_shared<Impl>(project, *this)) {}
LiveEditorTools::~LiveEditorTools() = default;
Mcp::McpServer& LiveEditorTools::Server() noexcept { return impl_->Server(); }
void LiveEditorTools::SetSaveHandler(std::function<bool(const std::filesystem::path&, bool)> handler) { impl_->SetSaveHandler(std::move(handler)); }
void LiveEditorTools::SetLuaTestHandler(std::function<QJsonObject(bool)> handler) { impl_->SetLuaTestHandler(std::move(handler)); }
void LiveEditorTools::SetMapTestHandler(std::function<QString()> handler) { impl_->SetMapTestHandler(std::move(handler)); }
void LiveEditorTools::SetWorldViewportHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
    impl_->SetWorldViewportHandler(std::move(handler));
}
void LiveEditorTools::SetBeforeWrite(std::function<bool()> handler) { impl_->SetBeforeWrite(std::move(handler)); }
void LiveEditorTools::SetProjectSearchQuery(ProjectSearchRevisionQuery revision, ProjectSearchPageQuery query) {
    impl_->SetProjectSearchQuery(std::move(revision), std::move(query));
}
void LiveEditorTools::SetPolicy(Mcp::ToolPolicy policy) { impl_->SetPolicy(std::move(policy)); }
void LiveEditorTools::SetSkillRoot(const std::filesystem::path& root) { Mcp::RegisterAllResources(impl_->Server(), root); }
QJsonObject LiveEditorTools::Invoke(const QString& name, const QJsonObject& arguments) {
    const auto invocation = impl_; // 仅当前 Invoke 保住原 server/handler，退出后按原 shutdown 语义释放。
    return invocation->Invoke(name, arguments);
}
QJsonArray LiveEditorTools::Tools() { return impl_->Tools(); }
QStringList LiveEditorTools::RegisteredToolNames() const { return impl_->RegisteredToolNames(); }

} // namespace War3Map::QtApp

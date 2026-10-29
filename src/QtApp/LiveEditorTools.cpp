#include "LiveEditorTools.h"
#include "LiveEditorAuxiliaryTools.h"
#include "McpLuaTestTools.h"
#include "McpLuaFileTools.h"
#include "McpGameUiTools.h"
#include "McpWorldViewportTools.h"
#include "McpCampaignTools.h"
#include "LiveObjectTools.h"
#include "EditorProject.h"
#include "TerrainWaterEditPreparation.h"
#include "TerrainResizeEditPreparation.h"
#include "TerrainTextureSlotEditPreparation.h"
#include "ProjectLuaModules.h"
#include "ObjectNames.h"
#include "McpResources.h"
#include "McpToolInternal.h"
#include "McpTriggerGraphJson.h"
#include "McpObjectTools.h"
#include "McpVariableTools.h"
#include "McpMapDataTools.h"
#include "McpMapCustomSettingsTools.h"
#include "ProjectMapCustomSettings.h"
#include "ProjectTriggerCodeIdentity.h"
#include "McpTriggerCodeIdentity.h"
#include "ProjectObjectCatalog.h"
#include "McpMapRuntimeTools.h"
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
#include "War3MapCore/Services/TerrainBoundsEditService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Trigger/TriggerTextDiff.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Workspace.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QStandardItemModel>
#include <QThread>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

namespace War3Map::QtApp {
namespace {
using QtProject::EditorProject;
using Trigger::TriggerDocument;

struct LiveBoundsPreparation {
    std::shared_ptr<const QtProject::TerrainWaterEditRequest> request;
    MapData::TerrainPathingResources resources;
    Services::PreparedTerrainBoundsEdit prepared;
    std::optional<Services::TerrainSessionResources> structureResources;
};

template<class Output, class Work> Output RunLiveBoundsWorker(Work work) {
    auto result = std::make_shared<Output>();
    QEventLoop loop;
    auto thread = std::unique_ptr<QThread>(QThread::create([result, work = std::move(work)] {
        try { *result = work(); }
        catch (const std::exception&) {
            result->diagnostics = Status::Failure("QtApp.Live", "LIVE_BOUNDS_WORKER_FAILED",
                "地图范围后台读取或准备失败，活动地图未被本次请求修改。").diagnostics;
        }
    }));
    QObject::connect(thread.get(), &QThread::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
    thread->start(QThread::LowPriority);
    // 同步工具仍持busy租约；只处理绘制/状态事件，不在等待时接受新GUI手势。
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    thread->wait();
    return std::move(*result);
}

Status VerifyLiveBoundsResources(const QtProject::TerrainWaterEditRequest& request,
    const MapData::TerrainPathingResources* resources = nullptr) {
    // 此处复核原始来源；草稿已有范围修改时，不能按保存输出的After地形验来源。
    const auto& opened = request.archivePath.empty() ? request.projectRoot : request.archivePath;
    std::error_code error;
    if (!std::filesystem::equivalent(opened, request.sourcePath, error) || error)
        return Status::Failure("QtApp.Live", "LIVE_BOUNDS_SOURCE_CHANGED", "当前地图打开路径不再属于范围来源。");
    if (request.archive) {
        const auto checked = request.archive->VerifySourceUnchanged();
        if (!checked.Succeeded()) return checked;
        return resources ? resources->VerifySourceInputs(*request.archive, request.maximumBytes) : Status::Ok();
    }
    if (!request.directory)
        return Status::Failure("QtApp.Live", "LIVE_BOUNDS_SOURCE_REQUIRED", "地图范围缺少实际来源快照。");
    return resources ? resources->VerifySourceInputs(*request.directory, request.maximumBytes) : Status::Ok();
}

Result<LiveBoundsPreparation> PrepareLiveBounds(
    std::shared_ptr<const QtProject::TerrainWaterEditRequest> request,
    std::shared_ptr<const MapData::MapDataEditSession> frozen, const Services::TerrainEditBinding& binding,
    const MapData::TerrainBoundsEditIntent& intent) {
    using Output = Result<LiveBoundsPreparation>;
    auto verified = VerifyLiveBoundsResources(*request);
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    if (frozen->HasTerrainTypedResizeState()) {
        auto view = request->archive
            ? Services::TerrainSessionResources::Capture(request->archive, frozen, request->gameRoot,
                request->maximumBytes, request->structureRetainedBytes)
            : Services::TerrainSessionResources::Capture(*request->directory, frozen, request->gameRoot,
                request->maximumBytes, request->structureRetainedBytes);
        if (!view.Succeeded() || !view.value) return {{}, std::move(view.diagnostics)};
        auto resources = view.value->CaptureNative(*frozen->Terrain(), nullptr, request->maximumBytes);
        if (!resources.Succeeded() || !resources.value) return {{}, std::move(resources.diagnostics)};
        auto prepared = Services::PrepareTerrainBoundsEdit(*frozen, binding.document, binding, *resources.value, intent);
        if (!prepared.Succeeded() || !prepared.value) return {{}, std::move(prepared.diagnostics)};
        verified = view.value->Verify(*resources.value, request->maximumBytes);
        if (!verified.Succeeded()) return {{}, verified.diagnostics};
        return Output::Success({std::move(request), std::move(*resources.value), std::move(*prepared.value), std::move(*view.value)});
    }
    Result<std::vector<std::byte>> bytes;
    if (request->archive) {
        const std::array<std::string, 1> names{"war3map.w3e"};
        ArchiveExactMemberLimits limits;
        limits.memberBytes = limits.totalBytes = request->maximumBytes;
        auto members = request->archive->CaptureExactMembers(names, limits);
        if (!members.Succeeded()) return {{}, std::move(members.diagnostics)};
        if (members.value->size() != 1 || !members.value->front().present)
            return Output::Failure("QtApp.Live", "LIVE_BOUNDS_TERRAIN_MISSING", "范围编辑缺少实际来源地形。");
        bytes = Result<std::vector<std::byte>>::Success(std::move(members.value->front().bytes));
    } else {
        const WorkspaceFile* selected = nullptr;
        for (const auto& file : request->directory->files) {
            const auto path = QString::fromStdWString(file.relativePath.generic_wstring()).toCaseFolded();
            if (path != QStringLiteral("war3map.w3e") && path != QStringLiteral("map/war3map.w3e")) continue;
            if (selected) return Output::Failure("QtApp.Live", "LIVE_BOUNDS_TERRAIN_AMBIGUOUS", "目录地形来源有冲突。");
            selected = &file;
        }
        if (!selected || selected->size > request->maximumBytes)
            return Output::Failure("QtApp.Live", "LIVE_BOUNDS_TERRAIN_BUDGET", "目录地形缺失或超出范围读取预算。");
        bytes = ReadVerifiedFile(request->sourcePath / selected->relativePath, selected->sha256, request->maximumBytes);
        if (bytes.Succeeded() && bytes.value->size() != selected->size)
            return Output::Failure("QtApp.Live", "LIVE_BOUNDS_SOURCE_CHANGED", "来源地形长度已经变化。");
    }
    if (!bytes.Succeeded()) return {{}, std::move(bytes.diagnostics)};
    auto terrain = MapData::ParseMapTerrain(*bytes.value);
    if (!terrain.Succeeded()) return {{}, std::move(terrain.diagnostics)};
    auto doodads = frozen->Snapshot(MapDataKind::Doodads, request->maximumBytes);
    if (!doodads.Succeeded()) return {{}, std::move(doodads.diagnostics)};
    const auto& member = doodads.value->Member(MapDataKind::Doodads);
    const std::vector<std::byte>* currentDoodads = nullptr;
    if (member.state == MapDataState::Ready && member.sources.size() == 1) currentDoodads = &member.sources.front().data;
    else if (member.state != MapDataState::Missing || !member.sources.empty())
        return Output::Failure("QtApp.Live", "LIVE_BOUNDS_DOODADS_UNAVAILABLE", "范围编辑需要完整当前装饰物来源。");
    auto resources = request->archive
        ? Services::TerrainPathingSave::CaptureNative(*request->archive, *terrain.value->Snapshot(), *request->terrain,
            request->gameRoot, request->maximumBytes, currentDoodads)
        : Services::TerrainPathingSave::CaptureNative(*request->directory, *terrain.value->Snapshot(), *request->terrain,
            request->gameRoot, request->maximumBytes, currentDoodads);
    if (!resources.Succeeded()) return {{}, std::move(resources.diagnostics)};
    auto prepared = Services::PrepareTerrainBoundsEdit(*frozen, binding.document, binding, *resources.value, intent);
    if (!prepared.Succeeded()) return {{}, std::move(prepared.diagnostics)};
    verified = VerifyLiveBoundsResources(*request, &*resources.value);
    if (!verified.Succeeded()) return {{}, std::move(verified.diagnostics)};
    return Output::Success({std::move(request), std::move(*resources.value), std::move(*prepared.value)});
}

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

QJsonObject WithBindingToken(QJsonObject schema, QStringList* tokenFields) {
    auto properties = schema.value("properties").toObject();
    if (!properties.contains("project_identity") || !properties.contains("expected_revision")) return schema;
    properties.insert("binding_token", QJsonObject{{"type", "string"}, {"maxLength", 64},
        {"description", "editor_state 等结果里的 binding_token；可代替 project_identity、expected_revision 和 expected"}});
    schema.insert("properties", properties);
    QJsonArray required;
    for (const auto& field : schema.value("required").toArray()) {
        const auto text = field.toString();
        if (text == "project_identity" || text == "expected_revision" || text == "expected") {
            if (tokenFields) tokenFields->append(text);
        } else required.append(field);
    }
    schema.insert("required", required);
    return schema;
}

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
    void SetValidationSaveHandler(std::function<bool(const std::filesystem::path&)> handler) { validationSave_ = std::move(handler); }
    void SetLuaTestHandler(std::function<QJsonObject(bool)> handler) { luaTest_ = std::move(handler); }
    void SetMapTestHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) { mapTest_ = std::move(handler); }
    void SetWorldViewportHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
        worldViewport_ = std::move(handler);
    }
    void SetGameUiHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
        gameUi_ = std::move(handler);
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

    // 当前触发器绑定按（工程身份, 修订）缓存；缓存过期只会让令牌代填旧绑定，图编辑仍会按实际绑定拒绝。
    std::optional<Trigger::TriggerGraphBinding> CurrentBinding() {
        if (!project_.triggerDocument_ || !project_.uiCatalog_) return std::nullopt;
        const auto key = projectIdentity_ + QLatin1Char('|') + QString::number(project_.editRevision_.Revision());
        if (bindingKey_ == key && cachedBinding_) return cachedBinding_;
        const auto references = project_.CaptureTriggerMapObjectReferences();
        const auto* objects = references.Succeeded() && references.value && !(*references.value)->References().empty()
            ? references.value->get() : nullptr;
        const auto binding = Trigger::CaptureTriggerGraphBinding(*project_.triggerDocument_, *project_.uiCatalog_, objects);
        if (!binding.Succeeded() || !binding.value) return std::nullopt;
        bindingKey_ = key; cachedBinding_ = *binding.value;
        return cachedBinding_;
    }
    // 绑定令牌：工程身份、修订和触发器绑定合成的短串，写工具可用它代替三项身份参数。
    QString BindingToken() {
        SyncIdentity();
        const auto binding = CurrentBinding();
        const auto text = (projectIdentity_ + QLatin1Char('|') + QString::number(project_.editRevision_.Revision()) + QLatin1Char('|')
            + (binding ? QString::fromUtf8(QJsonDocument(Binding(*binding)).toJson(QJsonDocument::Compact)) : QString{})).toStdString();
        const auto sha = Sha256(std::as_bytes(std::span(text.data(), text.size())));
        return sha.Succeeded() ? QStringLiteral("bt1-") + QString::fromStdString(sha.value->substr(0, 32)) : QString{};
    }
    QJsonObject ExpandBindingToken(QJsonObject& arguments, bool withBinding, const QStringList& requiredFields) {
        if (!arguments.contains("binding_token")) {
            for (const auto& field : requiredFields) if (!arguments.contains(field))
                return Mcp::Failure("LIVE_EDITOR_IDENTITY_REQUIRED", "缺少 " + field.toStdString() + "；也可以只传 binding_token。");
            return {};
        }
        for (const auto* field : {"project_identity", "expected_revision", "expected"}) if (arguments.contains(field))
            return Mcp::Failure("LIVE_EDITOR_IDENTITY_AMBIGUOUS", std::string("binding_token 不能和 ") + field + " 同时给出。");
        if (!arguments.value("binding_token").isString() || arguments.value("binding_token").toString() != BindingToken())
            return Mcp::Failure("LIVE_EDITOR_REVISION_CONFLICT", "binding_token 已过期：地图已被修改或重新打开；重新读取 editor_state。");
        arguments.remove("binding_token");
        arguments.insert("project_identity", projectIdentity_);
        arguments.insert("expected_revision", QString::number(project_.editRevision_.Revision()));
        if (withBinding) {
            const auto binding = CurrentBinding();
            if (!binding) return Mcp::Failure("LIVE_EDITOR_TRIGGERS_UNAVAILABLE", "当前地图未加载完整的触发器和 UI 定义。");
            arguments.insert("expected", Binding(*binding));
        }
        return {};
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
        Mcp::InsertMapObjectReferenceDiagnostics(result, references.diagnostics);
        return result;
    }

    // preview 非空时只在私有副本上应用并把结果交给调用方，不发布、不进撤销栈。
    Result<Trigger::TriggerGraphEditReceipt> ApplyGraphBatch(const Trigger::TriggerGraphEditBatch& batch,
        const Mcp::VariablePrepublishValidator& prepublish = {}, QJsonObject* changesResult = nullptr,
        TriggerDocument* preview = nullptr) {
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
        if (preview) {
            *preview = std::move(candidate);
            if (changesResult) *changesResult = payload;
            return edited;
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
            const bool dryRun = request.value("dry_run").toBool(false);
            request.remove("dry_run");
            const bool codeIdentity = Mcp::HasCodeIdentityOperation(request);
            if (codeIdentity && !arguments.value("dry_run").isBool()) return Mcp::Failure("TRIGGER_CODE_REQUEST_INVALID", "代码身份编辑必须显式提供dry_run布尔值。");
            Services::TriggerCodeIdentitySummary summary;
            std::optional<Services::PreparedTriggerCodeIdentityEdit> codePrepared;
            Result<Trigger::TriggerGraphEditBatch> decoded;
            if (codeIdentity) {
                auto context = QtProject::CaptureProjectTriggerCodeIdentityContext(project_); if (!context.Succeeded()) return Mcp::Failure(context);
                decoded = Mcp::DecodeCodeIdentityGraphBatch(request, *context.value, dryRun, &summary, &codePrepared);
            } else decoded = Mcp::DecodeGraphBatch(request);
            if (!decoded.Succeeded()) return Mcp::Failure(decoded);
            QJsonObject changes;
            TriggerDocument preview;
            Result<Trigger::TriggerGraphEditReceipt> edited;
            if (codeIdentity && !dryRun) {
                std::vector<Trigger::GraphEditOperation> suffix(decoded.value->operations.begin() + 1, decoded.value->operations.end());
                edited = QtProject::ApplyProjectTriggerCodeIdentity(project_, *codePrepared, QObject::tr("修改自定义触发器身份"), std::move(suffix));
            } else edited = ApplyGraphBatch(*decoded.value, {}, &changes, dryRun ? &preview : nullptr);
            if (!edited.Succeeded()) return Mcp::Failure(edited);
            QJsonObject created;
            for (const auto& [key, ref] : edited.value->created) created.insert(QString::fromStdString(key),
                QJsonObject{{"kind", RecordKind(ref.kind)}, {"id", QString::number(ref.id)}});
            QJsonObject result{{"ok", true}, {"scope", "gui-visible-document"}, {"project_identity", projectIdentity_},
                {"revision", QString::number(project_.editRevision_.Revision())}, {"before", Binding(edited.value->before)},
                {"binding", Binding(edited.value->after)}, {"applied_operations", QString::number(edited.value->appliedOperations)},
                {"created", created}, {"saved", false}, {"runtime_verified", false}};
            if (codeIdentity) result.insert("code_identity", Mcp::CodeIdentitySummaryJson(summary));
            result.insert("dry_run", dryRun);
            result.insert("published", !dryRun && (edited.value->before.currentWtgSha256 != edited.value->after.currentWtgSha256
                || edited.value->before.currentWctSha256 != edited.value->after.currentWctSha256));
            if (!dryRun) {
                // 预览不发布变化，活动面板不能把它当作已写入。
                result.insert("changes", changes);
                return result;
            }
            QJsonArray diffs;
            const auto diffObjects = project_.CaptureTriggerMapObjectReferences();
            const auto* objects = diffObjects.Succeeded() && diffObjects.value ? diffObjects.value->get() : nullptr;
            for (const auto& change : Trigger::DiffTriggerTexts(*project_.triggerDocument_, preview, *project_.uiCatalog_, 256U * 1024U, objects)) {
                QJsonObject row{{"trigger_id", QString::number(change.triggerId)}, {"name", QString::fromStdString(change.name)},
                    {"status", QString::fromStdString(change.status)}, {"diff", QString::fromStdString(change.diff)}};
                if (!change.note.empty()) row.insert("note", QString::fromStdString(change.note));
                diffs.append(row);
            }
            result.insert("dry_run", true); result.insert("published", false);
            result.insert("change_summary", changes); result.insert("trigger_diffs", diffs);
            return result;
        });
    }

    // 触发器整图检查：把（可选的）图编辑批次应用到当前文档的私有副本，不发布、不进撤销栈；
    // 后台按保存链生成整图并检查，报错附紧凑 T 行列。status 可带 wait_ms 在编辑器线程上等待结果。
    QJsonObject CheckTriggers(const QJsonObject& arguments) {
        const auto action = arguments.value("action").toString(QStringLiteral("submit"));
        if (action == QStringLiteral("status") || action == QStringLiteral("cancel")) {
            const auto check = CheckContext(arguments, true, false);
            if (!check.value("ok").toBool()) return check;
            const auto requestId = arguments.value("request_id").toString().toStdString();
            WaitForCheck(requestId, arguments.value("wait_ms").toInt(0));
            return codeChecks_.Report(arguments, project_.CaptureCodeCheckSnapshot()->binding);
        }
        if (action != QStringLiteral("submit")) return Mcp::Failure("TRIGGER_CHECK_ACTION_INVALID", "action 须为 submit、status 或 cancel。");
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图保存或另一个写命令正在执行。");
        const auto check = CheckContext(arguments, true, false);
        if (!check.value("ok").toBool()) return check;
        TriggerDocument candidate(*project_.triggerDocument_);
        const bool withEdits = arguments.contains("operations");
        if (withEdits) {
            QJsonObject request;
            for (const auto* key : {"expected", "operations", "schema_version"})
                if (arguments.contains(key)) request.insert(key, arguments.value(key));
            Result<Trigger::TriggerGraphEditBatch> decoded;
            if (Mcp::HasCodeIdentityOperation(request)) {
                auto context = QtProject::CaptureProjectTriggerCodeIdentityContext(project_); if (!context.Succeeded()) return Mcp::Failure(context);
                decoded = Mcp::DecodeCodeIdentityGraphBatch(request, *context.value, true);
            } else decoded = Mcp::DecodeGraphBatch(request);
            if (!decoded.Succeeded()) return Mcp::Failure(decoded);
            const auto references = project_.CaptureTriggerMapObjectReferences();
            const auto* objects = references.Succeeded() && references.value && !(*references.value)->References().empty()
                ? references.value->get() : nullptr;
            const auto edited = Trigger::ApplyTriggerGraphEditBatch(candidate, *project_.uiCatalog_, *decoded.value, {}, objects);
            if (!edited.Succeeded()) return Mcp::Failure(edited);
        }
        const auto snapshot = project_.CaptureCodeCheckSnapshot();
        if (!snapshot || !snapshot->catalog) return Mcp::Failure("TRIGGER_CHECK_CONTEXT_UNAVAILABLE", "当前地图没有可冻结的检查来源。");
        const auto generation = codeChecks_.ReserveGeneration();
        if (!generation.Succeeded() || !generation.value) return Mcp::Failure(generation);
        auto document = std::make_shared<const TriggerDocument>(std::move(candidate));
        Mcp::CodeCheckToolWork work{snapshot->binding, "T/whole-map",
            [snapshot, cache = project_.SharedCodeCheckCache(), document, id = *generation.value](std::stop_token cancel) {
                return Services::CheckProjectTriggers(snapshot, *cache, document, id, cancel);
            }};
        auto queued = codeChecks_.Enqueue(std::move(work), *generation.value);
        if (!queued.value("ok").toBool()) return queued;
        queued.insert("candidate", withEdits ? "operations-applied-privately" : "current-document");
        queued.insert("published", false);
        const auto requestId = queued.value("request_id").toString();
        const auto wait = arguments.value("wait_ms").toInt(0);
        if (wait <= 0) return queued;
        WaitForCheck(requestId.toStdString(), wait);
        QJsonObject status{{"action", "status"}, {"request_id", requestId}};
        for (const auto* key : {"offset", "limit"}) if (arguments.contains(key)) status.insert(key, arguments.value(key));
        auto report = codeChecks_.Report(status, snapshot->binding);
        report.insert("candidate", queued.value("candidate"));
        report.insert("published", false);
        return report;
    }

    // 在编辑器线程上等待后台检查（最多 60 秒）；只处理非用户输入事件，不能嵌套等待。
    void WaitForCheck(const std::string& requestId, int milliseconds) {
        if (milliseconds <= 0 || waitingForCheck_) return;
        QScopedValueRollback<bool> waiting(waitingForCheck_, true);
        QElapsedTimer timer; timer.start();
        const auto limit = std::min(milliseconds, 60000);
        while (codeChecks_.Pending(requestId) && timer.elapsed() < limit)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
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

    QJsonObject CustomSettingsTool(const QString& name, const QJsonObject& arguments, bool write) {
        const auto invoke = [&] {
            Mcp::MapCustomSettingsToolHost host;
            host.identity = [this] { auto identity = State(); identity.insert("dirty", project_.HasUnsavedMapChanges()); return identity; };
            host.context = [this] { return VisibleMapDataContext(); };
            host.validation = [this] { return QtProject::CaptureProjectMapCustomSettingsValidation(project_); };
            host.apply = [this](const Services::PreparedMapCustomSettingsEdit& plan, const Services::MapCustomSettingsValidation& validation,
                const Mcp::MapDataPrepublishValidator& validate) {
                return QtProject::ApplyProjectMapCustomSettings(project_, plan, validation, QObject::tr("AI 修改地图科技"), "MCP 修改地图科技设置",
                    [&](const auto& before, const auto& after, const auto& receipt) { return validate ? validate(before, after, receipt, {}) : Status::Ok(); });
            };
            return Mcp::InvokeMapCustomSettingsTool(name, arguments, host);
        };
        if (write) return Write(arguments, false, invoke);
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
        const auto checked = CheckContext(arguments, false, true); if (!checked.value("ok").toBool()) return checked;
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
            std::optional<QtProject::TerrainWaterEditCapture> waterCapture;
            std::shared_ptr<const LiveBoundsPreparation> boundsCapture;
            std::shared_ptr<const QtProject::TerrainResizeEditCapture> resizeCapture;
            bool boundsPublishing = false;
            bool resizePublishing = false;
            const QPointer<EditorProject> boundsProject(&project_);
            QScopedValueRollback<bool> boundsLease(busy_, busy_ || name == QStringLiteral("edit_terrain_bounds")
                || name == QStringLiteral("edit_terrain_resize") || name == QStringLiteral("edit_terrain_texture_slot"));
            host.identity = [this] { return State(); };
            host.context = context;
            host.prepareTextureSlot = [this, &waterCapture, boundsProject](const Mcp::TerrainEditToolContext& current,
                const Services::TerrainEditBinding& binding, const Services::TerrainTextureSlotCommand& command)
                -> Result<Services::PreparedTerrainEdit> {
                if (!boundsProject || !ownerHandle_ || project_.mapEdits_.get() != current.session)
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "活动贴图槽会话已变化。");
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded()) return {{}, std::move(request.diagnostics)};
                auto frozen = project_.mapEdits_->FreezeForTerrainTextureSlot(64u * 1024u * 1024u);
                if (!frozen.Succeeded()) return {{}, std::move(frozen.diagnostics)};
                const auto owned = std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value));
                auto captured = RunLiveBoundsWorker<Result<QtProject::TerrainTextureSlotEditCapture>>([owned,
                    frozen = std::move(*frozen.value), binding, command] {
                    return QtProject::PrepareTerrainTextureSlotEditCapture(owned, frozen, binding, command);
                });
                if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
                if (!boundsProject || !ownerHandle_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *owned, true))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "贴图槽后台准备期间工程或会话已变化。");
                waterCapture = std::move(captured.value->resources);
                return Result<Services::PreparedTerrainEdit>::Success(std::move(captured.value->prepared));
            };
            host.prepareWater = [this, &waterCapture](const Mcp::TerrainEditToolContext& current, const auto& binding,
                const auto& options, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, options.enforceWaterLimits);
                if (!request.Succeeded() || !request.value) return {{}, request.diagnostics};
                auto loaded = QtProject::LoadTerrainWaterEditResources(
                    std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value)));
                if (!loaded.Succeeded() || !loaded.value) return {{}, loaded.diagnostics};
                waterCapture = std::move(*loaded.value);
                if (!QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request, options.enforceWaterLimits))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "水刷准备期间来源或会话已经变化。");
                return Services::PrepareTerrainWaterEdit(*current.session, current.document, binding,
                    waterCapture->resources, options, points, follow);
            };
            host.verifyWater = [this, &waterCapture] {
                if (!waterCapture || !QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request,
                    waterCapture->request->enforceWaterLimits))
                    return Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "水刷实际来源或会话已变化。");
                return QtProject::VerifyTerrainWaterEditResources(*waterCapture);
            };
            host.prepareCliff = [this, &waterCapture](const Mcp::TerrainEditToolContext& current, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded() || !request.value) return {{}, request.diagnostics};
                auto loaded = QtProject::LoadTerrainWaterEditResources(
                    std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value)));
                if (!loaded.Succeeded() || !loaded.value) return {{}, loaded.diagnostics};
                waterCapture = std::move(*loaded.value);
                if (!QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request, true))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "悬崖准备期间来源或会话已经变化。");
                return Services::PrepareTerrainCliffEdit(*current.session, current.document, binding,
                    waterCapture->resources, brush, points, follow);
            };
            host.prepareCornerRamp = [this, &waterCapture](const Mcp::TerrainEditToolContext& current, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded() || !request.value) return {{}, request.diagnostics};
                auto loaded = QtProject::LoadTerrainWaterEditResources(
                    std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value)));
                if (!loaded.Succeeded() || !loaded.value) return {{}, loaded.diagnostics};
                waterCapture = std::move(*loaded.value);
                if (!QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request, true))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "角坡准备期间来源或会话已经变化。");
                return Services::PrepareTerrainCornerRampEdit(*current.session, current.document, binding,
                    waterCapture->resources, brush, points, follow);
            };
            host.prepareStraightRamp = [this, &waterCapture](const Mcp::TerrainEditToolContext& current, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow,
                MapData::TerrainStraightRampAlgorithm algorithm) -> Result<Services::PreparedTerrainEdit> {
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded() || !request.value) return {{}, request.diagnostics};
                auto loaded = QtProject::LoadTerrainWaterEditResources(
                    std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value)));
                if (!loaded.Succeeded() || !loaded.value) return {{}, loaded.diagnostics};
                waterCapture = std::move(*loaded.value);
                if (!QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request, true))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "直坡准备期间来源或会话已经变化。");
                return Services::PrepareTerrainStraightRampEdit(*current.session, current.document, binding,
                    waterCapture->resources, brush, points, follow, {}, algorithm);
            };
            host.prepareBlock = [this, &waterCapture](const Mcp::TerrainEditToolContext& current, const auto& binding,
                const MapData::TerrainBlockPasteIntent& intent, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded() || !request.value) return {{}, request.diagnostics};
                auto loaded = QtProject::LoadTerrainWaterEditResources(
                    std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value)));
                if (!loaded.Succeeded() || !loaded.value) return {{}, loaded.diagnostics};
                waterCapture = std::move(*loaded.value);
                if (!QtProject::TerrainWaterEditRequestCurrent(project_, *waterCapture->request, true))
                    return Result<Services::PreparedTerrainEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "地形块准备期间来源或会话已经变化。");
                return Services::PrepareTerrainBlockPasteEdit(*current.session, current.document, binding,
                    waterCapture->resources, intent, follow);
            };
            host.prepareResize = [this, &resizeCapture, boundsProject](const Mcp::TerrainEditToolContext& current,
                const Services::TerrainEditBinding& binding, const MapData::TerrainResizeIntent& intent)
                -> Result<Services::PreparedTerrainResizeEdit> {
                if (!boundsProject || !ownerHandle_ || project_.mapEdits_.get() != current.session)
                    return Result<Services::PreparedTerrainResizeEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "活动尺寸会话已经变化。");
                const MapData::TerrainResizeLimits limits;
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true, 64u * 1024u * 1024u, false);
                if (!request.Succeeded()) return {{}, std::move(request.diagnostics)};
                auto frozen = project_.mapEdits_->FreezeForSave(limits.maximumCandidateBytes);
                if (!frozen.Succeeded()) return {{}, std::move(frozen.diagnostics)};
                const auto owned = std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value));
                auto captured = RunLiveBoundsWorker<Result<QtProject::TerrainResizeEditCapture>>([owned,
                    frozen = std::move(*frozen.value), binding, intent, limits] {
                    return QtProject::PrepareTerrainResizeEditCapture(owned, frozen, binding.document, intent, {}, limits);
                });
                if (!captured.Succeeded() || !captured.value) return {{}, std::move(captured.diagnostics)};
                if (!boundsProject || !ownerHandle_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *owned, true))
                    return Result<Services::PreparedTerrainResizeEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "尺寸后台准备期间来源或会话已经变化。");
                resizeCapture = std::make_shared<const QtProject::TerrainResizeEditCapture>(std::move(*captured.value));
                if (!resizeCapture->prepared)
                    return Result<Services::PreparedTerrainResizeEdit>::Failure("QtApp.Live", "TERRAIN_RESIZE_CAPTURE_INVALID", "尺寸候选未完整准备。");
                return Result<Services::PreparedTerrainResizeEdit>::Success(*resizeCapture->prepared);
            };
            host.verifyResize = [this, &resizeCapture, &resizePublishing, boundsProject] {
                const auto capture = resizeCapture;
                const auto current = [&] { return boundsProject && ownerHandle_ && capture && !project_.savingMap_
                    && QtProject::TerrainWaterEditRequestCurrent(project_, *capture->request, true); };
                if (!current()) return Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "尺寸实际来源或会话已经变化。");
                if (resizePublishing) return Status::Ok();
                const auto checked = RunLiveBoundsWorker<Status>([capture] { return QtProject::VerifyTerrainResizeEditCapture(*capture); });
                if (!checked.Succeeded()) return checked;
                return current() ? Status::Ok() : Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "尺寸来源复核期间活动会话已经变化。");
            };
            host.applyResize = [this, &context, &host, &resizeCapture, &resizePublishing, boundsProject](const Services::PreparedTerrainResizeEdit& plan,
                const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& validate) -> Status {
                if (!boundsProject || !ownerHandle_ || !project_.mapEdits_ || !project_.mapDataUndoStack_ || !resizeCapture)
                    return Status::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE", "尺寸会话或共用撤销栈不可用。");
                const auto verified = host.verifyResize(); if (!verified.Succeeded()) return verified;
                const auto current = context(); if (!current.Succeeded()) return {current.diagnostics};
                QScopedValueRollback<bool> publishing(resizePublishing, true);
                const auto before = project_.mapEdits_->Revision();
                std::unique_ptr<QtProject::MapDataEditCommand> undo;
                const auto applied = Services::ApplyTerrainResizeEdit(*project_.mapEdits_, current.value->document, plan,
                    [&](const auto& receipt) {
                        if (validate) { const auto accepted = validate(receipt); if (!accepted.Succeeded()) return accepted; }
                        if (!boundsProject || !ownerHandle_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *resizeCapture->request, true))
                            return Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "尺寸发布前活动会话已经变化。");
                        try { undo = std::make_unique<QtProject::MapDataEditCommand>(project_, receipt,
                            QObject::tr("AI 地图尺寸"), "MCP 调整地图尺寸"); }
                        catch (const std::exception&) { return Status::Failure("QtApp.Live", "LIVE_RESIZE_UNDO_FAILED", "无法准备共用撤销记录，尺寸未修改。"); }
                        return Status::Ok();
                    });
                if (!applied.Succeeded() || project_.mapEdits_->Revision() == before) return applied;
                (void)project_.ObserveMapDataEdit("MCP 调整地图尺寸"); project_.mapDataUndoStack_->push(undo.release());
                if (ownerHandle_) emit owner_.Changed(); return applied;
            };
            host.prepareBounds = [this, &boundsCapture, boundsProject](const Mcp::TerrainEditToolContext& current,
                const Services::TerrainEditBinding& binding, const MapData::TerrainBoundsEditIntent& intent)
                -> Result<Services::PreparedTerrainBoundsEdit> {
                if (!boundsProject || !ownerHandle_ || project_.mapEdits_.get() != current.session)
                    return Result<Services::PreparedTerrainBoundsEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "活动范围会话已经变化。");
                auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true);
                if (!request.Succeeded()) return {{}, std::move(request.diagnostics)};
                auto frozen = request.value->structureSnapshot
                    ? Result<std::shared_ptr<const MapData::MapDataEditSession>>::Success(request.value->structureSnapshot)
                    : project_.mapEdits_->FreezeForSave(512u * 1024u * 1024u);
                if (!frozen.Succeeded()) return {{}, std::move(frozen.diagnostics)};
                const auto owned = std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*request.value));
                auto loaded = RunLiveBoundsWorker<Result<LiveBoundsPreparation>>([owned,
                    frozen = std::move(*frozen.value), binding, intent] { return PrepareLiveBounds(owned, frozen, binding, intent); });
                if (!loaded.Succeeded()) return {{}, std::move(loaded.diagnostics)};
                if (!boundsProject || !ownerHandle_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *owned, true))
                    return Result<Services::PreparedTerrainBoundsEdit>::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "范围后台准备期间来源或会话已经变化。");
                boundsCapture = std::make_shared<const LiveBoundsPreparation>(std::move(*loaded.value));
                return Result<Services::PreparedTerrainBoundsEdit>::Success(boundsCapture->prepared);
            };
            host.verifyBounds = [this, &boundsCapture, &boundsPublishing, boundsProject] {
                const auto capture = boundsCapture;
                const auto current = [&] {
                    return boundsProject && ownerHandle_ && capture && !project_.savingMap_
                        && QtProject::TerrainWaterEditRequestCurrent(project_, *capture->request, true);
                };
                if (!current()) return Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "范围实际来源或会话已经变化。");
                // 最后一次实际回读已在进入Core Apply前完成；命令栈内只同步CAS，
                // 不能让事件循环替换正在提交的unique_ptr会话。
                if (boundsPublishing) return Status::Ok();
                const auto checked = RunLiveBoundsWorker<Status>([capture] {
                    if (capture->structureResources) {
                        const auto path = VerifyLiveBoundsResources(*capture->request);
                        return path.Succeeded() ? capture->structureResources->Verify(capture->resources,
                            capture->request->maximumBytes) : path;
                    }
                    return VerifyLiveBoundsResources(*capture->request, &capture->resources);
                });
                if (!checked.Succeeded()) return checked;
                return current() ? Status::Ok() : Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "范围来源复核期间会话已经变化。");
            };
            host.applyBounds = [this, &context, &host, &boundsCapture, &boundsPublishing, boundsProject](const Services::PreparedTerrainBoundsEdit& plan,
                const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& validate) -> Status {
                if (!boundsProject || !ownerHandle_ || !project_.mapEdits_ || !project_.mapDataUndoStack_ || !boundsCapture)
                    return Status::Failure("QtApp.Live", "LIVE_EDITOR_UNDO_UNAVAILABLE", "范围会话或共用地图撤销栈不可用。");
                const auto verified = host.verifyBounds();
                if (!verified.Succeeded()) return verified;
                const auto current = context();
                if (!current.Succeeded()) return {current.diagnostics};
                QScopedValueRollback<bool> publishing(boundsPublishing, true);
                const auto before = project_.mapEdits_->Revision();
                std::unique_ptr<QtProject::MapDataEditCommand> undo;
                const auto applied = Services::ApplyTerrainBoundsEdit(*project_.mapEdits_, current.value->document, plan,
                    [&](const auto& receipt) {
                        if (validate) {
                            const auto accepted = validate(receipt);
                            if (!accepted.Succeeded()) return accepted;
                        }
                        if (!boundsProject || !ownerHandle_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *boundsCapture->request, true))
                            return Status::Failure("QtApp.Live", "TERRAIN_EDIT_STALE", "范围发布前活动会话已经变化。");
                        try { undo = std::make_unique<QtProject::MapDataEditCommand>(project_, receipt,
                            QObject::tr("AI 地图可玩范围"), "MCP 修改地图可玩范围"); }
                        catch (const std::exception&) { return Status::Failure("QtApp.Live", "LIVE_BOUNDS_UNDO_FAILED", "无法准备共用撤销记录，范围未修改。"); }
                        return Status::Ok();
                    });
                if (!applied.Succeeded() || project_.mapEdits_->Revision() == before) return applied;
                (void)project_.ObserveMapDataEdit("MCP 修改地图可玩范围");
                project_.mapDataUndoStack_->push(undo.release());
                if (ownerHandle_) emit owner_.Changed();
                return applied;
            };
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
            const auto purpose = arguments.value("runtime_purpose").toString("normal");
            if (purpose != "normal" && purpose != "internal_validation") return Mcp::Failure("RUNTIME_SAVE_PURPOSE_INVALID", "活动保存只支持normal或internal_validation。");
            const bool validation = purpose == "internal_validation";
            if (validation ? !validationSave_ : !save_) return Mcp::Failure("LIVE_EDITOR_SAVE_UNAVAILABLE", "宿主尚未接通相应用途的原地图保存服务。");
            const bool inPlace = arguments.value("in_place").toBool(false);
            if (validation && inPlace) return Mcp::Failure("RUNTIME_INTERNAL_COPY_REQUIRED", "内部验收只允许独立新副本，不能写回地图。");
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
            const int firstDiagnostic = project_.diagnosticsModel_ ? project_.diagnosticsModel_->rowCount() : 0;
            const bool succeeded = validation ? validationSave_(path) : save_(path, inPlace);
            if (!succeeded) {
                // 保存链写进诊断表的错误一并带回，Agent 不用看界面也知道原因。
                Status rejected = Status::Failure("MCP", "LIVE_EDITOR_SAVE_REJECTED", "原地图保存服务拒绝写出。");
                const auto* model = project_.diagnosticsModel_;
                for (int row = firstDiagnostic; model && row < model->rowCount() && rejected.diagnostics.size() <= 16; ++row) {
                    if (model->index(row, 0).data().toString() != QStringLiteral("错误")) continue;
                    rejected.diagnostics.push_back({DiagnosticSeverity::Error, model->index(row, 1).data().toString().toStdString(),
                        model->index(row, 2).data().toString().toStdString(), model->index(row, 3).data().toString().left(4096).toStdString(), std::nullopt});
                }
                return Mcp::Failure(rejected);
            }
            const auto sha = Sha256File(path);
            if (!sha.Succeeded()) return Mcp::Failure("LIVE_EDITOR_SAVE_OUTPUT_MISSING", "宿主报告成功，但无法读取输出身份。");
            SyncIdentity();
            emit owner_.Changed();
            const QJsonObject output{{"path", QString::fromStdWString(path.wstring())}, {"sha256", QString::fromStdString(*sha.value)}};
            const QJsonObject item{{"kind", "file"}, {"object_id", ""}, {"title", QString::fromStdWString(path.filename().wstring())},
                {"target", QString::fromStdWString(path.wstring())}, {"before", previous}, {"after", output}};
            return QJsonObject{{"ok", true}, {"saved", true}, {"in_place", inPlace},
                {"runtime_purpose", purpose}, {"project_baseline_changed", !validation},
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
            const auto rows = QtProject::ListLuaScripts(project_);
            if (rows.size() > 2147483647u)
                return Mcp::Failure("LUA_FILES_CATALOG_BUDGET", "Lua目录条目超出分页索引容量，未返回不完整前缀。");
            const int total = static_cast<int>(rows.size());
            const int offset = arguments.contains("offset") ? arguments.value("offset").toInt() : 0;
            const int limit = arguments.contains("limit") ? arguments.value("limit").toInt() : 64;
            if (offset > total)
                return Mcp::Failure("LUA_FILES_OFFSET_RANGE", "offset 超出当前Lua目录条目总数，请从0重新列出。");
            const int count = std::min(limit, total - offset);
            QJsonArray files;
            for (int i = offset; i < offset + count; ++i) {
                const auto& entry = rows[static_cast<std::size_t>(i)];
                files.append(QJsonObject{{"path", entry.path}, {"state", entry.state}, {"size", entry.size}});
            }
            const bool complete = offset + count == total;
            return QJsonObject{{"ok", true}, {"files", files},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"offset", offset}, {"limit", limit}, {"total", total},
                {"next_offset", complete ? QJsonValue(QJsonValue::Null) : QJsonValue(offset + count)},
                {"complete", complete}, {"scope", "captured-assets-and-overlay"}, {"source_checked_in_this_call", false}};
        }
        if (name == "lua_file_read") {
            auto read = QtProject::ReadLuaScript(project_, path);
            if (!read.Succeeded()) return Mcp::Failure(read);
            return QJsonObject{{"ok", true}, {"path", path}, {"text", read.value->text},
                {"sha256", QString::fromStdString(read.value->sha256)}, {"revision", QString::number(project_.editRevision_.Revision())}};
        }
        if (name == "lua_file_write") {
            const auto entries = project_.AssetEntries();
            const auto key = QtProject::EditorProject::AssetKey(path);
            const auto existing = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return QtProject::EditorProject::AssetKey(entry.path) == key;
            });
            if (existing != entries.end() && existing->ambiguous)
                return Mcp::Failure("LUA_FILE_WRITE_REJECTED", "该 Lua 路径有多份来源，未写入。");
            const auto before = QtProject::ReadLuaScript(project_, path);
            if (existing != entries.end()) {
                if (!before.Succeeded()) return Mcp::Failure(before);
                if (!arguments.contains("expected_sha256"))
                    return Mcp::Failure("LUA_FILE_WRITE_REJECTED", "替换已有 Lua 文件必须给出读取时的 sha256。");
            } else {
                if (!project_.archiveSession_)
                    return Mcp::Failure("LUA_FILE_WRITE_REJECTED", "当前来源不能证明 Lua 路径不存在，未按新文件写入。");
                const std::array<std::string, 1> names{path.toStdString()};
                const auto exact = project_.archiveSession_->CaptureExactMembers(names);
                if (!exact.Succeeded()) return Mcp::Failure(exact);
                if (exact.value->size() != 1 || exact.value->front().present)
                    return Mcp::Failure("LUA_FILE_WRITE_REJECTED", "归档中的 Lua 路径不能证明不存在，未按新文件写入。");
            }
            const QJsonValue previous = existing != entries.end()
                ? QJsonValue(QString::fromStdString(before.value->sha256)) : QJsonValue(QJsonValue::Null);
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
            return QJsonObject{{"ok", true}, {"path", path}, {"sha256", sha}, {"created", existing == entries.end()}, {"saved", false},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"changes", QJsonObject{{"files", 1}, {"items", QJsonArray{item}}}}};
        }
        if (name == "imports_add") {
            // 调用时读入内存：之后磁盘文件再变也不影响本次导入和撤销。
            constexpr qint64 kMaxFileBytes = 64LL * 1024 * 1024, kMaxTotalBytes = 256LL * 1024 * 1024;
            std::vector<QtProject::EditorProject::ImportPayload> payloads;
            qint64 total = 0;
            for (const auto& item : arguments.value("files").toArray()) {
                const auto file = item.toObject();
                const auto disk = file.value("disk_path").toString();
                QFile input(disk);
                if (!QFileInfo(disk).isFile() || !input.open(QIODevice::ReadOnly))
                    return Mcp::Failure("IMPORTS_ADD_FILE_UNREADABLE", "无法读取本机文件：" + disk.toStdString());
                if (input.size() > kMaxFileBytes || (total += input.size()) > kMaxTotalBytes)
                    return Mcp::Failure("IMPORTS_ADD_FILE_TOO_LARGE", "单个文件不能超过 64 MiB，一次合计不超过 256 MiB。");
                const auto data = input.readAll();
                if (data.size() != input.size())
                    return Mcp::Failure("IMPORTS_ADD_FILE_UNREADABLE", "读取本机文件不完整：" + disk.toStdString());
                auto bytes = std::make_shared<std::vector<std::byte>>(data.size());
                std::memcpy(bytes->data(), data.constData(), static_cast<std::size_t>(data.size()));
                payloads.push_back({file.value("archive_path").toString(), std::move(bytes)});
            }
            const auto rejected = project_.AddImportPayloads(payloads, arguments.value("replace_existing").toBool(false),
                QObject::tr("AI 导入 %1 个文件").arg(payloads.size()));
            if (!rejected.isEmpty()) return Mcp::Failure("IMPORTS_ADD_REJECTED", rejected.toStdString());
            SyncIdentity();
            emit owner_.Changed();
            QJsonArray added, items;
            for (const auto& payload : payloads) {
                const auto sha = Sha256(*payload.bytes);
                const auto shaText = sha.Succeeded() && sha.value ? QString::fromStdString(*sha.value) : QString();
                added.append(QJsonObject{{"archive_path", payload.archivePath},
                    {"size", static_cast<qint64>(payload.bytes->size())}, {"sha256", shaText}, {"replaced", payload.replaced}});
                items.append(QJsonObject{{"kind", "import"}, {"object_id", ""}, {"title", payload.archivePath},
                    {"target", payload.archivePath}, {"before", QJsonValue::Null}, {"after", shaText}});
            }
            return QJsonObject{{"ok", true}, {"added", added}, {"saved", false},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"changes", QJsonObject{{"files", added.size()}, {"items", items}}}};
        }
        if (name == "imports_remove") {
            // 与资源页的删除同一路径：已有成员记为删除改动，待导入文件直接移出队列；整批一个撤销步骤。
            const auto entries = project_.AssetEntries();
            QStringList paths;
            QSet<QString> requested;
            QJsonArray removed, items;
            for (const auto& item : arguments.value("archive_paths").toArray()) {
                const auto path = item.toString();
                if (const auto problem = QtProject::EditorProject::CheckAssetPath(path); !problem.isEmpty())
                    return Mcp::Failure("IMPORTS_REMOVE_REJECTED", (path + QStringLiteral("：") + problem).toStdString());
                const auto key = QtProject::EditorProject::AssetKey(path);
                if (requested.contains(key)) return Mcp::Failure("IMPORTS_REMOVE_REJECTED", "同一路径出现多次：" + path.toStdString());
                requested.insert(key);
                const auto entry = std::find_if(entries.begin(), entries.end(),
                    [&](const QtProject::AssetEntry& value) { return QtProject::EditorProject::AssetKey(value.path) == key; });
                if (entry == entries.end()) return Mcp::Failure("IMPORTS_REMOVE_REJECTED", "找不到资源：" + path.toStdString());
                paths << entry->path;
                removed.append(QJsonObject{{"archive_path", entry->path}, {"pending", entry->Pending()},
                    {"in_source_map", !entry->originalPath.isEmpty()}});
                items.append(QJsonObject{{"kind", "import"}, {"object_id", ""}, {"title", entry->path},
                    {"target", entry->path}, {"before", entry->path}, {"after", QJsonValue::Null}});
            }
            // 与独立 MCP 的 archive_members_remove 同一查找规则，只是按当前（含未保存修改）内容查。
            const auto scan = project_.FindAssetMentions(paths);
            QString found;
            int listed = 0;
            for (int index = 0; index < paths.size(); ++index) {
                QJsonArray mentions;
                for (const auto& mention : scan.mentions[static_cast<std::size_t>(index)]) {
                    const auto member = QString::fromStdString(mention.member), kind = QString::fromStdString(mention.kind);
                    mentions.append(QJsonObject{{"member", member}, {"kind", kind}, {"count", static_cast<qint64>(mention.count)}});
                    if (listed++ < 16) found += QStringLiteral("\n  %1 ← %2（%3 ×%4）").arg(paths[index], member, kind).arg(mention.count);
                }
                auto row = removed[index].toObject();
                row.insert("mentioned_in", mentions);
                removed[index] = row;
            }
            if (listed != 0 && !arguments.value("allow_referenced").toBool(false))
                return Mcp::Failure("IMPORTS_REMOVE_REFERENCED", QStringLiteral("要删除的文件路径仍出现在 %1 处，确认要删时加 "
                    "allow_referenced=true：%2").arg(listed).arg(found).toStdString());
            QJsonArray unread;
            for (const auto& source : scan.unread) unread.append(source);
            const auto rejected = project_.RecordAssetEdit(QObject::tr("AI 删除 %1 个导入文件").arg(paths.size()),
                [&](QtProject::AssetState& state) -> QString {
                    for (const auto& path : paths)
                        if (const auto problem = project_.RemoveAsset(state, path); !problem.isEmpty()) return problem;
                    return {};
                });
            if (!rejected.isEmpty()) return Mcp::Failure("IMPORTS_REMOVE_REJECTED", rejected.toStdString());
            SyncIdentity();
            emit owner_.Changed();
            return QJsonObject{{"ok", true}, {"removed", removed}, {"saved", false}, {"references_checked", false},
                {"unread_sources", unread},
                {"project_identity", projectIdentity_}, {"revision", QString::number(project_.editRevision_.Revision())},
                {"changes", QJsonObject{{"files", removed.size()}, {"items", items}}}};
        }
        if (!mapTest_) return Mcp::Failure("MAP_TEST_UNAVAILABLE", "宿主尚未接通地图测试。");
        auto result = mapTest_(name, arguments);
        if (result.value("ok").toBool() || result.contains("diagnostics")) return result;
        // 宿主只给 code/message 时转成统一的失败格式，保留其余字段（如已执行步骤的输出）。
        auto failure = Mcp::Failure(result.value("code").toString(QStringLiteral("MAP_TEST_REJECTED")).toStdString(),
            result.value("message").toString().toStdString());
        for (auto it = result.begin(); it != result.end(); ++it) if (!failure.contains(it.key())) failure.insert(it.key(), it.value());
        return failure;
    }

    QJsonObject Catalog(const QJsonObject& arguments) {
        if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "当前地图正在写入。");
        const auto check = CheckContext(arguments, true, false);
        if (!check.value("ok").toBool()) return check;
        // 不给 kind 时按事件、条件、动作、函数依次搜索全部种类；签名前缀标明种类。
        std::vector<Trigger::UiDefinitionKind> kinds;
        const auto kindText = arguments.value("kind").toString();
        if (kindText.isEmpty()) kinds = {Trigger::UiDefinitionKind::Event, Trigger::UiDefinitionKind::Condition,
            Trigger::UiDefinitionKind::Action, Trigger::UiDefinitionKind::Call};
        else if (kindText == QStringLiteral("event")) kinds = {Trigger::UiDefinitionKind::Event};
        else if (kindText == QStringLiteral("condition")) kinds = {Trigger::UiDefinitionKind::Condition};
        else if (kindText == QStringLiteral("action")) kinds = {Trigger::UiDefinitionKind::Action};
        else if (kindText == QStringLiteral("call")) kinds = {Trigger::UiDefinitionKind::Call};
        else return Mcp::Failure("LIVE_EDITOR_CATALOG_KIND", "仅支持 event/condition/action/call。");
        const int start = arguments.value("start").toInt(0), limit = arguments.value("limit").toInt(64);
        const QString search = arguments.value("search").toString();
        if (start < 0 || limit < 1 || limit > 128 || search.size() > 256)
            return Mcp::Failure("LIVE_EDITOR_QUERY_BUDGET", "目录分页要求 start>=0、limit=1..128、search<=256字符。");
        QJsonArray rows;
        qint64 matched = 0;
        for (const auto kind : kinds) for (const auto* definition : project_.uiCatalog_->Definitions(kind)) {
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
        // 带身份的工具都可改用 binding_token：schema 里这几个字段不再必填，缺失时由下面的入口检查。
        QStringList tokenFields;
        tool.inputSchema = WithBindingToken(std::move(tool.inputSchema), &tokenFields);
        const auto properties = tool.inputSchema.value("properties").toObject();
        const bool tokenable = properties.contains("binding_token");
        const bool withBinding = properties.contains("expected");
        tool.handler = [this, name = std::move(name), title = std::move(title), handler = std::move(handler),
                           tokenable, withBinding, tokenFields](const QJsonObject& arguments) -> QJsonValue {
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
            try {
                auto expanded = arguments;
                const auto rejected = tokenable ? ExpandBindingToken(expanded, withBinding, tokenFields) : QJsonObject{};
                result = rejected.isEmpty() ? invoke(expanded) : rejected;
                if (owner && result.value("ok").toBool() && result.contains("project_identity")) {
                    const auto token = BindingToken();
                    if (!token.isEmpty()) result.insert("binding_token", token);
                }
            }
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

    QJsonObject RuntimeTool(const QString& name, const QJsonObject& arguments) {
        const bool write = name == QStringLiteral("map_runtime_set");
        return Mcp::InvokeMapRuntimeTool(name, arguments, {[&](const QString&, const QJsonObject&) {
            const auto query = [&]() -> QJsonObject {
                const auto state = project_.QueryMapRuntimeState();
                if (!state.Succeeded()) return Mcp::Failure(state);
                auto result = State();
                const auto mode = state.value->settings.mode;
                result.insert("mode", mode == Services::MapRuntimeMode::Automatic ? "automatic"
                    : mode == Services::MapRuntimeMode::Enabled ? "enabled" : "disabled");
                result.insert("package_sha256", QString::fromStdString(state.value->settings.packageSha256));
                result.insert("approved", state.value->approved);
                QJsonArray packages;
                for (const auto& package : state.value->packages) {
                    const auto diagnostics = Mcp::Failure(Status{package.diagnostics}).value("diagnostics");
                    packages.append(QJsonObject{{"package_sha256", QString::fromStdString(package.packageSha256)},
                        {"id", QString::fromStdString(package.id)}, {"version", QString::fromStdString(package.version)},
                        {"payload_sha256", QString::fromStdString(package.payloadSha256)}, {"internal_only", package.internalOnly},
                        {"available", package.available}, {"diagnostics", diagnostics}});
                }
                result.insert("packages", packages);
                return result;
            };
            if (write) return Write(arguments, true, [&]() -> QJsonObject {
                const auto mode = arguments.value("mode").toString();
                const Services::MapRuntimeSettings settings{mode == "automatic" ? Services::MapRuntimeMode::Automatic
                    : mode == "enabled" ? Services::MapRuntimeMode::Enabled : Services::MapRuntimeMode::Disabled,
                    arguments.value("package_sha256").toString().toStdString()};
                const auto edited = QtProject::PublishMapRuntimeSettings(project_, settings, QObject::tr("设置地图运行库"));
                if (!edited.Succeeded()) return Mcp::Failure(edited);
                return query();
            });
            if (busy_ || project_.savingMap_) return Mcp::Failure("LIVE_EDITOR_BUSY", "地图写任务正在执行。");
            auto bound = arguments;
            if (!bound.contains("project_identity") && !bound.contains("expected_revision")) {
                const auto state = State(); bound.insert("project_identity", state.value("project_identity"));
                bound.insert("expected_revision", state.value("revision"));
            }
            const auto checked = CheckContext(bound, false, true);
            return checked.value("ok").toBool() ? query() : checked;
        }});
    }

    void RegisterTools() {
        for (auto definition : Mcp::MapRuntimeToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) { return RuntimeTool(name, args); },
                definition.maxRequestIdBytes, definition.maxResponseBytes);
        }
        for (auto definition : Mcp::WorldViewportToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) {
                    const auto check = CheckContext(args, false, false);
                    if (!check.value("ok").toBool()) return check;
                    return Mcp::InvokeWorldViewportTool(name, args, {worldViewport_});
                }, definition.maxRequestIdBytes, definition.maxResponseBytes, definition.pngImageResult);
        }
        for (auto definition : Mcp::GameUiToolDefinitions()) {
            const auto name = definition.name;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name](const QJsonObject& args) {
                    const auto check = CheckContext(args, false, false);
                    if (!check.value("ok").toBool()) return check;
                    return Mcp::InvokeGameUiTool(name, args, {gameUi_});
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
                    if (name == "lua_file_write" || name == "imports_add" || name == "imports_remove")
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
                        auto cache = project_.SharedCodeCheckCache();
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
        for (auto definition : Mcp::MapCustomSettingsToolDefinitions()) {
            const auto name = definition.name; const bool write = definition.permission == Mcp::ToolPermission::Edit;
            AddTool(name, definition.description, definition.permission, definition.inputSchema,
                [this, name, write](const QJsonObject& arguments) { return CustomSettingsTool(name, arguments, write); }, definition.maxRequestIdBytes);
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
        const auto graphQuery = Mcp::GraphQueryProperties();
        for (auto it = graphQuery.begin(); it != graphQuery.end(); ++it) query.insert(it.key(), it.value());
        AddTool("editor_query_trigger_graph",
            "查询作者当前可见的触发器（T）结构；ecas 表可用 functions（精确函数名）或 ui_layer（函数库层，如 WarVK）筛选；"
            "trigger_text 表配 trigger_id 把整个触发器导出为紧凑 T 写法；feature_text 表配 category_id 把整个类别导出为功能写法；"
            "presets 表（可配 type）列预设；function_examples 表配 functions 给出本图里的真实用法原句；"
            "outline 表一次列出全部类别和触发器的名字与编号，可用 match 按名字筛选；"
            "legacy_trigger_references 表（可配 trigger_id）列出旧版本编辑器写法的触发器引用，附带可直接交给 editor_edit_trigger_graph 的改正 operations", Mcp::ToolPermission::Query,
            Schema(query, {"project_identity", "expected_revision", "table"}), [this](const QJsonObject& arguments) { return Query(arguments); });
        auto edit = Mcp::GraphOperationSchema();
        auto properties = edit.value("properties").toObject();
        const auto identity = IdentityProperties();
        for (auto it = identity.begin(); it != identity.end(); ++it) properties.insert(it.key(), it.value());
        edit.insert("properties", properties);
        properties.insert("dry_run", QJsonObject{{"type", "boolean"}, {"default", false}});
        edit.insert("properties", properties);
        edit.insert("required", QJsonArray{"project_identity", "expected_revision", "expected", "operations"});
        AddTool("editor_edit_trigger_graph", "在共用撤销栈中整批编辑触发器（T）；insert_text / replace_trigger_text 可用紧凑 T 写法一次写入整段逻辑，apply_feature_text 一次写入一个功能的变量与多个触发器",
            Mcp::ToolPermission::Edit, edit,
            [this](const QJsonObject& arguments) { return Edit(arguments); });
        {
            // 与 editor_edit_trigger_graph 相同的批次字段，另加检查任务字段；批次只用于私有候选，不发布。
            auto checkSchema = Mcp::GraphOperationSchema();
            auto checkProperties = checkSchema.value("properties").toObject();
            for (auto it = identity.begin(); it != identity.end(); ++it) checkProperties.insert(it.key(), it.value());
            const auto codeCheck = Mcp::CodeCheckToolDefinition().inputSchema.value("properties").toObject();
            for (const auto* key : {"action", "request_id", "offset", "limit"}) checkProperties.insert(key, codeCheck.value(key));
            checkProperties.insert("wait_ms", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}});
            checkSchema.insert("properties", checkProperties);
            checkSchema.insert("required", QJsonArray{"project_identity", "expected_revision"});
            AddTool("editor_check_trigger_graph",
                "按保存链生成整图脚本并做同一 JASS 检查（模板与函数库同保存）；可附 operations 批次，只在私有副本上应用、不发布。"
                "submit 返回 request_id，status 读取结果，wait_ms 可直接等待；报错落在 T 语句上时带 t 字段（trigger_text 导出的行列与函数名）",
                Mcp::ToolPermission::Query, checkSchema, [this](const QJsonObject& arguments) { return CheckTriggers(arguments); });
        }
        auto save = IdentityProperties(); save.insert("path", StringSchema()); save.insert("in_place", QJsonObject{{"type", "boolean"}, {"default", false}});
        save.insert("runtime_purpose", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"normal", "internal_validation"}}, {"default", "normal"}});
        AddTool("editor_save_map", "通过原编辑器地图保存服务写回或另存", Mcp::ToolPermission::Save,
            Schema(save, {"project_identity", "expected_revision"}), [this](const QJsonObject& arguments) { return Save(arguments); });
        auto catalog = IdentityProperties();
        catalog.insert("kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"event", "condition", "action", "call"}}});
        catalog.insert("search", StringSchema()); catalog.insert("start", QJsonObject{{"type", "integer"}});
        catalog.insert("limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}});
        AddTool("editor_get_ui_catalog", "查询当前地图实际生效的 T 函数签名；不给 kind 时搜索事件、条件、动作和函数全部种类", Mcp::ToolPermission::Query,
            Schema(catalog, {"project_identity", "expected_revision"}), [this](const QJsonObject& arguments) { return Catalog(arguments); });
        registeredNames_.sort();
    }

    EditorProject& project_;
    LiveEditorTools& owner_;
    QPointer<LiveEditorTools> ownerHandle_;
    Mcp::McpServer server_;
    std::function<bool(const std::filesystem::path&, bool)> save_;
    std::function<bool(const std::filesystem::path&)> validationSave_;
    std::function<bool()> beforeWrite_;
    std::function<QJsonObject(bool)> luaTest_;
    std::function<QJsonObject(const QString&, const QJsonObject&)> mapTest_;
    std::function<QJsonObject(const QString&, const QJsonObject&)> worldViewport_;
    std::function<QJsonObject(const QString&, const QJsonObject&)> gameUi_;
    ProjectSearchRevisionQuery projectSearchRevision_;
    ProjectSearchPageQuery projectSearchQuery_;
    std::shared_ptr<const int> projectSearchToken_;
    QString bindingKey_;
    std::optional<Trigger::TriggerGraphBinding> cachedBinding_;
    QStringList registeredNames_;
    bool busy_ = false;
    QString projectIdentity_;
    std::filesystem::path root_, archive_, uiRoot_;
    std::uint64_t generation_ = 0;
    TriggerDocument* document_ = nullptr;
    Trigger::UiCatalog* catalog_ = nullptr;
    std::string configSha_;
    Mcp::CodeCheckToolSession codeChecks_;
    bool waitingForCheck_ = false;
    std::unique_ptr<LiveObjectResourceTools> objectResources_;
};

LiveEditorTools::LiveEditorTools(QtProject::EditorProject& project, QObject* parent)
    : QObject(parent), impl_(std::make_shared<Impl>(project, *this)) {}
LiveEditorTools::~LiveEditorTools() = default;
Mcp::McpServer& LiveEditorTools::Server() noexcept { return impl_->Server(); }
void LiveEditorTools::SetSaveHandler(std::function<bool(const std::filesystem::path&, bool)> handler) { impl_->SetSaveHandler(std::move(handler)); }
void LiveEditorTools::SetValidationSaveHandler(std::function<bool(const std::filesystem::path&)> handler) { impl_->SetValidationSaveHandler(std::move(handler)); }
void LiveEditorTools::SetLuaTestHandler(std::function<QJsonObject(bool)> handler) { impl_->SetLuaTestHandler(std::move(handler)); }
void LiveEditorTools::SetMapTestHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) { impl_->SetMapTestHandler(std::move(handler)); }
void LiveEditorTools::SetWorldViewportHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
    impl_->SetWorldViewportHandler(std::move(handler));
}
void LiveEditorTools::SetGameUiHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler) {
    impl_->SetGameUiHandler(std::move(handler));
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

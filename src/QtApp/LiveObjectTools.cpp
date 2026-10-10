#include "LiveObjectTools.h"
#include "EditorProject.h"
#include "Commands/ObjectSnapshotCommand.h"
#include "McpObjectTools.h"
#include "McpToolInternal.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Workspace.h"

#include <QPointer>
#include <QUndoStack>

#include <array>
#include <memory>
#include <optional>

namespace War3Map::QtApp {
namespace {
using namespace ObjectData;
using QtProject::EditorProject;

class LiveObjectHost {
public:
    LiveObjectHost(EditorProject& project, QJsonObject identity) : project_(project), identity_(std::move(identity)) {}
    Mcp::ObjectToolHost Host() {
        return {
            [this] {
                return QJsonObject{{"project_identity", identity_.value("project_identity")},
                    {"revision", QString::number(project_.editRevision_.Revision())}};
            },
            [this](ObjectType type) { return Context(type); },
            [this](const Services::ObjectCommandPlan& plan) { return Commit(plan); },
            [this](const ObjectKey& key) { return project_.AssessDeletion(key); }
        };
    }
private:
    Result<Services::ObjectCommandContext> Context(ObjectType type) {
        if (!project_.standardCatalog_ || !project_.objectFieldSchema_)
            return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_METADATA_UNAVAILABLE",
                "当前地图没有完整的标准目录/字段元数据；先在设置中选择真实游戏来源。");
        const auto index = static_cast<std::size_t>(type);
        if (index >= project_.objectDocuments_.size())
            return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_KIND_INVALID", "物编类别无效。");
        if (!project_.objectDocuments_[index] && !empty_[index]) {
            if (project_.nativeObjectDocuments_[index])
                return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                    "原生物编存在但未完整转换，不能用空文档替代。");
            if (!project_.sourceArchivePath_.empty()) {
                const auto archive = Mpq::MpqArchive::OpenReadOnly(project_.sourceArchivePath_);
                if (!archive.Succeeded()) return {{}, archive.diagnostics};
                if (archive.value->HasFile(std::string(NativeFileName(type))))
                    return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                        "地图中已有此类物编文件，但没有可编辑文档；原文件保持不变。");
                const auto inventory = archive.value->ListFilesStrict();
                if (!inventory.Succeeded()) return {{}, inventory.diagnostics};
            } else if (project_.directorySnapshot_) {
                std::error_code error;
                for (const auto& path : {project_.projectRoot_ / "table" / LniFileName(type),
                    project_.projectRoot_ / LniFileName(type), project_.projectRoot_ / "map" / NativeFileName(type),
                    project_.projectRoot_ / NativeFileName(type)}) {
                    const bool exists = std::filesystem::exists(path, error);
                    if (error || exists)
                        return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE",
                            "此类物编来源存在或无法确认，不能创建猜测的空文档。");
                }
            } else return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_MAP_UNAVAILABLE", "先打开地图。");
            const auto parsed = LniObjectCodec(type).Parse({});
            const auto* empty = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
            if (!empty) return Result<Services::ObjectCommandContext>::Failure("Object.Live", "OBJECT_EMPTY_DOCUMENT_FAILED", "无法建立空物编快照。");
            empty_[index] = *empty;
        }
        Services::ObjectCommandContext context;
        context.type = type; context.standard = project_.standardCatalog_.get(); context.schema = project_.objectFieldSchema_.get();
        context.document = project_.objectDocuments_[index] ? project_.objectDocuments_[index].get() : &*empty_[index];
        for (std::size_t i = 0; i < context.documents.size(); ++i)
            context.documents[i] = project_.objectDocuments_[i].get();
        context.documents[index] = context.document;
        if (!nameMap_ && project_.MapEdits()) nameMap_ = std::make_shared<MapDocument>(project_.MapEdits()->Snapshot());
        context.nameMap = nameMap_ ? nameMap_.get() : project_.mapDocument_.get();
        return Result<Services::ObjectCommandContext>::Success(context);
    }
    Status Commit(const Services::ObjectCommandPlan& plan) {
        const auto index = static_cast<std::size_t>(plan.subject.type);
        const auto current = Context(plan.subject.type);
        if (!current.Succeeded()) return {current.diagnostics};
        if (!current.value->document || current.value->document->DocumentFingerprint() != plan.before.DocumentFingerprint())
            return Status::Failure("Object.Live", "OBJECT_PLAN_STALE", "命令不属于当前物编文档；地图未修改。");
        auto* stack = project_.objectUndoStacks_[index];
        if (!stack) return Status::Failure("Object.Live", "OBJECT_UNDO_UNAVAILABLE", "未接通此类物编的共用撤销栈。");
        const auto sources = project_.CheckProjectSources();
        if (!sources.Succeeded()) return sources;
        if (!project_.sourceArchivePath_.empty() && project_.nativeObjectDocuments_[index] &&
            !project_.nativeObjectDocuments_[index]->IsEditable())
            return Status::Failure("Object.Live", "OBJECT_NATIVE_OPERATION_UNSUPPORTED", "原生物编档位不支持完整结构编辑；没有放宽保存门禁。");
        std::unique_ptr<LniObjectDocument> newDocument;
        std::unique_ptr<NativeObjectDocument> newNative;
        if (!project_.objectDocuments_[index]) {
            newDocument = std::make_unique<LniObjectDocument>(plan.before);
            const auto native = NativeObjectDocument::CreateEmpty(plan.subject.type, 2);
            if (!native.Succeeded()) return Status{native.diagnostics};
            newNative = std::make_unique<NativeObjectDocument>(*native.value);
        }
        const QPointer<EditorProject> project(&project_);
        auto* expected = newDocument ? newDocument.get() : project_.objectDocuments_[index].get();
        auto command = std::make_unique<QtProject::ObjectSnapshotCommand>(plan.before, plan.after,
            [project, expected, index](const LniObjectDocument& snapshot) {
                if (!project || project->objectDocuments_[index].get() != expected) return;
                LniObjectDocument replacement(snapshot);
                // 值对象地址不变，既有控件/命令仍引用原文档。信号仅用于所有视图统一刷新。
                emit project->ObjectDocumentsAboutToReplace();
                *expected = std::move(replacement);
                project->ObserveObjectEdit(static_cast<int>(index));
                emit project->ObjectDocumentsReplaced();
            }, QString::fromStdString(plan.label));
        if (newDocument) {
            emit project_.ObjectDocumentsAboutToReplace();
            project_.objectDocuments_[index] = std::move(newDocument);
            project_.nativeObjectDocuments_[index] = std::move(newNative);
            emit project_.ObjectDocumentsReplaced();
        }
        stack->push(command.release());
        project_.ObserveObjectEdit(static_cast<int>(index));
        return Status::Ok();
    }
    EditorProject& project_;
    QJsonObject identity_;
    std::array<std::optional<LniObjectDocument>, 7> empty_;
    std::shared_ptr<MapDocument> nameMap_;
};
} // namespace

QJsonObject InvokeLiveObjectTool(EditorProject& project, const QString& name, const QJsonObject& arguments,
    const QJsonObject& currentIdentity) {
    LiveObjectHost host(project, currentIdentity);
    return Mcp::InvokeObjectTool(name, arguments, host.Host());
}

} // namespace War3Map::QtApp

#include "LiveObjectTools.h"
#include "EditorProject.h"
#include "ObjectContextProvider.h"
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
    LiveObjectHost(EditorProject& project, QJsonObject identity) : project_(project), identity_(std::move(identity)),
        context_(QtProject::MakeObjectContextProvider(project)) {}
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
    Result<Services::ObjectCommandContext> Context(ObjectType type) {
        return context_(type);
    }
private:
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
    QtProject::ObjectContextProvider context_;
};
} // namespace

LiveObjectContextProvider MakeLiveObjectContextProvider(EditorProject& project, const QJsonObject& currentIdentity) {
    Q_UNUSED(currentIdentity);
    return QtProject::MakeObjectContextProvider(project);
}

QJsonObject InvokeLiveObjectTool(EditorProject& project, const QString& name, const QJsonObject& arguments,
    const QJsonObject& currentIdentity) {
    LiveObjectHost host(project, currentIdentity);
    return Mcp::InvokeObjectTool(name, arguments, host.Host());
}

} // namespace War3Map::QtApp

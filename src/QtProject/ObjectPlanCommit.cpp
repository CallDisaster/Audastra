#include "ObjectPlanCommit.h"
#include "EditorProject.h"
#include "Commands/ObjectSnapshotCommand.h"

#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <QPointer>
#include <QUndoStack>

#include <memory>
#include <optional>

namespace War3Map::QtProject {
using namespace ObjectData;

Status CommitObjectCommandPlan(EditorProject& project, const Services::ObjectCommandPlan& plan) {
    const auto index = static_cast<std::size_t>(plan.subject.type);
    if (index >= project.objectDocuments_.size())
        return Status::Failure("Object.Live", "OBJECT_KIND_INVALID", "物编类别无效。");
    auto* stack = project.objectUndoStacks_[index];
    if (!stack) return Status::Failure("Object.Live", "OBJECT_UNDO_UNAVAILABLE", "未接通此类物编的共用撤销栈。");
    std::optional<LniObjectDocument> empty;
    const LniObjectDocument* current = project.objectDocuments_[index].get();
    if (!current) {
        if (project.nativeObjectDocuments_[index])
            return Status::Failure("Object.Live", "OBJECT_DOCUMENT_UNAVAILABLE", "原生物编存在但未完整转换，不能用空文档替代。");
        const auto parsed = LniObjectCodec(plan.subject.type).Parse({});
        const auto* document = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
        if (!document) return Status::Failure("Object.Live", "OBJECT_EMPTY_DOCUMENT_FAILED", "无法建立空物编快照。");
        empty = *document;
        current = &*empty;
    }
    if (current->DocumentFingerprint() != plan.before.DocumentFingerprint())
        return Status::Failure("Object.Live", "OBJECT_PLAN_STALE", "命令不属于当前物编文档；地图未修改。");
    const auto sources = project.CheckProjectSources();
    if (!sources.Succeeded()) return sources;
    if (!project.sourceArchivePath_.empty() && project.nativeObjectDocuments_[index] &&
        !project.nativeObjectDocuments_[index]->IsEditable())
        return Status::Failure("Object.Live", "OBJECT_NATIVE_OPERATION_UNSUPPORTED", "原生物编档位不支持完整结构编辑；没有放宽保存门禁。");
    std::unique_ptr<LniObjectDocument> newDocument;
    std::unique_ptr<NativeObjectDocument> newNative;
    if (!project.objectDocuments_[index]) {
        newDocument = std::make_unique<LniObjectDocument>(plan.before);
        const auto native = NativeObjectDocument::CreateEmpty(plan.subject.type, 2);
        if (!native.Succeeded()) return Status{native.diagnostics};
        newNative = std::make_unique<NativeObjectDocument>(*native.value);
    }
    const QPointer<EditorProject> guarded(&project);
    auto* expected = newDocument ? newDocument.get() : project.objectDocuments_[index].get();
    auto command = std::make_unique<ObjectSnapshotCommand>(plan.before, plan.after,
        [guarded, expected, index](const LniObjectDocument& snapshot) {
            if (!guarded || guarded->objectDocuments_[index].get() != expected) return;
            LniObjectDocument replacement(snapshot);
            // 值对象地址不变，既有控件/命令仍引用原文档。信号仅用于所有视图统一刷新。
            emit guarded->ObjectDocumentsAboutToReplace();
            *expected = std::move(replacement);
            guarded->ObserveObjectEdit(static_cast<int>(index));
            emit guarded->ObjectDocumentsReplaced();
        }, QString::fromStdString(plan.label));
    if (newDocument) {
        emit project.ObjectDocumentsAboutToReplace();
        project.objectDocuments_[index] = std::move(newDocument);
        project.nativeObjectDocuments_[index] = std::move(newNative);
        emit project.ObjectDocumentsReplaced();
    }
    stack->push(command.release());
    project.ObserveObjectEdit(static_cast<int>(index));
    return Status::Ok();
}

} // namespace War3Map::QtProject

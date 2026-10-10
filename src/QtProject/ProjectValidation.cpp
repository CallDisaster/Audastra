#include "EditorProject.h"

#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QUndoStack>

#include <algorithm>

// 结构验证与旁路候选导出：只写诊断，不弹窗；输出位置由界面选定后传入。
namespace War3Map::QtProject {
namespace {

constexpr std::array kObjectTypes{
    ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability, ObjectData::ObjectType::Item,
    ObjectData::ObjectType::Buff, ObjectData::ObjectType::Upgrade, ObjectData::ObjectType::Doodad,
    ObjectData::ObjectType::Destructable,
};

} // namespace

ValidationSummary EditorProject::ValidateDocuments() {
    ValidationSummary summary;
    if (projectRoot_.empty()) {
        AddDiagnostic(tr("警告"), tr("验证"), tr("NO_PROJECT"), tr("请先打开展开地图目录。"));
        return summary;
    }
    summary.opened = true;
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        const auto* document = objectDocuments_[index].get();
        if (document == nullptr) continue;
        const Status status = ObjectData::LniObjectCodec(kObjectTypes[index]).Validate(*document);
        if (status.Succeeded()) {
            ++summary.objectTypesPassed;
            continue;
        }
        ++summary.objectTypesFailed;
        for (const auto& diagnostic : status.diagnostics) {
            AddDiagnostic(tr("错误"), QString::fromUtf8(diagnostic.subsystem),
                QString::fromUtf8(diagnostic.code), QString::fromUtf8(diagnostic.message));
        }
    }
    const bool objectsPassed = summary.objectTypesFailed == 0;
    AddDiagnostic(objectsPassed ? tr("信息") : tr("错误"), tr("验证"),
        objectsPassed ? tr("LNI_VALIDATION_PASSED") : tr("LNI_VALIDATION_FAILED"),
        tr("LNI 文档验证完成：%1 类通过，%2 类失败。该结果不等于 World Editor/YDWE 真保存。")
            .arg(summary.objectTypesPassed).arg(summary.objectTypesFailed));

    if (triggerDocument_ == nullptr || uiCatalog_ == nullptr) {
        AddDiagnostic(tr("警告"), tr("验证"), tr("WTG_WCT_NOT_READY"),
            tr("触发器尚未按匹配的动态 UI Catalog 打开；本次只验证物体数据。"));
        return summary;
    }
    summary.triggersValidated = true;
    const Status triggerStatus = Trigger::WtgWctCodec{}.Validate(*triggerDocument_, *uiCatalog_);
    summary.triggersPassed = triggerStatus.Succeeded();
    AddDiagnostics(triggerStatus.diagnostics);
    AddDiagnostic(summary.triggersPassed ? tr("信息") : tr("错误"), tr("验证"),
        summary.triggersPassed ? tr("WTG_WCT_VALIDATION_PASSED") : tr("WTG_WCT_VALIDATION_FAILED"),
        summary.triggersPassed ? tr("WTG/WCT 结构、WCT 槽位与来源身份验证通过。")
                               : tr("WTG/WCT 验证失败；不会生成触发器候选。"));
    return summary;
}

bool EditorProject::HasPendingCandidateEdits() const {
    return !projectRoot_.empty() &&
        (triggerDirty_ || std::any_of(objectDirty_.begin(), objectDirty_.end(), [](bool dirty) { return dirty; }));
}

bool EditorProject::ExportCandidate(const std::filesystem::path& outputPath) {
    const auto report = [this](const std::vector<Diagnostic>& diagnostics) { AddDiagnostics(diagnostics); };
    if (!HasPendingCandidateEdits()) {
        AddDiagnostic(tr("警告"), tr("导出"), tr("NO_PENDING_EDIT"), tr("当前没有可导出的事务修改。"));
        return false;
    }
    // 函数目录变了的会话不能继续用于触发器补丁。
    if (session_ != nullptr && triggerDirty_ && uiCatalog_ != nullptr &&
        session_->Revision().uiCatalogSha256 != uiCatalog_->Sha256()) {
        session_.reset();
    }
    if (session_ == nullptr) {
        auto opened = EditorSession::Open(OpenRequest{
            projectRoot_, CompatibilityProfile::Default(),
            uiCatalog_ == nullptr ? std::string(64, '0') : uiCatalog_->Sha256(),
        });
        if (!opened.Succeeded()) {
            report(opened.diagnostics);
            return false;
        }
        session_ = std::make_unique<EditorSession>(std::move(*opened.value));
    }
    const ProjectRevision revision = session_->Revision();
    auto transaction = session_->BeginEdit(revision);
    if (!transaction.Succeeded()) {
        report(transaction.diagnostics);
        return false;
    }
    const TransactionId transactionId = *transaction.value;
    const auto rollback = [this, transactionId]() { (void)session_->Rollback(transactionId); };

    if (triggerDirty_) {
        if (triggerDocument_ == nullptr || uiCatalog_ == nullptr) {
            AddDiagnostic(tr("错误"), tr("导出"), tr("TRIGGER_DOCUMENT_NOT_READY"),
                tr("触发器修改存在，但 WTG/WCT 或 UI Catalog 已失效。"));
            rollback();
            return false;
        }
        auto patch = Trigger::WtgWctCodec{}.BuildPatch(*triggerDocument_, *uiCatalog_,
            std::filesystem::path("map") / "war3map.wtg", std::filesystem::path("map") / "war3map.wct",
            "Audastra 触发器事务修改");
        if (!patch.Succeeded()) {
            report(patch.diagnostics);
            rollback();
            return false;
        }
        const Status applied = session_->ApplyTriggerPatch(transactionId, std::move(*patch.value));
        if (!applied.Succeeded()) {
            report(applied.diagnostics);
            rollback();
            return false;
        }
    }
    for (std::size_t index = 0; index < kObjectTypes.size(); ++index) {
        if (!objectDirty_[index] || objectDocuments_[index] == nullptr) continue;
        const std::filesystem::path relative =
            std::filesystem::path("table") / std::string(ObjectData::LniFileName(kObjectTypes[index]));
        auto patch = ObjectData::LniObjectCodec(kObjectTypes[index]).BuildPatch(
            *objectDocuments_[index], relative, "Audastra 物编事务修改");
        if (!patch.Succeeded()) {
            report(patch.diagnostics);
            rollback();
            return false;
        }
        const Status applied = session_->ApplyObjectPatch(transactionId, std::move(*patch.value));
        if (!applied.Succeeded()) {
            report(applied.diagnostics);
            rollback();
            return false;
        }
    }
    const auto preview = session_->PreviewEdit(transactionId);
    const ValidationReport validation = session_->Validate(transactionId, session_->Profile(), CompatibilityLevel::Parsed);
    if (!preview.Succeeded() || !validation.satisfied) {
        AddDiagnostic(tr("错误"), tr("导出"), tr("TRANSACTION_VALIDATION_FAILED"), tr("事务预览或 L0 验证失败，候选未生成。"));
        rollback();
        return false;
    }
    const Status committed = session_->CommitCandidate(transactionId, revision, outputPath);
    if (!committed.Succeeded()) {
        report(committed.diagnostics);
        rollback();
        return false;
    }
    objectDirty_.fill(false);
    triggerDirty_ = false;
    if (triggerUndoStack_ != nullptr) triggerUndoStack_->setClean();
    AddDiagnostic(tr("信息"), tr("导出"), tr("CANDIDATE_COMMITTED"),
        tr("已生成旁路候选与 JSON manifest：%1。尚未取得 World Editor/YDWE 真保存证据。")
            .arg(QString::fromStdWString(outputPath.wstring())));
    return true;
}

} // namespace War3Map::QtProject

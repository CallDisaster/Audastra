#include "ProjectRemovalCodeAudit.h"
#include "EditorProject.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <QCoreApplication>
#include <algorithm>
#include <unordered_map>

namespace War3Map::QtProject {
namespace {
QString Tr(const char* text) { return QCoreApplication::translate("RemovalCodeAudit", text); }
QString U8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString Snippet(std::string_view text) {
    auto value = U8(text).simplified();
    if (value.size() > 80) value = value.left(79) + QChar(0x2026);
    return value;
}
enum class Use { None, Reference, Possible };
// 审计证据分三类：确定引用；须作者判断的按名使用（字符串里的名字、动态调用目标、拼接宏等）；
// 其余（调用位置标记、注释里的提及、来源不完整的标记）不算引用，来源不完整由所在块的原因说明。
Use Classify(const Trigger::RemovalReferenceEvidence& hit) {
    if (hit.definite) return Use::Reference;
    if (hit.context.find("须单独确认") != std::string::npos || hit.context.find("须审阅") != std::string::npos) return Use::Possible;
    return Use::None;
}
constexpr qsizetype kShownLines = 8;
} // namespace

RemovalCodeAudit RemovalCodeAudit::Prepare(const EditorProject& project,
    std::vector<Trigger::GraphRemove> targets, std::set<StableId> removedTriggers) {
    RemovalCodeAudit audit;
    audit.targets_ = std::move(targets);
    audit.removedTriggers_ = std::move(removedTriggers);
    audit.generation_ = project.editRevision_.Generation();
    audit.revision_ = project.editRevision_.Revision();
    audit.snapshot_ = project.CaptureCodeCheckSnapshot();
    if (!audit.snapshot_ || !audit.snapshot_->triggers || !audit.snapshot_->catalog) {
        audit.unavailable_ = Tr("工程触发器尚不可用");
        return audit;
    }
    // 与 Agent 查询相同：物体引用来源取不到或为空时按无物体引用审计。
    const auto objects = project.CaptureTriggerMapObjectReferences();
    if (objects.Succeeded() && objects.value && !(*objects.value)->References().empty()) audit.mapObjects_ = *objects.value;
    return audit;
}

std::optional<RemovalCodeNotice> RemovalCodeAudit::Run(
    const std::function<void(std::size_t, std::size_t)>& progress, const std::atomic_bool& stop) const {
    RemovalCodeNotice notice;
    if (!unavailable_.isEmpty()) {
        notice.incomplete = unavailable_;
        return notice;
    }
    const auto& document = *snapshot_->triggers;
    // 参数代码块所属的触发器：显示“触发器 · 自定义脚本”，并排除一起删掉的触发器。
    std::unordered_map<StableId, StableId> argumentOwner;
    for (const auto& eca : document.ecaNodes)
        for (const auto index : eca.arguments)
            if (index < document.argumentNodes.size()) argumentOwner.emplace(document.argumentNodes[index].id, eca.sourceTriggerId);
    const auto triggerName = [&](StableId id) {
        const auto found = std::find_if(document.triggers.begin(), document.triggers.end(), [id](const auto& t) { return t.id == id; });
        return found == document.triggers.end() ? Tr("未知触发器") : U8(found->name.text);
    };
    const auto owner = [&](const Trigger::RemovalBlockIdentity& block) -> std::optional<QString> {
        using Kind = Trigger::AtomicRenameCodeKind;
        switch (block.kind) {
        case Kind::GlobalCustomCode: return Tr("全局自定义代码");
        case Kind::TriggerCustomCode:
            if (removedTriggers_.contains(block.id)) return std::nullopt;
            return triggerName(block.id);
        case Kind::ArgumentCode: {
            const auto found = argumentOwner.find(block.id);
            if (found != argumentOwner.end() && removedTriggers_.contains(found->second)) return std::nullopt;
            return (found == argumentOwner.end() ? Tr("未知触发器") : triggerName(found->second)) + Tr(" · 自定义脚本");
        }
        case Kind::VariableInitialValue: {
            const auto found = std::find_if(document.variables.begin(), document.variables.end(), [&](const auto& v) { return v.id == block.id; });
            return Tr("变量 %1 初始值").arg(found == document.variables.end() ? QString() : U8(found->name.text));
        }
        }
        return Tr("代码");
    };
    const auto incomplete = [&](const QString& reason) { if (notice.incomplete.isEmpty()) notice.incomplete = reason; };
    const auto add = [&](const QString& kind, const QString& at, const QString& text) {
        if (++notice.references <= static_cast<std::size_t>(kShownLines))
            notice.lines << (text.isEmpty() ? QStringLiteral("%1 · %2").arg(kind, at) : QStringLiteral("%1 · %2 · %3").arg(kind, at, text));
    };
    const auto provider = Services::MakeProjectRemovalPreprocessProvider(snapshot_);
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (stop.load()) return std::nullopt;
        if (progress) progress(i, targets_.size());
        const auto review = Trigger::QueryDeclarationRemovalAudit(document, *snapshot_->catalog, targets_[i], mapObjects_.get(), &provider);
        if (!review.Succeeded() || !review.value) {
            incomplete(review.diagnostics.empty() ? Tr("代码审计没有完成") : U8(review.diagnostics.front().message));
            continue;
        }
        // 触发器之间的引用由目录的引用索引列出；变量没有这份索引，用审计里的触发器参数引用。
        if (targets_[i].target.kind == Trigger::GraphRecordKind::Variable)
            for (const auto id : review.value->typedArgumentIds) {
                const auto found = argumentOwner.find(id);
                if (found != argumentOwner.end() && removedTriggers_.contains(found->second)) continue;
                add(Tr("触发器参数"), found == argumentOwner.end() ? Tr("未知触发器") : triggerName(found->second), {});
            }
        for (const auto& block : review.value->individualBlocks) {
            const auto where = owner(block.identity);
            if (!where) continue;
            if (!block.complete) incomplete(*where + Tr("：") + U8(block.incompleteReason));
            for (const auto& hit : block.evidence) {
                const auto use = Classify(hit);
                if (use == Use::None) continue;
                add(use == Use::Reference ? Tr("代码引用") : Tr("可能按名使用"),
                    hit.sourceFile.empty() ? Tr("%1 第 %2 行").arg(*where).arg(hit.line)
                                           : Tr("%1 第 %2 行").arg(U8(hit.sourceFile)).arg(hit.sourceLine),
                    Snippet(hit.text));
            }
        }
    }
    if (notice.references > static_cast<std::size_t>(kShownLines))
        notice.lines << Tr("还有 %1 处").arg(notice.references - static_cast<std::size_t>(kShownLines));
    return notice;
}

bool RemovalCodeAudit::Current(const EditorProject& project) const {
    // 快照身份已含修订、引用索引与来源变化标记；没有快照时只核对修订。
    if (snapshot_ && !project.IsCodeCheckBindingCurrent(snapshot_->binding)) return false;
    return project.editRevision_.Generation() == generation_ && project.editRevision_.Revision() == revision_;
}
} // namespace War3Map::QtProject

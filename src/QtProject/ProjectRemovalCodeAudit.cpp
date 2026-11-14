#include "ProjectRemovalCodeAudit.h"
#include "EditorProject.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Trigger/TriggerCodeReferences.h"

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
    if (!audit.snapshot_ || !audit.snapshot_->triggers || !audit.snapshot_->catalog)
        audit.unavailable_ = Tr("工程触发器尚不可用");
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
    if (progress) progress(0, 1);
    // 参数所属的触发器：显示“触发器 · 自定义脚本”，并排除一起删掉的触发器。
    std::unordered_map<StableId, StableId> argumentOwner;
    for (const auto& eca : document.ecaNodes)
        for (const auto index : eca.arguments)
            if (index < document.argumentNodes.size()) argumentOwner.emplace(document.argumentNodes[index].id, eca.sourceTriggerId);
    const auto triggerName = [&](StableId id) {
        const auto found = std::find_if(document.triggers.begin(), document.triggers.end(), [id](const auto& t) { return t.id == id; });
        return found == document.triggers.end() ? Tr("未知触发器") : U8(found->name.text);
    };
    const auto add = [&](const QString& kind, const QString& at, const QString& text) {
        if (++notice.references <= static_cast<std::size_t>(kShownLines))
            notice.lines << (text.isEmpty() ? QStringLiteral("%1 · %2").arg(kind, at) : QStringLiteral("%1 · %2 · %3").arg(kind, at, text));
    };
    // 变量：触发器参数里的直接引用（触发器之间的引用由目录的引用索引列出）。
    for (const auto& target : targets_) {
        if (target.target.kind != Trigger::GraphRecordKind::Variable) continue;
        const std::string symbol = "udg_" + target.expectedName;
        for (const auto& argument : document.argumentNodes) {
            if (argument.type != 1 || (argument.value.text != symbol && argument.value.text != target.expectedName)) continue;
            const auto found = argumentOwner.find(argument.id);
            if (found != argumentOwner.end() && removedTriggers_.contains(found->second)) continue;
            add(Tr("触发器参数"), found == argumentOwner.end() ? Tr("未知触发器") : triggerName(found->second), {});
        }
    }
    if (stop.load()) return std::nullopt;
    std::vector<Trigger::GraphRecordRef> refs;
    refs.reserve(targets_.size());
    for (const auto& target : targets_) refs.push_back(target.target);
    const auto found = Trigger::FindDeclarationCodeReferences(document, *snapshot_->catalog, refs);
    if (!found.Succeeded() || !found.value) {
        notice.incomplete = found.diagnostics.empty() ? Tr("代码引用检查没有完成") : U8(found.diagnostics.front().message);
        return notice;
    }
    using Kind = Trigger::AtomicRenameCodeKind;
    for (const auto& hit : *found.value) {
        QString where;
        switch (hit.kind) {
        case Kind::GlobalCustomCode: where = Tr("全局自定义代码 第 %1 行").arg(hit.line); break;
        case Kind::TriggerCustomCode:
            if (removedTriggers_.contains(hit.blockId)) continue;
            where = Tr("%1 第 %2 行").arg(triggerName(hit.blockId)).arg(hit.line);
            break;
        case Kind::ArgumentCode: {
            const auto owner = argumentOwner.find(hit.blockId);
            if (owner != argumentOwner.end() && removedTriggers_.contains(owner->second)) continue;
            where = (owner == argumentOwner.end() ? Tr("未知触发器") : triggerName(owner->second)) + Tr(" · 自定义脚本");
            break;
        }
        case Kind::VariableInitialValue: {
            const auto variable = std::find_if(document.variables.begin(), document.variables.end(), [&](const auto& v) { return v.id == hit.blockId; });
            where = Tr("变量 %1 初始值").arg(variable == document.variables.end() ? QString() : U8(variable->name.text));
            break;
        }
        }
        add(Tr("代码引用"), where, Snippet(hit.excerpt));
    }
    if (notice.references > static_cast<std::size_t>(kShownLines))
        notice.lines << Tr("还有 %1 处").arg(notice.references - static_cast<std::size_t>(kShownLines));
    return notice;
}

Trigger::RemovalPreprocessProvider LazyRemovalPreprocessProvider(const EditorProject& project) {
    return [&project, provider = Trigger::RemovalPreprocessProvider{}](const auto& document, const auto& catalog, const auto& operation) mutable {
        if (!provider) provider = Services::MakeProjectRemovalPreprocessProvider(project.CaptureCodeCheckSnapshot());
        return provider(document, catalog, operation);
    };
}

bool RemovalCodeAudit::Current(const EditorProject& project) const {
    // 快照身份已含修订、引用索引与来源变化标记；没有快照时只核对修订。
    if (snapshot_ && !project.IsCodeCheckBindingCurrent(snapshot_->binding)) return false;
    return project.editRevision_.Generation() == generation_ && project.editRevision_.Revision() == revision_;
}
} // namespace War3Map::QtProject

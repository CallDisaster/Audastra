#include "ObjectModule.h"
#include "ObjectModuleShared.h"
#include "Widgets/ObjectTreeBrowser.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectNames.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectReferenceIndex.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>

#include <algorithm>
#include <set>

namespace War3Map::Modules {

using namespace ObjectData;
using namespace ObjectDetail;

namespace {

// 删除确认里每类最多列几处；其余只给数量，细节在物体管理里看。
constexpr qsizetype kListed = 6;

QString ObjectLabel(const QtProject::EditorProject& project, const ObjectKey& key) {
    const auto name = ResolveProjectObjectPresentation(project, key).name;
    const auto id = U8(key.rawcode);
    return name.isEmpty() || name == id ? id : QStringLiteral("%1（%2）").arg(name, id);
}

// 代码候选位置的来源换成作者看得懂的说法：全局代码 / 某触发器的自定义代码。
QString ScriptPlace(const QtProject::EditorProject& project, const Services::ScriptReferenceHint& hint) {
    constexpr std::string_view triggerPrefix = "war3map.wct:trigger:";
    QString place = U8(hint.source);
    if (hint.source == "war3map.wct:header") {
        place = QObject::tr("全局自定义代码");
    } else if (std::string_view(hint.source).starts_with(triggerPrefix) && project.triggerDocument_) {
        const auto id = std::string_view(hint.source).substr(triggerPrefix.size());
        for (const auto& trigger : project.triggerDocument_->triggers)
            if (std::to_string(trigger.id) == id) place = QObject::tr("触发器“%1”的自定义代码").arg(U8(trigger.name.text));
    }
    return QObject::tr("%1 第 %2 行").arg(place).arg(hint.line);
}

// 引用位置换成作者的说法：哪个物体的哪个字段 / 地图上放置的实例；放置类按目标合并成一行计数。
QString ReferencePlace(const QtProject::EditorProject& project, const IndexedObjectReference& reference) {
    if (reference.ownerObject) {
        const QString owner = ObjectLabel(project, *reference.ownerObject);
        if (reference.origin == IndexedReferenceOrigin::ObjectParent) return QObject::tr("%1 以它为父物体").arg(owner);
        QString field = U8(reference.fieldId);
        if (const auto* definition = project.objectFieldSchema_
                ? project.objectFieldSchema_->FindNative(reference.ownerObject->type, reference.fieldId) : nullptr;
            definition && !definition->displayName.empty())
            field = QStringLiteral("%1（%2）").arg(U8(definition->displayName), field);
        return QObject::tr("%1 的字段 %2").arg(owner, field);
    }
    return U8(reference.location);
}

bool Placement(const IndexedObjectReference& reference) {
    return !reference.ownerObject && reference.placementIndex.has_value();
}

void AppendList(QString& body, const QStringList& rows, qsizetype total) {
    for (qsizetype i = 0; i < std::min(rows.size(), kListed); ++i) body += QStringLiteral("\n• ") + rows[i];
    if (total > kListed) body += QObject::tr("\n……另有 %1 处").arg(total - kListed);
}

} // namespace

void ObjectModule::DeleteSelectedObject() {
    const int index = currentObjectType_;
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size())) return;
    const auto type = static_cast<ObjectType>(index);
    // 目录多选优先；列表页只删当前行。原版物体不能删，只出现在目录里时不进入候选。
    QStringList targets = SelectedCustomObjects();
    if (targets.isEmpty() && objectListView_ && objectFilter_) {
        const auto current = objectListView_->currentIndex();
        const auto* object = current.isValid() ? objectListModel_->ObjectAt(objectFilter_->mapToSource(current).row()) : nullptr;
        if (object && object->parentRawcode) targets << U8(object->key.rawcode);
    }
    if (targets.isEmpty()) {
        ShowStatus(tr("只能删除地图里的自定义物体"), 5000);
        return;
    }
    if (!EnsureObjectDocument(index)) return;

    // 审计按删除前的整张地图做；同批里互相引用的物体按确定引用拦下，作者分两次删。
    std::vector<Services::ObjectDeletionAssessment> assessments;
    QStringList refusals, possible, gui, scripts;
    qsizetype possibleTotal = 0, guiTotal = 0, scriptTotal = 0;
    bool dynamic = false;
    for (const auto& id : targets) {
        const ObjectKey key{type, id.toStdString()};
        auto assessment = project_->AssessDeletion(key);
        const QString label = ObjectLabel(*project_, key);
        qsizetype placed = 0;
        for (const auto& reference : assessment.objectReferences.definite) {
            if (Placement(reference)) ++placed;
            else refusals << tr("%1：%2").arg(label, ReferencePlace(*project_, reference));
        }
        if (placed != 0) refusals << tr("%1：地图上放置了 %2 处").arg(label).arg(placed);
        for (const auto& reference : assessment.objectReferences.possible)
            possible << tr("%1：%2").arg(label, ReferencePlace(*project_, reference));
        possibleTotal += static_cast<qsizetype>(assessment.objectReferences.possible.size());
        std::set<std::string> owners;
        for (const auto& usage : assessment.untypedGuiReferences)
            if (owners.insert(usage.ownerId).second) gui << tr("%1：触发器“%2”").arg(label, U8(usage.owner));
        guiTotal += static_cast<qsizetype>(owners.size());
        for (const auto& hint : assessment.scriptHints) scripts << tr("%1：%2").arg(label, ScriptPlace(*project_, hint));
        scriptTotal += static_cast<qsizetype>(assessment.scriptHintCount);
        dynamic = dynamic || assessment.dynamicValueCount != 0;
        assessments.push_back(std::move(assessment));
    }
    const QString title = tr("删除物体");
    const QString subject = targets.size() == 1 ? ObjectLabel(*project_, {type, targets.front().toStdString()})
                                                : tr("%1 个自定义物体").arg(targets.size());
    if (!refusals.isEmpty()) {
        QString body = tr("不能删除%1，它还在被使用：").arg(subject);
        AppendList(body, refusals, refusals.size());
        body += tr("\n\n先改掉这些引用再删除。");
        project_->AddDiagnostic(tr("警告"), tr("物体数据"), QStringLiteral("OBJECT_DELETE_REFERENCED"), body);
        ShowStatus(tr("未删除：物体还在被使用"), 7000);
        if (confirm_) {
            (void)confirm_(title, body, false);
            return;
        }
        QMessageBox box(QMessageBox::Warning, title, body, QMessageBox::NoButton, page_);
        QPushButton* show = box.addButton(tr("查看引用"), QMessageBox::ActionRole);
        box.addButton(tr("关闭"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == show && host_)
            host_->Navigate({"manager", "object", Kind(type) + ':' + targets.front(), {}});
        return;
    }

    // 先不发凭证试一遍：来源读不全、原版物体等作者也判断不了的情况在这里就拒绝，不弹确认。
    Services::ObjectCommandContext context;
    context.type = type;
    context.document = project_->objectDocuments_[index].get();
    context.standard = project_->standardCatalog_.get();
    context.schema = project_->objectFieldSchema_.get();
    for (std::size_t i = 0; i < assessments.size(); ++i) {
        const auto probe = Services::PlanConfirmedObjectDeletion(context, assessments[i].subject.rawcode, assessments[i], false);
        const bool onlyNeedsApproval = !probe.Succeeded() && std::all_of(probe.diagnostics.begin(), probe.diagnostics.end(),
            [](const Diagnostic& diagnostic) { return diagnostic.code == "object-delete-approval-required"; });
        if (!onlyNeedsApproval) {
            project_->AddDiagnostics(probe.diagnostics);
            ShowStatus(tr("未删除：%1").arg(probe.diagnostics.empty() ? tr("无法确认引用")
                : U8(probe.diagnostics.front().message)), 8000);
            return;
        }
    }

    QString body = tr("删除%1？").arg(subject);
    if (possibleTotal + guiTotal + scriptTotal == 0) {
        body += tr("\n\n没有找到引用。");
    } else {
        body += tr("\n\n没有找到确定的引用，但下面这些地方出现了同样的编号，不能确定是不是在用它：");
        AppendList(body, possible + gui + scripts, possibleTotal + guiTotal + scriptTotal);
    }
    if (dynamic) body += tr("\n\n按变量或函数取值的参数、地图导入的脚本无法静态检查。");
    body += tr("\n删除后可以撤销。");
    bool accepted = false;
    if (confirm_) {
        accepted = confirm_(title, body, true);
    } else {
        QMessageBox box(QMessageBox::Question, title, body, QMessageBox::NoButton, page_);
        QPushButton* remove = box.addButton(tr("删除"), QMessageBox::DestructiveRole);
        QPushButton* cancel = box.addButton(tr("取消"), QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.exec();
        accepted = box.clickedButton() == remove;
    }
    // 对话框期间文档可能已被替换；按当前文档重新规划，指纹不符时删除凭证不会生效。
    if (!accepted || !project_->objectDocuments_[index]) return;

    LniObjectDocument working = *project_->objectDocuments_[index];
    context.document = &working;
    for (const auto& assessment : assessments) {
        const auto plan = Services::PlanConfirmedObjectDeletion(context, assessment.subject.rawcode, assessment, true);
        if (!plan.Succeeded()) {
            project_->AddDiagnostics(plan.diagnostics);
            ShowStatus(tr("未删除：%1").arg(plan.diagnostics.empty() ? tr("删除失败")
                : U8(plan.diagnostics.front().message)), 8000);
            return;
        }
        working = plan.value->after;
    }
    CommitObjectCreation(index, std::move(working), {},
        targets.size() == 1 ? tr("删除物体 %1").arg(targets.front()) : tr("删除 %1 个物体").arg(targets.size()));
    ShowStatus(tr("已删除%1").arg(subject), 5000);
}

} // namespace War3Map::Modules

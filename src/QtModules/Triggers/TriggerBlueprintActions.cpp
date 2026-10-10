#include "TriggerModule.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "TriggerBlueprint.h"
#include "TriggerModels.h"

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

// 传统视图 / 蓝图切换与两者的同步。蓝图的所有编辑都经传统视图的语句操作（选中项先同步到语句树）。
namespace War3Map::Modules {
namespace {

QString KindText(std::int32_t type) {
    switch (type) {
    case 0: return QObject::tr("事件");
    case 1: return QObject::tr("条件");
    default: return QObject::tr("动作");
    }
}

} // namespace

void TriggerModule::BuildBlueprint(QWidget* pane, QVBoxLayout* layout, QHBoxLayout* heading) {
    auto* group = new QButtonGroup(pane);
    const auto segment = [&](const char* name, const QString& text, const char* icon) {
        auto* button = new QToolButton(pane);
        button->setObjectName(QString::fromLatin1(name));
        button->setText(text);
        button->setIcon(UiKit::SvgIcon(QString::fromLatin1(icon)));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button);
        heading->insertWidget(1, button);
        return button;
    };
    // insertWidget(1) 依次插在标题之后：先插蓝图，再插传统，显示顺序为 传统 | 蓝图。
    blueprintButton_ = segment("triggerViewBlueprint", tr("蓝图"), "lucide/git-branch");
    traditionalButton_ = segment("triggerViewTraditional", tr("传统"), "lucide/list-tree");
    traditionalButton_->setChecked(true);
    connect(traditionalButton_, &QToolButton::clicked, this, [this]() { SetBlueprintVisible(false); });
    connect(blueprintButton_, &QToolButton::clicked, this, [this]() { SetBlueprintVisible(true); });

    blueprint_ = new TriggerBlueprint(pane);
    blueprint_->SetIconResolver([this](std::string_view path) {
        return project_.iconProvider_->IconForTexture(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
    });
    blueprint_->hide();
    layout->addWidget(blueprint_, 1);
    TriggerBlueprint::Callbacks callbacks;
    callbacks.selected = [this](StableId ecaId) {
        const QModelIndex index = ecaModel_->IndexForStableId(ecaId);
        if (!index.isValid()) return;
        syncingSelection_ = true;
        ecaView_->setCurrentIndex(index);
        syncingSelection_ = false;
    };
    callbacks.argument = [this](StableId ecaId, std::size_t position, const QRect& anchor) {
        OpenValuePanel(ecaId, position, anchor);
    };
    callbacks.editScalar = [this](StableId ecaId, std::size_t position, const QRect& anchor) {
        return EditScalarArgument(ecaId, position, anchor);
    };
    callbacks.activated = [this](StableId) { EditFirstArgument(); };
    callbacks.contextMenu = [this](StableId ecaId, const std::vector<TriggerBlueprint::PinTarget>& targets, const QPoint& globalPos) {
        ShowBlueprintMenu(ecaId, targets, globalPos);
    };
    callbacks.showConditions = [this]() { SetBlueprintVisible(false); };
    callbacks.connect = [this](StableId ecaId, const TriggerBlueprint::PinTarget& target) {
        ConnectBlueprint(ecaId, target.slot, target.before);
    };
    callbacks.create = [this](const TriggerBlueprint::PinTarget& target) { ChooseAndInsertEca(target.slot, target.before); };
    blueprint_->SetCallbacks(std::move(callbacks));
}

bool TriggerModule::BlueprintVisible() const {
    return blueprint_ != nullptr && traditionalButton_ != nullptr && !traditionalButton_->isChecked();
}

void TriggerModule::SetBlueprintVisible(bool visible) {
    if (!page_) {
        return;
    }
    if (showingGlobalCode_ || (CurrentTrigger() && CurrentTrigger()->customTextFlag != 0)) return;
    if (!CommitPendingEdits()) return;
    traditionalButton_->setChecked(!visible);
    blueprintButton_->setChecked(visible);
    LeaveMapScript();
    const auto* trigger = CurrentTrigger();
    // 自定义脚本与注释条目都没有语句：两种视图都不显示，只切换按钮状态。
    const bool noLogic = trigger != nullptr && (trigger->customTextFlag != 0 || trigger->type != 0);
    ecaView_->setVisible(!noLogic && !visible);
    blueprint_->setVisible(!noLogic && visible);
    SyncBlueprint();
    if (!noLogic) (visible ? static_cast<QWidget*>(blueprint_) : static_cast<QWidget*>(ecaView_))->setFocus();
}

void TriggerModule::SyncBlueprint() {
    if (!BlueprintVisible() || blueprint_->isHidden()) {
        return;
    }
    Trigger::PresentationOptions options;
    options.referenceResolver = referenceResolver_;
    options.showRawValues = showRawValues_ && showRawValues_->isChecked();
    options.typeSystem = ecaModel_->TypeSystem();
    blueprint_->SetSource(project_.triggerDocument_.get(), project_.uiCatalog_.get(), std::move(options));
    const auto* trigger = CurrentTrigger();
    blueprint_->ShowTrigger(trigger == nullptr ? 0 : trigger->id);
    if (const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex())) {
        blueprint_->Select(eca->id);
    }
}

void TriggerModule::ConnectBlueprint(StableId ecaId, const EcaSlot& slot, StableId beforeEcaId) {
    auto* document = project_.triggerDocument_.get();
    const auto* eca = document == nullptr ? nullptr : Trigger::TriggerDocumentEditor::FindEca(*document, ecaId);
    if (eca == nullptr) {
        return;
    }
    if (eca->type != slot.type) {
        ShowStatus(tr("“%1”只接受%2，不能接入%3。").arg(slot.label, KindText(slot.type), KindText(eca->type)), 6000);
        return;
    }
    if (beforeEcaId == ecaId) {
        return; // 已经在这里
    }
    ApplyEcaMove({ecaId, slot, beforeEcaId});
}

// 蓝图右键菜单：先列出这里可以添加语句的位置（在之后 / 各分支），再接语句本身的命令。
// 不依赖语句树的索引，块语句（含空分支）、触发条件节点与空白处都有菜单。
void TriggerModule::ShowBlueprintMenu(
    StableId ecaId, const std::vector<TriggerBlueprint::PinTarget>& targets, const QPoint& globalPos) {
    if (project_.triggerDocument_ == nullptr || project_.triggerUndoStack_ == nullptr || CurrentTrigger() == nullptr) {
        return;
    }
    QMenu menu(page_);
    menu.setObjectName("blueprintMenu");
    std::vector<std::pair<QAction*, std::function<void()>>> actions;
    for (const auto& target : targets) {
        const QString text = target.label.isEmpty()
            ? tr("在之后添加%1…").arg(KindText(target.slot.type))
            : tr("添加%1到“%2”…").arg(KindText(target.slot.type), target.label);
        actions.emplace_back(menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/plus")), text),
            [this, target]() { ChooseAndInsertEca(target.slot, target.before); });
    }
    if (ecaId != 0) {
        if (const QModelIndex index = ecaModel_->IndexForStableId(ecaId); index.isValid()) {
            syncingSelection_ = true;
            ecaView_->setCurrentIndex(index); // 复制 / 删除等命令作用于当前语句
            syncingSelection_ = false;
        }
        menu.addSeparator();
        AddEcaCommands(menu, ecaId, actions);
    }
    lastBlueprintMenu_.clear();
    for (const auto& [action, run] : actions) {
        if (!action->isSeparator()) lastBlueprintMenu_ << action->text();
    }
    if (actions.empty()) {
        return;
    }
    QAction* chosen = menu.exec(globalPos);
    for (auto& [action, run] : actions) {
        if (action == chosen) {
            run();
            break;
        }
    }
}

} // namespace War3Map::Modules

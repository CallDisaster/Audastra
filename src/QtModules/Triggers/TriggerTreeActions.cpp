#include "TriggerModule.h"
#include "UiLibraryDialog.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "TriggerModels.h"
#include "Modules/ModuleHost.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"

#include <QAction>
#include <QCheckBox>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>

// 触发器目录的视图部分：右键菜单、键盘、刷新（保留折叠 / 当前项 / 滚动）与信息条；以及函数库入口。
namespace War3Map::Modules {
namespace {

QIcon Icon(const char* name) {
    return UiKit::SvgIcon(QString::fromLatin1(name));
}

} // namespace

void TriggerModule::AddNewItemActions(QMenu& menu) {
    const auto add = [&](const char* name, const char* icon, const QString& text, auto callback) {
        auto* action = menu.addAction(Icon(icon), text);
        action->setObjectName(QString::fromLatin1(name));
        connect(action, &QAction::triggered, this, callback);
    };
    add("triggerNewGuiAction", "lucide/plus", tr("新建触发器"), [this] { NewTrigger(); });
    add("triggerNewCodeAction", "lucide/file-code-2", tr("新建自定义代码"), [this] { NewCustomTrigger(); });
    add("triggerNewCommentAction", "lucide/pencil", tr("新建注释"), [this] { NewComment(); });
    add("triggerNewCategoryAction", "lucide/folder-plus", tr("新建类别"), [this] { NewCategory(); });
}

void TriggerModule::ShowTreeContextMenu(const QPoint& pos) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const QModelIndex proxyIndex = explorerView_->indexAt(pos);
    if (proxyIndex.isValid()) {
        explorerView_->setCurrentIndex(proxyIndex); // 右键即选中（原型行为）
    }
    const auto item = proxyIndex.isValid() ? explorerModel_->ItemAt(filter_->mapToSource(proxyIndex)) : std::nullopt;
    QMenu menu(page_);
    menu.setObjectName("triggerDirectoryContextMenu");
    menu.setToolTipsVisible(true);
    const auto add = [&menu](const char* icon, const QString& text, const QKeySequence& key = {}) {
        QAction* action = menu.addAction(Icon(icon), text);
        action->setShortcut(key);
        action->setShortcutVisibleInContextMenu(true);
        return action;
    };
    const QString pasteText = triggerClipboard_
        ? tr("粘贴 %1").arg(QString::fromUtf8(triggerClipboard_->trigger.name.text)) : tr("粘贴");
    std::vector<std::pair<QAction*, std::function<void()>>> actions;
    const auto bind = [&actions](QAction* action, std::function<void()> run) { actions.emplace_back(action, std::move(run)); };
    AddNewItemActions(menu);
    menu.actions().front()->setShortcut(QKeySequence(Qt::Key_Insert));
    menu.actions().back()->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    for (auto* action : menu.actions()) action->setShortcutVisibleInContextMenu(true);
    menu.addSeparator();

    if (!item || item->category) {
        QAction* paste = add("lucide/copy", pasteText, QKeySequence::Paste);
        paste->setEnabled(triggerClipboard_.has_value() && item.has_value());
        bind(paste, [this]() { PasteTrigger(); });
        if (item) {
            menu.addSeparator();
            bind(add("lucide/pencil", tr("重命名"), QKeySequence(Qt::Key_F2)),
                [this, proxyIndex]() { explorerView_->edit(proxyIndex.siblingAtColumn(0)); });
            bind(add("lucide/trash-2", tr("删除类别"), QKeySequence::Delete), [this, item]() { DeleteDirectoryItem(*item); });
        }
    } else {
        const auto* trigger = explorerModel_->TriggerAt(filter_->mapToSource(proxyIndex));
        const StableId triggerId = trigger->id;
        const bool enabled = trigger->enabled != 0;
        const bool initiallyOn = trigger->initiallyClosed == 0;
        auto* convert = add("lucide/file-code-2", tr("转换为自定义代码…"));
        convert->setObjectName("triggerConvertCodeAction");
        convert->setEnabled(trigger->type == 0 && trigger->customTextFlag == 0 && ClassicCodeAllowed());
        convert->setToolTip(tr("转换为 JASS / vJass；确认后可撤销。"));
        bind(convert, [this] { (void)ConvertCurrentTriggerToCustomCode(); });
        bind(add("lucide/copy", tr("创建副本"), QKeySequence(Qt::CTRL | Qt::Key_D)), [this]() { DuplicateCurrentTrigger(); });
        bind(add("lucide/copy", tr("复制"), QKeySequence::Copy), [this]() { CopyCurrentTrigger(); });
        QAction* paste = add("lucide/copy", pasteText, QKeySequence::Paste);
        paste->setEnabled(triggerClipboard_.has_value());
        bind(paste, [this]() { PasteTrigger(); });
        bind(add("lucide/pencil", tr("重命名"), QKeySequence(Qt::Key_F2)),
            [this, proxyIndex]() { explorerView_->edit(proxyIndex.siblingAtColumn(0)); });
        menu.addSeparator();
        if (trigger->type == 0) { // 注释条目没有允许 / 初始打开
            QAction* allow = menu.addAction(tr("允许"));
            allow->setCheckable(true);
            allow->setChecked(enabled);
            allow->setToolTip(ScriptRebuildNotice());
            bind(allow, [this, triggerId, enabled, initiallyOn]() {
                SetTriggerFlags(triggerId, !enabled, initiallyOn, enabled ? tr("禁用触发器") : tr("允许触发器"));
            });
            QAction* open = menu.addAction(tr("初始打开"));
            open->setCheckable(true);
            open->setChecked(initiallyOn);
            open->setEnabled(enabled);
            open->setToolTip(ScriptRebuildNotice());
            bind(open, [this, triggerId, enabled, initiallyOn]() {
                SetTriggerFlags(triggerId, enabled, !initiallyOn, tr("触发器初始状态"));
            });
        }
        QMenu* moveTo = menu.addMenu(Icon("lucide/folder-open"), tr("移到类别"));
        for (const auto& category : document->categories) {
            if (category.categoryId == trigger->categoryId) continue;
            const std::int32_t target = category.categoryId;
            QAction* action = moveTo->addAction(QString::fromUtf8(category.name.text));
            bind(action, [this, target, item]() { ApplyDirectoryMove({*item, target, 0, std::nullopt}); });
        }
        moveTo->setEnabled(!moveTo->isEmpty());
        menu.addSeparator();
        QAction* references = add("lucide/search", tr("查看引用（物体管理）"));
        references->setEnabled(host_ != nullptr);
        bind(references, [this, triggerId]() { host_->Navigate({"manager", "trigger", QString::number(triggerId), {}}); });
        bind(add("lucide/trash-2", tr("删除触发器"), QKeySequence::Delete), [this, item]() { DeleteDirectoryItem(*item); });
    }
    QAction* chosen = menu.exec(explorerView_->viewport()->mapToGlobal(pos));
    for (auto& [action, run] : actions) {
        if (action == chosen) {
            run();
            break;
        }
    }
}

bool TriggerModule::HandleExplorerKey(QEvent* event) {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) {
        return false;
    }
    // 就地重命名时焦点在编辑框里，按键不会经过视图，这里无需额外判断。
    auto* key = static_cast<QKeyEvent*>(event);
    const bool ctrl = key->modifiers() == Qt::ControlModifier;
    const bool plain = key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier;
    std::function<void()> run;
    if (plain && key->key() == Qt::Key_Delete) run = [this]() { DeleteCurrentItem(); };
    else if ((plain && key->key() == Qt::Key_Insert) || (ctrl && key->key() == Qt::Key_T)) run = [this]() { NewTrigger(); };
    else if (ctrl && key->key() == Qt::Key_G) run = [this]() { NewCategory(); };
    else if (ctrl && key->key() == Qt::Key_C) run = [this]() { CopyCurrentTrigger(); };
    else if (ctrl && key->key() == Qt::Key_V) run = [this]() { PasteTrigger(); };
    else if (ctrl && key->key() == Qt::Key_D) run = [this]() { DuplicateCurrentTrigger(); };
    else if (plain && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
        run = [this]() {
            const QModelIndex current = explorerView_->currentIndex();
            if (explorerModel_->CategoryIdAt(filter_->mapToSource(current))) {
                explorerView_->setExpanded(current.siblingAtColumn(0), !explorerView_->isExpanded(current.siblingAtColumn(0)));
            } else if (ecaView_->isVisible()) {
                ecaView_->setFocus(); // 触发器：进入逻辑区
            }
        };
    }
    if (!run) {
        return false;
    }
    // ShortcutOverride：声明目录自己处理这个键，窗口级快捷键（例如逻辑区的 Ctrl+D）不抢。
    event->accept();
    if (event->type() == QEvent::KeyPress) {
        run();
    }
    return true;
}

void TriggerModule::RefreshExplorer(StableId selectTriggerId) {
    if (!page_ || project_.triggerDocument_ == nullptr) {
        return;
    }
    if (selectTriggerId != 0) {
        selectedTrigger_ = selectTriggerId;
        selectedCategory_.reset();
    }
    refreshingExplorer_ = true;
    const int scroll = explorerView_->verticalScrollBar()->value();
    // 结构未变时模型只发数据变化，视图展开状态原样保留，不必重新展开。
    if (explorerModel_->RefreshDocument(project_.triggerDocument_.get())) ApplyExplorerExpansion();
    QModelIndex source;
    if (selectedTrigger_ != 0) {
        source = explorerModel_->IndexForTriggerId(selectedTrigger_);
    } else if (selectedCategory_) {
        source = explorerModel_->IndexForCategoryId(*selectedCategory_);
    }
    if (!source.isValid()) {
        // 已被删除（例如撤销了新建）：退回到它所在的类别。
        selectedTrigger_ = 0;
        selectedCategory_.reset();
        if (selectionCategory_) {
            source = explorerModel_->IndexForCategoryId(*selectionCategory_);
            if (source.isValid()) selectedCategory_ = selectionCategory_;
        }
    }
    const QModelIndex proxy = filter_->mapFromSource(source);
    if (proxy.isValid()) {
        explorerView_->setCurrentIndex(proxy);
    }
    explorerView_->verticalScrollBar()->setValue(scroll);
    if (proxy.isValid() && selectTriggerId != 0) {
        explorerView_->scrollTo(proxy);
    }
    refreshingExplorer_ = false;
    UpdateSummary();
    if (displayedTrigger_ != selectedTrigger_) {
        ShowSelectedTrigger();
    }
}

void TriggerModule::ApplyExplorerExpansion() {
    if (!page_) {
        return;
    }
    const bool previous = refreshingExplorer_;
    refreshingExplorer_ = true;
    explorerView_->expandAll();
    if (explorerSearch_->text().isEmpty()) {
        for (int row = 0; row < filter_->rowCount(); ++row) {
            const QModelIndex index = filter_->index(row, 0);
            const auto categoryId = explorerModel_->CategoryIdAt(filter_->mapToSource(index));
            if (categoryId && collapsedCategories_.contains(*categoryId)) {
                explorerView_->collapse(index);
            }
        }
    }
    refreshingExplorer_ = previous;
    UpdateCategoryIcons();
}

void TriggerModule::UpdateCategoryIcons() {
    if (!page_ || explorerModel_ == nullptr) {
        return;
    }
    std::set<std::int32_t> expanded;
    for (int row = 0; row < filter_->rowCount(); ++row) {
        const QModelIndex index = filter_->index(row, 0);
        const auto id = explorerModel_->CategoryIdAt(filter_->mapToSource(index));
        if (id && explorerView_->isExpanded(index)) expanded.insert(*id);
    }
    explorerModel_->SetExpandedCategories(std::move(expanded));
}

QString TriggerModule::ScriptRebuildNotice() {
    if (project_.luaMapTarget_ && project_.luaMapTargetSource_ == project_.sourceArchivePath_) {
        scriptCapabilityCached_ = false;
        scriptCapabilityDetails_ = tr("当前地图已选择Lua保存档案；实际生成和检查仍在保存时执行。");
        return {};
    }
    EnsureScriptCapability(project_.compilerDataRoot_.empty() ? project_.activeUiDataRoot_ : project_.compilerDataRoot_);
    return scriptCapabilityNotice_;
}

const std::vector<std::filesystem::path>& TriggerModule::ScriptSystemIncludes(const std::filesystem::path& toolRoot) {
    EnsureScriptCapability(toolRoot);
    return scriptCapabilityIncludes_;
}

void TriggerModule::EnsureScriptCapability(const std::filesystem::path& toolRoot) {
    if (!scriptCapabilityCached_ || scriptCapabilityToolRoot_ != toolRoot || scriptCapabilityUiRoot_ != project_.activeUiDataRoot_) {
        const auto capability = Services::QueryScriptRebuildCapability(toolRoot, project_.activeUiDataRoot_);
        scriptCapabilityToolRoot_ = toolRoot; scriptCapabilityUiRoot_ = project_.activeUiDataRoot_;
        scriptCapabilityCached_ = true;
        scriptCapabilityIncludes_ = capability.layout ? capability.layout->systemIncludes
            : std::vector<std::filesystem::path>{toolRoot / "compiler" / "include"};
        switch (capability.state) {
        case Services::ScriptRebuildCapabilityState::Ready: scriptCapabilityNotice_.clear(); break;
        case Services::ScriptRebuildCapabilityState::Unsupported:
            scriptCapabilityNotice_ = tr("旧版图暂时无法保存触发器改动：旧版 YDWE 工具链尚未支持。"); break;
        case Services::ScriptRebuildCapabilityState::Missing:
            scriptCapabilityNotice_ = tr("工具链未就绪，触发器改动暂时无法保存。"); break;
        case Services::ScriptRebuildCapabilityState::Unknown:
            scriptCapabilityNotice_ = tr("工具链来源无法确认，触发器改动暂时无法保存。"); break;
        }
        QStringList details;
        for (const auto& diagnostic : capability.diagnostics)
            details << QString::fromStdString(diagnostic.code) + QStringLiteral(" · ") + QString::fromUtf8(diagnostic.message);
        scriptCapabilityDetails_ = details.join(QLatin1Char('\n'));
    }
}

void TriggerModule::UpdateMetaBar(const Trigger::TriggerRecord* trigger) {
    const bool has = trigger != nullptr;
    // 注释条目：隐藏允许 / 初始打开等开关，注释编辑框占满逻辑区（原版“触发器注释”面板）。
    const bool commentEntry = has && trigger->type != 0;
    const auto saveNotice = has && !commentEntry ? ScriptRebuildNotice() : QString{};
    // 不能保存触发器改动是警告：写入错误列表，不在属性区放说明文字。原因消失后再出现时重新写入。
    if (saveNotice.isEmpty()) reportedScriptNotice_.clear();
    if (const auto key = saveNotice + QLatin1Char('|') + QString::fromStdWString(project_.sourceArchivePath_.wstring());
        !saveNotice.isEmpty() && key != reportedScriptNotice_) {
        reportedScriptNotice_ = key;
        project_.AddDiagnostic(tr("警告"), tr("触发器"), QStringLiteral("SCRIPT_REBUILD_UNAVAILABLE"),
            scriptCapabilityDetails_.isEmpty() ? saveNotice : saveNotice + QLatin1Char('\n') + scriptCapabilityDetails_);
    }
    const auto hint = [&saveNotice](const QString& text) { return saveNotice.isEmpty() ? text : text + QLatin1Char('\n') + saveNotice; };
    enabledCheck_->setToolTip(hint(tr("取消后触发器不生成代码，也不会被创建")));
    initiallyOnCheck_->setToolTip(hint(tr("取消后触发器在地图初始化时被关闭，需要用“开启触发器”动作打开")));
    runOnInitCheck_->setToolTip(hint(tr("在地图初始化阶段自动运行该触发器")));
    for (QWidget* widget : {static_cast<QWidget*>(enabledCheck_), static_cast<QWidget*>(initiallyOnCheck_),
             static_cast<QWidget*>(runOnInitCheck_), static_cast<QWidget*>(metaNote_), static_cast<QWidget*>(commentToggle_)}) {
        widget->setVisible(!commentEntry);
    }
    commentEdit_->setVisible(commentEntry || commentToggle_->isChecked());
    if (commentEntry) {
        commentEdit_->setMinimumHeight(56);
        commentEdit_->setMaximumHeight(QWIDGETSIZE_MAX);
        commentEdit_->setPlaceholderText(tr("注释"));
    } else {
        commentEdit_->setFixedHeight(72);
        commentEdit_->setPlaceholderText(tr("触发器注释"));
    }
    if (auto* box = qobject_cast<QBoxLayout*>(propertiesBox_->parentWidget()->layout())) {
        box->setStretchFactor(propertiesBox_, commentEntry ? 1 : 0);
    }
    logicTitle_->setText(commentEntry ? tr("注释") : has ? QString::fromUtf8(trigger->name.text) : tr("触发器逻辑"));
    const bool off = has && !commentEntry && trigger->enabled == 0;
    initiallyOnCheck_->setEnabled(has && !off);
    metaNote_->setText(!has ? QString{}
        : off ? tr("已禁用")
        : trigger->initiallyClosed != 0 ? tr("初始关闭") : QString{});
    propertiesBox_->setProperty("off", off);
    const QString comment = has ? QString::fromUtf8(trigger->description.text) : QString{};
    const QString firstLine = comment.section(QRegularExpression(QStringLiteral("\\r?\\n")), 0, 0);
    commentToggle_->setText(comment.isEmpty() ? tr("添加触发器注释…")
        : tr("触发器注释：%1").arg(commentToggle_->fontMetrics().elidedText(firstLine, Qt::ElideRight, 360)));
    commentToggle_->setIcon(Icon(commentToggle_->isChecked() ? "lucide/chevron-down" : "lucide/chevron-right"));
}

void TriggerModule::OpenUiLibraries() {
    if (!CommitPendingEdits()) return;
    if (project_.uiLibraryPackages_.empty()) {
        ShowStatus(tr("当前数据根没有可管理的函数库包；请先选择 UI 数据根并打开地图。"), 6000);
        return;
    }
    UiLibraryDialog dialog(project_, page_);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (project_.triggerDirty_ && QMessageBox::question(page_, tr("函数库"),
            tr("重新载入触发器会放弃尚未保存的触发器修改。继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!project_.ApplyUiLibrarySettings(dialog.Settings(), dialog.MapOnly())) {
        ShowStatus(tr("按新的函数库设置重新载入触发器失败，详见诊断。"), 8000);
    }
}

} // namespace War3Map::Modules

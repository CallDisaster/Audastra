#include "TriggerModule.h"

#include "EditorProject.h"
#include "TriggerBlueprint.h"
#include "TriggerActionHelpers.h"
#include "TriggerModels.h"

#include "War3MapCore/Trigger/TriggerSelection.h"

#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>

#include <algorithm>
#include <set>
#include <unordered_set>

// 多选（Windows 式）：单击选一项，Ctrl+单击增减，Shift+单击从锚点选一段，Ctrl+Shift+单击追加一段，
// Shift+方向键扩展，Ctrl+A 全选。当前项（焦点）仍驱动属性区、重命名与参数编辑；
// 选区按稳定 ID 记住，模型刷新与撤销后按 ID 恢复。批量命令本身在 TriggerDirectoryBatch.cpp / TriggerEcaBatch.cpp。
namespace War3Map::Modules {
namespace {

using TriggerActions::FindTrigger;

// 只按 Shift（不带 Ctrl）的导航键：从锚点重新选一段，之前 Ctrl 选的项不保留。
bool IsShiftNavigation(const QKeyEvent* key) {
    if ((key->modifiers() & ~Qt::KeypadModifier) != Qt::ShiftModifier) return false;
    switch (key->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        return true;
    default:
        return false;
    }
}

// 依次访问模型的全部行（第 0 列，含子行）。
template <typename Visit>
void ForEachRow(const QAbstractItemModel& model, const QModelIndex& parent, const Visit& visit) {
    const int rows = model.rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        visit(index);
        if (model.hasChildren(index)) ForEachRow(model, index, visit);
    }
}

// 按选区与当前项设置视图，结果与此刻按住的修饰键无关；当前项同时成为 Shift 锚点。
void ApplySelection(QAbstractItemView& view, const QItemSelection& wanted, const QModelIndex& focus, bool onlyFocus) {
    if (onlyFocus) {
        if (focus.isValid()) TriggerActions::SelectOnly(view, focus);
        return;
    }
    if (focus.isValid()) view.setCurrentIndex(focus);
    view.selectionModel()->select(wanted, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

} // namespace

void TriggerModule::ConfigureMultiSelection() {
    for (QTreeView* view : {explorerView_, ecaView_}) {
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->viewport()->installEventFilter(this);
    }
    ecaView_->installEventFilter(this);
    connect(explorerView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!refreshingExplorer_ && !restoringExplorerSelection_) CaptureExplorerSelection();
    });
    connect(ecaView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (!restoringEcaSelection_) CaptureEcaSelection();
    });
}

void TriggerModule::OnExplorerCurrentChanged(const QModelIndex& current) {
    if (refreshingExplorer_ || restoringExplorerSelection_) {
        return;
    }
    const auto item = explorerModel_->ItemAt(filter_->mapToSource(current));
    const auto indexOfSelected = [this] {
        const auto source = selectedTrigger_ ? explorerModel_->IndexForTriggerId(selectedTrigger_)
            : selectedCategory_ ? explorerModel_->IndexForCategoryId(*selectedCategory_) : QModelIndex{};
        return filter_->mapFromSource(source);
    };
    if (!CommitPendingEdits()) {
        // 草稿未能提交：留在原来的项上。鼠标按下之后视图还会按点击位置改选区，事件结束后再恢复一次。
        explorerSelection_ = {};
        RestoreExplorerSelection(indexOfSelected());
        const auto trigger = selectedTrigger_;
        const auto category = selectedCategory_;
        QTimer::singleShot(0, this, [this, trigger, category, indexOfSelected] {
            if (!page_ || selectedTrigger_ != trigger || selectedCategory_ != category) return;
            explorerSelection_ = {};
            RestoreExplorerSelection(indexOfSelected());
        });
        return;
    }
    selectedTrigger_ = item && !item->category ? item->triggerId : 0;
    selectedCategory_ = item && item->category ? std::optional<std::int32_t>(item->categoryId) : std::nullopt;
    if (item) selectionCategory_ = item->categoryId;
    {
        // 提交可能重置目录模型；按已捕获的稳定身份恢复这次导航的目标，选区由视图（点击 / 方向键）决定。
        const QScopedValueRollback guard(restoringExplorerSelection_, true);
        const QModelIndex target = indexOfSelected();
        auto* selection = explorerView_->selectionModel();
        if (target.isValid() && target != explorerView_->currentIndex().siblingAtColumn(0)) {
            QItemSelectionModel::SelectionFlags command = QItemSelectionModel::NoUpdate;
            if (!selection->hasSelection()) command = QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
            selection->setCurrentIndex(target, command);
        }
    }
    ShowSelectedTrigger();
}

void TriggerModule::CaptureExplorerSelection() {
    Trigger::DirectorySelection selection;
    for (const QModelIndex& index : explorerView_->selectionModel()->selectedRows(0)) {
        if (const auto item = explorerModel_->ItemAt(filter_->mapToSource(index))) {
            if (item->category) selection.categories.push_back(item->categoryId);
            else selection.triggers.push_back(item->triggerId);
        }
    }
    explorerSelection_ = std::move(selection);
}

void TriggerModule::CaptureEcaSelection() {
    std::vector<StableId> ids;
    for (const QModelIndex& index : ecaView_->selectionModel()->selectedRows(0)) {
        if (const auto* eca = ecaModel_->EcaAt(index)) ids.push_back(eca->id); // 分组头 / 分支头不算
    }
    ecaSelection_ = std::move(ids);
}

void TriggerModule::RestoreExplorerSelection(const QModelIndex& current) {
    const QModelIndex focus = current.isValid() ? current.siblingAtColumn(0) : QModelIndex{};
    const auto focusItem = focus.isValid() ? explorerModel_->ItemAt(filter_->mapToSource(focus)) : std::nullopt;
    const auto isFocus = [&focusItem](bool category, std::int32_t categoryId, StableId triggerId) {
        return focusItem && focusItem->category == category
            && (category ? focusItem->categoryId == categoryId : focusItem->triggerId == triggerId);
    };
    const std::size_t remembered = explorerSelection_.categories.size() + explorerSelection_.triggers.size();
    bool onlyFocus = remembered == 0
        || (remembered == 1 && (explorerSelection_.categories.empty()
            ? isFocus(false, 0, explorerSelection_.triggers.front())
            : isFocus(true, explorerSelection_.categories.front(), 0)));
    QItemSelection wanted;
    if (!onlyFocus) {
        const std::set<std::int32_t> categories(explorerSelection_.categories.begin(), explorerSelection_.categories.end());
        const std::set<StableId> triggers(explorerSelection_.triggers.begin(), explorerSelection_.triggers.end());
        ForEachRow(*filter_, {}, [&](const QModelIndex& index) {
            const auto item = explorerModel_->ItemAt(filter_->mapToSource(index));
            if (item && (item->category ? categories.contains(item->categoryId) : triggers.contains(item->triggerId))) {
                wanted.select(index, index);
            }
        });
        // 记住的项都已不在（删除、过滤）：退回到只选当前项。
        onlyFocus = wanted.isEmpty() || (wanted.size() == 1 && focus.isValid() && wanted.contains(focus));
    }
    {
        const QScopedValueRollback guard(restoringExplorerSelection_, true);
        ApplySelection(*explorerView_, wanted, focus, onlyFocus);
    }
    CaptureExplorerSelection();
}

void TriggerModule::RestoreEcaSelection(const QModelIndex& current) {
    const QModelIndex focus = current.isValid() ? current.siblingAtColumn(0) : QModelIndex{};
    const auto* focusEca = focus.isValid() ? ecaModel_->EcaAt(focus) : nullptr;
    bool onlyFocus = ecaSelection_.empty()
        || (ecaSelection_.size() == 1 && focusEca != nullptr && ecaSelection_.front() == focusEca->id);
    QItemSelection wanted;
    if (!onlyFocus) {
        const std::unordered_set<StableId> ids(ecaSelection_.begin(), ecaSelection_.end());
        ForEachRow(*ecaModel_, {}, [&](const QModelIndex& index) {
            if (const auto* eca = ecaModel_->EcaAt(index); eca != nullptr && ids.contains(eca->id)) {
                wanted.select(index, index);
            }
        });
        onlyFocus = wanted.isEmpty() || (wanted.size() == 1 && focus.isValid() && wanted.contains(focus));
    }
    {
        const QScopedValueRollback guard(restoringEcaSelection_, true);
        ApplySelection(*ecaView_, wanted, focus, onlyFocus);
    }
    CaptureEcaSelection();
}

Trigger::DirectorySelection TriggerModule::NormalizedDirectorySelection() const {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return {};
    }
    auto selection = Trigger::NormalizeDirectorySelection(*document, explorerSelection_.categories, explorerSelection_.triggers);
    if (selection.categories.empty() && selection.triggers.empty()) {
        // 没有选区（例如刚换了文档）：按当前项。
        if (const auto current = CurrentDirectoryItem()) {
            if (current->category) selection.categories.push_back(current->categoryId);
            else selection.triggers.push_back(current->triggerId);
        }
    }
    return selection;
}

std::vector<TriggerDirectoryItem> TriggerModule::SelectedDirectoryItems() const {
    std::vector<TriggerDirectoryItem> items;
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return items;
    }
    const auto selection = NormalizedDirectorySelection();
    for (const std::int32_t categoryId : selection.categories) {
        items.push_back({true, 0, categoryId});
    }
    for (const StableId triggerId : selection.triggers) {
        if (const auto* trigger = FindTrigger(*document, triggerId)) items.push_back({false, triggerId, trigger->categoryId});
    }
    return items;
}

std::vector<StableId> TriggerModule::SelectedTriggerIds() const {
    const auto* document = project_.triggerDocument_.get();
    return document == nullptr ? std::vector<StableId>{}
                               : Trigger::DirectorySelectionTriggers(*document, NormalizedDirectorySelection());
}

void TriggerModule::SelectDirectoryItems(const std::vector<TriggerDirectoryItem>& items) {
    if (!page_ || items.empty()) {
        return;
    }
    if (!CommitPendingEdits()) return;
    explorerSelection_ = {};
    for (const auto& item : items) {
        if (item.category) {
            explorerSelection_.categories.push_back(item.categoryId);
        } else {
            explorerSelection_.triggers.push_back(item.triggerId);
            collapsedCategories_.erase(item.categoryId);
        }
    }
    const auto& focus = items.front();
    selectionCategory_ = focus.categoryId;
    selectedTrigger_ = focus.category ? 0 : focus.triggerId;
    selectedCategory_ = focus.category ? std::optional<std::int32_t>(focus.categoryId) : std::nullopt;
    RefreshExplorer();
    const QModelIndex source = focus.category ? explorerModel_->IndexForCategoryId(focus.categoryId)
                                              : explorerModel_->IndexForTriggerId(focus.triggerId);
    if (const QModelIndex proxy = filter_->mapFromSource(source); proxy.isValid()) {
        explorerView_->scrollTo(proxy);
    }
}

std::vector<StableId> TriggerModule::SelectedEcaIds() const {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr || displayedTrigger_ == 0 || !page_) {
        return {};
    }
    if (BlueprintVisible()) return Trigger::NormalizeEcaSelection(*document, displayedTrigger_, blueprint_->SelectedEcas()).ids;
    std::vector<StableId> ids = ecaSelection_;
    if (ids.empty()) {
        if (const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex())) ids.push_back(eca->id);
    }
    return Trigger::NormalizeEcaSelection(*document, displayedTrigger_, ids).ids;
}

std::vector<StableId> TriggerModule::SelectedEcaRowIds() const {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr || displayedTrigger_ == 0 || !page_) {
        return {};
    }
    if (BlueprintVisible()) return blueprint_->SelectedEcas();
    std::vector<StableId> ids = ecaSelection_;
    if (ids.empty()) {
        if (const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex())) ids.push_back(eca->id);
    }
    std::erase_if(ids, [&](StableId id) { return !Trigger::EcaSlotOf(*document, displayedTrigger_, id).has_value(); });
    return ids;
}

void TriggerModule::SelectEcas(const std::vector<StableId>& ecaIds) {
    if (!page_ || ecaIds.empty()) {
        return;
    }
    ecaSelection_ = ecaIds;
    RestoreEcaSelection(ecaModel_->IndexForStableId(ecaIds.front()));
    if (BlueprintVisible()) blueprint_->SetSelection(ecaSelection_, ecaIds.front(), true);
}

bool TriggerModule::HandleSelectionInput(QObject* watched, QEvent* event) {
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::KeyPress && type != QEvent::ShortcutOverride) {
        return false;
    }
    if (!page_ || explorerView_ == nullptr || ecaView_ == nullptr) {
        return false;
    }
    QTreeView* view = nullptr;
    if (watched == explorerView_ || watched == explorerView_->viewport()) view = explorerView_;
    else if (watched == ecaView_ || watched == ecaView_->viewport()) view = ecaView_;
    if (view == nullptr) {
        return false;
    }
    if (type == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto modifiers = mouse->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
        if (watched == view->viewport() && mouse->button() == Qt::LeftButton
            && view->indexAt(mouse->position().toPoint()).isValid()) {
            if (modifiers == Qt::ShiftModifier) {
                // 只按 Shift 单击：Windows 语义是从锚点重新选一段（Qt 默认会保留之前 Ctrl 选的项）。
                view->selectionModel()->clearSelection();
            } else if (modifiers == (Qt::ShiftModifier | Qt::ControlModifier)) {
                // Ctrl+Shift 单击：在现有选区上追加一段（Qt 默认会替换上一次 Shift 选的那段），先把它并入选区。
                view->selectionModel()->select(QItemSelection(), QItemSelectionModel::Select);
            }
        }
        return false;
    }
    if (watched != view) {
        return false;
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->matches(QKeySequence::SelectAll)) {
        // 声明视图自己处理 Ctrl+A，窗口级快捷键不抢。
        event->accept();
        if (type == QEvent::KeyPress) view->selectAll();
        return true;
    }
    if (type == QEvent::KeyPress && IsShiftNavigation(key)) {
        view->selectionModel()->clearSelection();
    }
    return false;
}

} // namespace War3Map::Modules

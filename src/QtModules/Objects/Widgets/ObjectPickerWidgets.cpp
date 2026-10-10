#include "Widgets/ObjectCatalogWidgets.h"
#include "Widgets/ObjectCatalogDetail.h"
#include "Widgets/RoundedPopups.h"

namespace War3Map::QtApp {
using namespace CatalogDetail;

// 非模态对象选择弹层与有序对象列表。

ObjectPickerPopup::ObjectPickerPopup(IconProvider* icons, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("objectPickerPopup"));
    setWindowModality(Qt::NonModal);
    setFrameShape(QFrame::NoFrame);
    setAccessibleName(QStringLiteral("对象选择器"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);
    auto* header = new QHBoxLayout;
    title_ = new QLabel(QStringLiteral("选择对象"), this);
    title_->setObjectName(QStringLiteral("objectPickerTitle"));
    title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(title_, 1);
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("objectPickerClose"));
    close->setAccessibleName(QStringLiteral("关闭对象选择器"));
    header->addWidget(close);
    layout->addLayout(header);
    catalog_ = new ObjectCatalogWidget(icons, this);
    catalog_->SetDirectoryVisible(false);
    // 弹层使用窄筛选栏；完整候选文字仍可在下拉菜单读取，不让三个最小宽度撑破高 DPI 屏幕。
    for (const auto* name : {"objectCatalogKinds", "objectCatalogRaces", "objectCatalogCategories"}) {
        if (auto* combo = catalog_->findChild<QComboBox*>(QString::fromLatin1(name))) {
            combo->setMinimumContentsLength(3);
            // 窄弹层不让候选内容的 minimumSizeHint 约束整个窗口；
            // 收缩时保留两个汉字和箭头的宽度，展开菜单仍显示完整候选。
            combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            combo->setMinimumWidth(72);
        }
    }
    if (auto* search = catalog_->findChild<QLineEdit*>(QStringLiteral("objectCatalogSearch"))) {
        search->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        search->setMinimumWidth(96);
    }
    if (auto* status = catalog_->findChild<QLabel*>(QStringLiteral("objectCatalogStatus"))) {
        status->setWordWrap(true);
        status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    layout->addWidget(catalog_, 1);
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    connect(catalog_, &ObjectCatalogWidget::EntryChosen, this, [this](const ObjectViewEntry& entry) {
        const ObjectViewEntry chosen = entry;
        hide();
        emit EntryChosen(chosen);
    });
    War3Map::UiKit::ConfigureRoundedPopup(*this);
    resize(490, 570);
}

void ObjectPickerPopup::SetEntries(const QVector<ObjectViewEntry>& entries) { catalog_->SetEntries(entries); }
void ObjectPickerPopup::SetKindFilter(const QString& kind) { catalog_->SetKindFilter(kind, !kind.isEmpty()); }
void ObjectPickerPopup::SetCurrentId(const QString& id) { catalog_->SetCurrentId(id); }
void ObjectPickerPopup::SetTitle(const QString& title) { title_->setText(title); }

void ObjectPickerPopup::OpenAt(QWidget* anchor) {
    if (!anchor) {
        return;
    }
    anchor_ = anchor;
    PositionAtAnchor();
    qApp->installEventFilter(this);
    show();
    // QSplitter 在子目录真正显示后才刷新隐藏栏/筛选行的尺寸约束。
    // 再按当前布局定位，避免首次 show 仍沿用显示前缓存的窗口最小宽度。
    PositionAtAnchor();
    raise();
    activateWindow();
    catalog_->FocusSearch();
}

void ObjectPickerPopup::PositionAtAnchor() {
    if (!anchor_) {
        return;
    }
    const QRect anchorRect(anchor_->mapToGlobal(QPoint{}), anchor_->size());
    QScreen* screen = QGuiApplication::screenAt(anchorRect.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) {
        return;
    }
    const QRect bounds = screen->availableGeometry().adjusted(8, 8, -8, -8);
    const int width = std::min(490, bounds.width());
    const int height = std::min(570, bounds.height());
    ensurePolished();
    layout()->activate();
    // 顶层窗口可能仍保留上次布局激活写入的 minimumSize；直接同步最新布局下限，
    // 不按屏幕裁低下限。若真实内容放不下，actualSize 和严格边界验收仍会明确失败。
    setMinimumSize(layout()->totalMinimumSize());
    resize(width, height);
    // 使用布局实际接受的尺寸定位；不能用请求尺寸掩盖 minimumSizeHint 撑大的窗口。
    const QSize actualSize = size();
    int x = anchorRect.right() + 10;
    if (x + actualSize.width() > bounds.right() + 1) {
        x = anchorRect.left() - actualSize.width() - 10;
    }
    x = std::clamp(x, bounds.left(), std::max(bounds.left(), bounds.right() - actualSize.width() + 1));
    const int y = std::clamp(anchorRect.top(), bounds.top(), std::max(bounds.top(), bounds.bottom() - actualSize.height() + 1));
    move(x, y);
}

bool ObjectPickerPopup::eventFilter(QObject* watched, QEvent* event) {
    if (!isVisible()) {
        return QFrame::eventFilter(watched, event);
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        auto* activePopup = QApplication::activePopupWidget();
        const bool ownNativePopup = activePopup && isAncestorOf(activePopup) &&
            activePopup->frameGeometry().contains(mouse->globalPosition().toPoint());
        // 下拉框是独立原生 popup；允许其接受选择，不把它误判为外点。
        const bool childPopup = ownNativePopup || (widget && (isAncestorOf(widget) || widget == this));
        if (!childPopup && !frameGeometry().contains(mouse->globalPosition().toPoint())) {
            hide();
            return false;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            if (anchor_) {
                anchor_->setFocus(Qt::PopupFocusReason);
            }
            return true;
        }
    }
    if (event->type() == QEvent::ApplicationDeactivate) {
        hide();
    }
    if (anchor_ && watched == anchor_->window() &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        PositionAtAnchor();
    }
    return QFrame::eventFilter(watched, event);
}

void ObjectPickerPopup::hideEvent(QHideEvent* event) {
    qApp->removeEventFilter(this);
    QFrame::hideEvent(event);
}

OrderedObjectListWidget::OrderedObjectListWidget(IconProvider* icons, QWidget* parent)
    : QWidget(parent), icons_(icons) {
    setObjectName(QStringLiteral("orderedObjectList"));
    setAccessibleName(QStringLiteral("已选对象列表"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* actions = new QHBoxLayout;
    add_ = new QPushButton(QStringLiteral("添加"), this);
    replace_ = new QPushButton(QStringLiteral("替换"), this);
    remove_ = new QPushButton(QStringLiteral("删除"), this);
    up_ = new QPushButton(QStringLiteral("上移"), this);
    down_ = new QPushButton(QStringLiteral("下移"), this);
    for (auto* button : {add_, replace_, remove_, up_, down_}) {
        actions->addWidget(button);
    }
    actions->addStretch();
    layout->addLayout(actions);
    content_ = new QSplitter(Qt::Horizontal, this);
    content_->setHandleWidth(1);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("orderedObjectSelection"));
    list_->setAccessibleName(QStringLiteral("已选对象及顺序"));
    list_->setIconSize(QSize(32, 32));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    content_->addWidget(list_);
    layout->addWidget(content_, 1);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    picker_ = new ObjectPickerPopup(icons, this);
    connect(add_, &QPushButton::clicked, this, [this] { OpenPicker(false); });
    connect(replace_, &QPushButton::clicked, this, [this] { OpenPicker(true); });
    connect(remove_, &QPushButton::clicked, this, &OrderedObjectListWidget::RemoveCurrent);
    connect(up_, &QPushButton::clicked, this, [this] { MoveCurrent(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { MoveCurrent(1); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { UpdateActions(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] {
        const int row = list_->currentRow();
        if (row >= 0 && row < ids_.size()) {
            if (const auto* entry = Resolve(ids_[row])) {
                emit EntryOpenRequested(*entry);
            }
        }
    });
    connect(picker_, &ObjectPickerPopup::EntryChosen, this, [this](const ObjectViewEntry& entry) {
        if (popupSessionActive_ && !inlineCatalogEnabled_) AcceptEntry(entry);
    });
    RebuildList();
}

void OrderedObjectListWidget::SetEntries(const QVector<ObjectViewEntry>& entries) {
    picker_->hide();
    popupSessionActive_ = false;
    entries_ = entries;
    picker_->SetEntries(entries);
    if (inlineCatalog_) inlineCatalog_->SetEntries(entries);
    RebuildList(list_->currentRow());
}

void OrderedObjectListWidget::SetKnownEntries(const QVector<ObjectViewEntry>& entries) {
    knownEntries_ = entries;
    RebuildList(list_->currentRow());
}

void OrderedObjectListWidget::SetKindFilter(const QString& kind) {
    kind_ = kind;
    picker_->hide();
    popupSessionActive_ = false;
    picker_->SetKindFilter(kind);
    picker_->SetTitle(QStringLiteral("选择%1").arg(kind.isEmpty() ? QStringLiteral("对象") : KindName(kind)));
    if (inlineCatalog_) inlineCatalog_->SetKindFilter(kind, !kind.isEmpty());
    RebuildList(list_->currentRow());
}

void OrderedObjectListWidget::SetIds(const QStringList& ids) {
    // 同值也可能代表切换到另一文档；关闭旧选择会话，避免跨对象误写。
    picker_->hide();
    popupSessionActive_ = false;
    ids_ = ids;
    replacementRow_ = -1;
    pickerBaseline_ = ids_;
    if (inlineCatalog_) {
        inlineCatalog_->SetSearchText({});
        inlineCatalog_->SetCurrentId({});
    }
    RebuildList(list_->currentRow());
}

void OrderedObjectListWidget::SetReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    if (readOnly_) {
        picker_->hide();
        popupSessionActive_ = false;
    }
    if (inlineCatalog_) inlineCatalog_->setEnabled(!readOnly_);
    UpdateActions();
}

void OrderedObjectListWidget::SetInlineCatalogEnabled(bool enabled) {
    if (inlineCatalogEnabled_ == enabled) return;
    inlineCatalogEnabled_ = enabled;
    picker_->hide();
    popupSessionActive_ = false;
    if (enabled && !inlineCatalog_) {
        auto* pane = new QWidget;
        pane->setObjectName(QStringLiteral("orderedObjectCandidatePane"));
        auto* paneLayout = new QVBoxLayout(pane);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        paneLayout->setSpacing(4);
        inlineMode_ = new QLabel(QStringLiteral("候选对象 · 点击添加"), pane);
        inlineMode_->setObjectName(QStringLiteral("orderedObjectCandidateMode"));
        paneLayout->addWidget(inlineMode_);
        inlineCatalog_ = new ObjectCatalogWidget(icons_, pane);
        inlineCatalog_->setObjectName(QStringLiteral("orderedObjectCatalog"));
        inlineCatalog_->SetDirectoryVisible(false);
        inlineCatalog_->SetEntries(entries_);
        inlineCatalog_->SetKindFilter(kind_, !kind_.isEmpty());
        inlineCatalog_->setEnabled(!readOnly_);
        paneLayout->addWidget(inlineCatalog_, 1);
        content_->addWidget(pane);
        content_->setStretchFactor(0, 2);
        content_->setStretchFactor(1, 3);
        content_->setSizes({320, 520});
        connect(inlineCatalog_, &ObjectCatalogWidget::EntryChosen, this, &OrderedObjectListWidget::AcceptEntry);
    }
    if (inlineCatalog_) inlineCatalog_->parentWidget()->setVisible(enabled);
    replacementRow_ = -1;
    pickerBaseline_ = ids_;
    if (inlineMode_) inlineMode_->setText(QStringLiteral("候选对象 · 点击添加"));
}

void OrderedObjectListWidget::RefreshIcons() {
    picker_->Catalog()->RefreshIcons();
    if (inlineCatalog_) inlineCatalog_->RefreshIcons();
    RebuildList(list_->currentRow());
}

const ObjectViewEntry* OrderedObjectListWidget::Resolve(const QString& id) const {
    for (const auto& entry : entries_) {
        if (entry.id == id && (kind_.isEmpty() || entry.kind == kind_)) {
            return &entry;
        }
    }
    return nullptr;
}

void OrderedObjectListWidget::RebuildList(int selectedRow) {
    const QSignalBlocker blocker(list_);
    list_->clear();
    int unresolved = 0;
    for (int row = 0; row < ids_.size(); ++row) {
        const QString& id = ids_[row];
        const auto* entry = Resolve(id);
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, id);
        item->setSizeHint(QSize(120, 56));
        if (entry) {
            item->setIcon(EntryIcon(icons_, *entry));
            item->setText(QStringLiteral("%1  %2\n%3").arg(row + 1).arg(EntryName(*entry), id));
            item->setToolTip(EntryTooltip(icons_, *entry));
            const auto status = IconStatus(icons_, *entry);
            if (!status.isEmpty()) {
                item->setText(item->text() + QStringLiteral(" · ") + status);
            }
        } else {
            ++unresolved;
            const auto known = std::find_if(knownEntries_.begin(), knownEntries_.end(),
                [&id](const ObjectViewEntry& candidate) { return candidate.id == id; });
            if (known != knownEntries_.end()) {
                item->setText(QStringLiteral("%1  %2\n类型不匹配 · 已保留").arg(row + 1).arg(id));
                item->setToolTip(QStringLiteral("原始 ID：%1\n实际类型：%2\n当前字段不接受此类型；原值保留。")
                    .arg(id, KindName(known->kind)));
            } else {
                item->setText(QStringLiteral("%1  %2\n未解析引用 · 已保留").arg(row + 1).arg(id));
                item->setToolTip(QStringLiteral("原始 ID：%1\n目录未找到相同类型对象；原值保留。").arg(id));
            }
        }
    }
    if (!ids_.isEmpty()) {
        list_->setCurrentRow(std::clamp(selectedRow, 0, static_cast<int>(ids_.size()) - 1));
    }
    status_->setText(ids_.isEmpty() ? QStringLiteral("没有已选对象") :
        QStringLiteral("%1 项%2").arg(ids_.size()).arg(unresolved > 0 ?
            QStringLiteral(" · %1 个未解析引用").arg(unresolved) : QString{}));
    UpdateActions();
}

void OrderedObjectListWidget::UpdateActions() {
    const int row = list_->currentRow();
    const bool selected = row >= 0 && row < ids_.size();
    add_->setEnabled(!readOnly_);
    replace_->setEnabled(!readOnly_ && selected);
    remove_->setEnabled(!readOnly_ && selected);
    up_->setEnabled(!readOnly_ && selected && row > 0);
    down_->setEnabled(!readOnly_ && selected && row + 1 < ids_.size());
}

void OrderedObjectListWidget::OpenPicker(bool replace) {
    if (readOnly_) {
        return;
    }
    replacementRow_ = replace ? list_->currentRow() : -1;
    if (replace && (replacementRow_ < 0 || replacementRow_ >= ids_.size())) {
        return;
    }
    pickerBaseline_ = ids_;
    if (inlineCatalogEnabled_ && inlineCatalog_) {
        popupSessionActive_ = false;
        inlineMode_->setText(replace ? QStringLiteral("候选对象 · 点击替换第 %1 项").arg(replacementRow_ + 1)
            : QStringLiteral("候选对象 · 点击添加"));
        inlineCatalog_->SetSearchText({});
        inlineCatalog_->SetCurrentId(replace ? ids_[replacementRow_] : QString{});
        inlineCatalog_->FocusSearch();
    } else {
        popupSessionActive_ = true;
        picker_->Catalog()->SetSearchText({});
        picker_->SetCurrentId(replace ? ids_[replacementRow_] : QString{});
        picker_->OpenAt(replace ? static_cast<QWidget*>(replace_) : static_cast<QWidget*>(add_));
    }
}

void OrderedObjectListWidget::AcceptEntry(const ObjectViewEntry& entry) {
    if (readOnly_ || !entry.available || (!kind_.isEmpty() && entry.kind != kind_) || entry.id.isEmpty()) {
        return;
    }
    const auto candidate = std::find_if(entries_.begin(), entries_.end(), [&entry](const ObjectViewEntry& value) {
        return value.id == entry.id && value.kind == entry.kind && value.available;
    });
    if (candidate == entries_.end()) return;
    // 外部撤销或切换文档后，不把旧浮窗的选择写到新的列表位置。
    if (ids_ != pickerBaseline_) {
        status_->setText(QStringLiteral("列表已变化，请重新选择"));
        return;
    }
    QStringList next = ids_;
    int row = replacementRow_;
    if (row >= 0 && row < next.size()) {
        next[row] = entry.id;
    } else {
        row = static_cast<int>(next.size());
        next.append(entry.id);
    }
    // 保留已有重复项；新增重复选择只定位，不擅自去重原始文档。
    const int duplicate = ids_.indexOf(entry.id);
    if (duplicate >= 0 && duplicate != replacementRow_) {
        list_->setCurrentRow(duplicate);
        status_->setText(QStringLiteral("该对象已在列表中"));
        return;
    }
    if (next == ids_) {
        return;
    }
    ids_ = next;
    popupSessionActive_ = false;
    RebuildList(row);
    replacementRow_ = -1;
    pickerBaseline_ = ids_;
    if (inlineMode_) inlineMode_->setText(QStringLiteral("候选对象 · 点击添加"));
    emit IdsChanged(ids_);
}

void OrderedObjectListWidget::RemoveCurrent() {
    const int row = list_->currentRow();
    if (readOnly_ || row < 0 || row >= ids_.size()) {
        return;
    }
    picker_->hide();
    popupSessionActive_ = false;
    ids_.removeAt(row);
    RebuildList(std::min(row, static_cast<int>(ids_.size()) - 1));
    replacementRow_ = -1;
    pickerBaseline_ = ids_;
    if (inlineMode_) inlineMode_->setText(QStringLiteral("候选对象 · 点击添加"));
    emit IdsChanged(ids_);
}

void OrderedObjectListWidget::MoveCurrent(int direction) {
    const int row = list_->currentRow();
    const int next = row + direction;
    if (readOnly_ || row < 0 || next < 0 || next >= ids_.size()) {
        return;
    }
    picker_->hide();
    popupSessionActive_ = false;
    ids_.swapItemsAt(row, next);
    RebuildList(next);
    replacementRow_ = -1;
    pickerBaseline_ = ids_;
    if (inlineMode_) inlineMode_->setText(QStringLiteral("候选对象 · 点击添加"));
    emit IdsChanged(ids_);
}


} // namespace War3Map::QtApp

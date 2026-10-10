#include "Widgets/ObjectCatalogWidgets.h"
#include "Widgets/ObjectCatalogDetail.h"

namespace War3Map::QtApp {
using namespace CatalogDetail;

// 通用对象目录（图标网格 / 列表、筛选与搜索）。

ObjectCatalogWidget::ObjectCatalogWidget(IconProvider* icons, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("objectCatalog"));
    setAccessibleName(QStringLiteral("对象资源目录"));
    entries_ = new ObjectEntryModel(icons, this);
    filter_ = new ObjectEntryFilter(this);
    filter_->setSourceModel(entries_);
    filter_->setDynamicSortFilter(true);
    filter_->sort(0);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* splitter = new QSplitter(this);
    splitter->setHandleWidth(1);
    root->addWidget(splitter);
    directory_ = new QTreeWidget(splitter);
    directory_->setObjectName(QStringLiteral("objectCatalogDirectory"));
    directory_->setAccessibleName(QStringLiteral("对象分类目录"));
    directory_->setHeaderHidden(true);
    directory_->setIndentation(14);
    directory_->setMinimumWidth(130);
    directory_->setMaximumWidth(300);
    auto* content = new QWidget(splitter);
    content->setObjectName(QStringLiteral("objectCatalogContent"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    auto* searchRow = new QHBoxLayout;
    searchRow->setSpacing(4);
    search_ = new QLineEdit(content);
    search_->setObjectName(QStringLiteral("objectCatalogSearch"));
    search_->setAccessibleName(QStringLiteral("搜索物体名称或 rawcode"));
    search_->setPlaceholderText(QStringLiteral("搜索名称 / ID"));
    search_->setToolTip(QStringLiteral("名称、ID、分类或资源路径"));
    search_->setClearButtonEnabled(true);
    searchRow->addWidget(search_, 1);
    iconsButton_ = new QToolButton(content);
    iconsButton_->setObjectName(QStringLiteral("objectCatalogIconsButton"));
    iconsButton_->setText(QStringLiteral("图标"));
    iconsButton_->setToolTip(QStringLiteral("图标视图"));
    iconsButton_->setAccessibleName(QStringLiteral("图标视图"));
    listButton_ = new QToolButton(content);
    listButton_->setObjectName(QStringLiteral("objectCatalogListButton"));
    listButton_->setText(QStringLiteral("列表"));
    listButton_->setToolTip(QStringLiteral("列表视图"));
    listButton_->setAccessibleName(QStringLiteral("列表视图"));
    for (auto* button : {iconsButton_, listButton_}) {
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        searchRow->addWidget(button);
    }
    layout->addLayout(searchRow);
    auto* filters = new QHBoxLayout;
    filters->setSpacing(4);
    kinds_ = new QComboBox(content);
    races_ = new QComboBox(content);
    categories_ = new QComboBox(content);
    kinds_->setAccessibleName(QStringLiteral("物体类型"));
    kinds_->setObjectName(QStringLiteral("objectCatalogKinds"));
    races_->setAccessibleName(QStringLiteral("种族筛选"));
    races_->setObjectName(QStringLiteral("objectCatalogRaces"));
    categories_->setAccessibleName(QStringLiteral("分类筛选"));
    categories_->setObjectName(QStringLiteral("objectCatalogCategories"));
    for (auto* combo : {kinds_, races_, categories_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(5);
        filters->addWidget(combo, 1);
    }
    layout->addLayout(filters);
    view_ = new QListView(content);
    view_->setObjectName(QStringLiteral("objectCatalogGrid"));
    view_->setAccessibleName(QStringLiteral("对象候选"));
    view_->setModel(filter_);
    view_->setItemDelegate(new ObjectCatalogDelegate(view_));
    view_->setResizeMode(QListView::Adjust);
    view_->setMovement(QListView::Static);
    view_->setLayoutMode(QListView::Batched);
    view_->setBatchSize(64);
    view_->setSpacing(0);
    view_->setWordWrap(false);
    view_->setMouseTracking(true);
    view_->viewport()->setMouseTracking(true);
    view_->setFrameShape(QFrame::NoFrame);
    view_->setUniformItemSizes(true);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(view_, 1);
    empty_ = new QLabel(QStringLiteral("尚未载入对象目录"), content);
    empty_->setTextFormat(Qt::PlainText);
    empty_->setObjectName(QStringLiteral("objectCatalogEmpty"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    layout->addWidget(empty_, 1);
    status_ = new QLabel(content);
    status_->setObjectName(QStringLiteral("objectCatalogStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setAccessibleName(QStringLiteral("目录状态"));
    status_->setWordWrap(false);
    layout->addWidget(status_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({180, 620});
    SetViewMode(ViewMode::Icons);

    search_->installEventFilter(this);
    view_->installEventFilter(this);
    connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilters(); });
    connect(iconsButton_, &QToolButton::clicked, this, [this] { SetViewMode(ViewMode::Icons); });
    connect(listButton_, &QToolButton::clicked, this, [this] { SetViewMode(ViewMode::List); });
    connect(kinds_, &QComboBox::currentIndexChanged, this, [this] {
        if (!kindLocked_) {
            kind_ = kinds_->currentData().toString();
            const QSignalBlocker raceBlocker(races_);
            const QSignalBlocker categoryBlocker(categories_);
            races_->setCurrentIndex(0);
            categories_->setCurrentIndex(0);
            RebuildFilters();
            ApplyFilters();
        }
    });
    connect(races_, &QComboBox::currentIndexChanged, this, [this] { ApplyFilters(); });
    connect(categories_, &QComboBox::currentIndexChanged, this, [this] { ApplyFilters(); });
    connect(directory_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const QString requestedKind = item->data(0, Qt::UserRole).toString();
        const QVariant requestedCategory = item->data(0, Qt::UserRole + 1);
        if (kindLocked_ && requestedKind != kind_) {
            return;
        }
        kind_ = requestedKind;
        const QSignalBlocker kindBlocker(kinds_);
        const QSignalBlocker raceBlocker(races_);
        const QSignalBlocker categoryBlocker(categories_);
        kinds_->setCurrentIndex(std::max(0, kinds_->findData(kind_)));
        races_->setCurrentIndex(0);
        RebuildFilters(false);
        categories_->setCurrentIndex(std::max(0, categories_->findData(requestedCategory)));
        ApplyFilters();
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        if (const auto entry = CurrentEntry()) {
            emit EntrySelected(*entry);
        }
    });
    connect(view_, &QListView::clicked, this, [this] { ChooseCurrent(); });
    RebuildFilters();
    UpdateStatus();
}

void ObjectCatalogWidget::SetEntries(const QVector<ObjectViewEntry>& entries) {
    const auto selected = CurrentEntry();
    entries_->SetEntries(entries);
    RebuildFilters();
    ApplyFilters();
    if (selected) {
        SetCurrentId(selected->id);
    }
}

void ObjectCatalogWidget::RebuildFilters(bool rebuildDirectory) {
    QStringList kinds, races, categories;
    for (const auto& entry : entries_->Entries()) {
        kinds << entry.kind;
        if (kind_.isEmpty() || entry.kind == kind_) {
            races << entry.race;
            categories << entry.category;
        }
    }
    if (!kind_.isEmpty()) {
        kinds << kind_;
    }
    FillCombo(kinds_, QStringLiteral("全部类型"), kinds, true);
    {
        const QSignalBlocker blocker(kinds_);
        kinds_->setCurrentIndex(std::max(0, kinds_->findData(kind_)));
    }
    FillCombo(races_, QStringLiteral("全部种族"), races);
    FillCombo(categories_, QStringLiteral("全部分类"), categories);
    if (!rebuildDirectory) {
        return;
    }
    const QSignalBlocker blocker(directory_);
    directory_->clear();
    auto* all = new QTreeWidgetItem(directory_, {QStringLiteral("全部对象")});
    all->setData(0, Qt::UserRole, kindLocked_ ? kind_ : QString{});
    kinds.removeDuplicates();
    std::sort(kinds.begin(), kinds.end());
    for (const auto& kind : kinds) {
        if (kindLocked_ && kind != kind_) {
            continue;
        }
        auto* parent = new QTreeWidgetItem(all, {KindName(kind)});
        parent->setData(0, Qt::UserRole, kind);
        QSet<QString> categorySet;
        for (const auto& entry : entries_->Entries()) {
            if (entry.kind == kind && !entry.category.isEmpty()) {
                categorySet.insert(entry.category);
            }
        }
        QStringList sortedCategories(categorySet.begin(), categorySet.end());
        std::sort(sortedCategories.begin(), sortedCategories.end());
        for (const auto& category : sortedCategories) {
            auto* child = new QTreeWidgetItem(parent, {ClassificationName(category)});
            child->setData(0, Qt::UserRole, kind);
            child->setData(0, Qt::UserRole + 1, category);
        }
        parent->setExpanded(kind == kind_);
    }
    all->setExpanded(true);
}

void ObjectCatalogWidget::SetKindFilter(const QString& kind, bool locked) {
    kind_ = kind;
    kindLocked_ = locked;
    kinds_->setEnabled(!locked);
    kinds_->setVisible(!locked);
    RebuildFilters();
    ApplyFilters();
}

void ObjectCatalogWidget::SetDirectoryVisible(bool visible) { directory_->setVisible(visible); }
void ObjectCatalogWidget::SetSearchText(const QString& text) { search_->setText(text); }
void ObjectCatalogWidget::SetCatalogStatus(const QString& status) { catalogStatus_ = status; UpdateStatus(); }
void ObjectCatalogWidget::FocusSearch() { search_->setFocus(Qt::PopupFocusReason); search_->selectAll(); }
void ObjectCatalogWidget::RefreshIcons() { entries_->RefreshIcons(); }

void ObjectCatalogWidget::SetViewMode(ViewMode mode) {
    viewMode_ = mode;
    const bool icons = mode == ViewMode::Icons;
    const QModelIndex current = view_->currentIndex();
    view_->setViewMode(icons ? QListView::IconMode : QListView::ListMode);
    view_->setFlow(icons ? QListView::LeftToRight : QListView::TopToBottom);
    view_->setWrapping(icons);
    view_->setMovement(QListView::Static);
    view_->setIconSize(icons ? QSize(48, 48) : QSize(32, 32));
    view_->setGridSize(icons ? QSize(112, 96) : QSize{});
    iconsButton_->setChecked(icons);
    listButton_->setChecked(!icons);
    if (current.isValid()) view_->scrollTo(current);
    view_->viewport()->update();
}

void ObjectCatalogWidget::ApplyFilters() {
    filter_->SetFilters(kind_, races_->currentData().toString(), categories_->currentData().toString(), search_->text());
    UpdateStatus();
}

void ObjectCatalogWidget::UpdateStatus() {
    const int count = filter_->rowCount();
    view_->setVisible(count > 0);
    empty_->setVisible(count == 0);
    empty_->setText(entries_->rowCount() == 0 ?
        (catalogStatus_.isEmpty() ? QStringLiteral("尚未载入对象目录") : catalogStatus_) :
        QStringLiteral("没有匹配对象"));
    QString text = QStringLiteral("%1 / %2 项").arg(count).arg(entries_->rowCount());
    status_->setText(text);
    status_->setToolTip(catalogStatus_);
}

std::optional<ObjectViewEntry> ObjectCatalogWidget::CurrentEntry() const {
    const auto current = view_->currentIndex();
    if (!current.isValid()) {
        return std::nullopt;
    }
    const auto* entry = entries_->EntryAt(filter_->mapToSource(current).row());
    return entry ? std::optional<ObjectViewEntry>(*entry) : std::nullopt;
}

void ObjectCatalogWidget::SetCurrentId(const QString& id) {
    for (int row = 0; row < filter_->rowCount(); ++row) {
        const auto index = filter_->index(row, 0);
        if (index.data(Qt::UserRole).toString() == id) {
            view_->setCurrentIndex(index);
            view_->scrollTo(index);
            return;
        }
    }
    view_->setCurrentIndex({});
    view_->clearSelection();
}

void ObjectCatalogWidget::ChooseCurrent() {
    if (const auto entry = CurrentEntry(); entry && entry->available) {
        emit EntryChosen(*entry);
    }
}

bool ObjectCatalogWidget::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == view_ || watched == search_) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == search_ && filter_->rowCount() == 1) {
                view_->setCurrentIndex(filter_->index(0, 0));
                ChooseCurrent();
            } else if (watched == view_) {
                ChooseCurrent();
            }
            return true;
        }
        if (watched == search_ && key->key() == Qt::Key_Down && filter_->rowCount() > 0) {
            view_->setFocus();
            view_->setCurrentIndex(filter_->index(0, 0));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}


} // namespace War3Map::QtApp

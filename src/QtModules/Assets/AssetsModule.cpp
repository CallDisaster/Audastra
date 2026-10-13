#include "AssetsModule.h"

#include "AssetsDetail.h"
#include "../NavigationBookmarkDetail.h"
#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "Modules/ModuleHost.h"
#include "Widgets/SearchField.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>
#include <QVBoxLayout>

#include <map>
#include <vector>

// 资源模块的界面与刷新（操作、拖放、右键菜单见 AssetsActions.cpp）。
namespace War3Map::Modules {

using namespace AssetsDetail;

AssetsModule::AssetsModule(QtProject::EditorProject& project)
    : project_(project), undoStack_(project.AssetUndoStack()) {
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, &AssetsModule::Refresh);
}

AssetsModule::~AssetsModule() {
    rebuilding_ = true;
    delete page_.data();
}

Shell::ModuleInfo AssetsModule::Info() const {
    return {"assets", tr("资源"), "lucide/layers", QKeySequence(Qt::ALT | Qt::Key_5)};
}

QUndoStack* AssetsModule::UndoStack() {
    return undoStack_;
}

void AssetsModule::AttachHost(Shell::ModuleHost* host) {
    host_ = host;
}

void AssetsModule::Status(const QString& text) const {
    if (host_ != nullptr) host_->ShowStatusMessage(text, 6000);
}

QWidget* AssetsModule::Widget() {
    if (page_) {
        return page_;
    }
    page_ = new QWidget;
    page_->setObjectName("assetsWorkspace");
    page_->installEventFilter(this);
    auto* layout = new QVBoxLayout(page_);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    toolbar_ = new QWidget(page_);
    toolbar_->setObjectName("assetToolbar");
    // 允许宿主直接收窄；完整按钮行的最小提示不能抢先把浮窗撑回去。
    toolbar_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    toolbar_->installEventFilter(this);
    auto* toolbar = new QHBoxLayout(toolbar_);
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);
    toolbarTitle_ = new QLabel(tr("资源"), toolbar_);
    toolbarTitle_->setObjectName("sectionTitle");
    toolbar->addWidget(toolbarTitle_);
    toolbar->addSpacing(12);
    const auto button = [&](const char* name, const QString& text, const char* icon, const QString& tip) {
        auto* item = new QPushButton(UiKit::SvgIcon(QString::fromLatin1(icon)), text, page_);
        item->setObjectName(QString::fromLatin1(name));
        item->setToolTip(tip);
        toolbar->addWidget(item);
        return item;
    };
    auto* importButton = button("addImportButton", tr("导入文件…"), "lucide/file-plus-2", tr("把磁盘文件导入到当前文件夹（也可以直接拖进来）"));
    importButton_ = importButton;
    auto* folderButton = button("newAssetFolderButton", tr("新建文件夹"), "lucide/folder-plus", tr("在当前文件夹中新建文件夹"));
    auto* renameButton = button("renameAssetButton", tr("重命名"), "lucide/pencil", tr("修改完整路径（F2）"));
    auto* deleteButton = button("removeImportButton", tr("删除"), "lucide/trash-2", tr("从地图中删除选中的资源（保存时生效，可撤销）"));
    secondaryButtons_ = {folderButton, renameButton, deleteButton};
    moreButton_ = new QToolButton(toolbar_);
    moreButton_->setObjectName("assetMoreActions");
    moreButton_->setText(tr("更多"));
    moreButton_->setToolTip(tr("更多资源操作"));
    moreButton_->setPopupMode(QToolButton::InstantPopup);
    auto* more = new QMenu(moreButton_);
    for (auto* control : {folderButton, renameButton, deleteButton}) {
        // 菜单沿用原按钮的业务连接，编辑、撤销和保存仍只有一条路径。
        auto* action = more->addAction(control->icon(), control->text(), control, &QPushButton::click);
        action->setObjectName(control->objectName() + QStringLiteral("Action"));
        action->setToolTip(control->toolTip());
    }
    moreButton_->setMenu(more);
    moreButton_->hide();
    toolbar->addWidget(moreButton_);
    toolbar->addStretch();
    search_ = new QLineEdit(page_);
    search_->setObjectName("assetSearch");
    search_->setPlaceholderText(tr("搜索整个地图的资源路径"));
    search_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(search_);
    search_->setMinimumWidth(120);
    search_->setMaximumWidth(320);
    toolbar->addWidget(search_, 1);
    layout->addWidget(toolbar_);

    // 路径栏：地图 › 文件夹 › 子文件夹，点击跳转。
    breadcrumbScroll_ = new QScrollArea(page_);
    breadcrumbScroll_->setObjectName("assetBreadcrumbScroll");
    breadcrumbScroll_->setFrameShape(QFrame::NoFrame);
    breadcrumbScroll_->setWidgetResizable(true);
    breadcrumbScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    breadcrumbScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    breadcrumbScroll_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    breadcrumbScroll_->viewport()->installEventFilter(this);
    breadcrumb_ = new QWidget(breadcrumbScroll_);
    breadcrumb_->setObjectName("assetBreadcrumb");
    breadcrumbLayout_ = new QHBoxLayout(breadcrumb_);
    breadcrumbLayout_->setContentsMargins(0, 0, 0, 0);
    breadcrumbLayout_->setSpacing(2);
    breadcrumbLayout_->setSizeConstraint(QLayout::SetMinimumSize);
    breadcrumbScroll_->setWidget(breadcrumb_);
    connect(breadcrumbScroll_->horizontalScrollBar(), &QScrollBar::rangeChanged, this,
        [this] { QueueResponsiveLayout(); });
    layout->addWidget(breadcrumbScroll_);

    auto* splitter = new QSplitter(Qt::Horizontal, page_);
    splitter->setObjectName("assetSplitter");
    treeModel_ = new AssetItemModel(this);
    tree_ = new QTreeView(splitter);
    tree_->setObjectName("assetFolderTree");
    tree_->setModel(treeModel_);
    tree_->setHeaderHidden(true);
    tree_->setIconSize(QSize(16, 16));
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->viewport()->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->viewport()->installEventFilter(this);
    listModel_ = new AssetItemModel(this);
    files_ = new QTreeView(splitter);
    files_->setObjectName("assetFileList");
    files_->setModel(listModel_);
    files_->setRootIsDecorated(false);
    files_->setUniformRowHeights(true);
    files_->setAlternatingRowColors(true);
    files_->setIconSize(QSize(16, 16));
    files_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    files_->setSortingEnabled(true);
    files_->setDragEnabled(true);
    files_->setAcceptDrops(true);
    files_->viewport()->setAcceptDrops(true);
    files_->setDragDropMode(QAbstractItemView::DragDrop);
    files_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    files_->setContextMenuPolicy(Qt::CustomContextMenu);
    files_->header()->setSectionsMovable(false);
    files_->header()->setStretchLastSection(true);
    files_->viewport()->installEventFilter(this);
    files_->installEventFilter(this);
    splitter->addWidget(tree_);
    splitter->addWidget(files_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({260, 780});
    layout->addWidget(splitter, 1);
    summary_ = new QLabel(page_);
    summary_->setObjectName("assetSummary");
    summary_->setWordWrap(true);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);

    connect(search_, &QLineEdit::textChanged, this, [this]() { RebuildList(); });
    connect(importButton, &QPushButton::clicked, this, [this]() {
        const QStringList chosen = QFileDialog::getOpenFileNames(page_, tr("选择要导入的文件"), QString{}, tr("所有文件 (*.*)"));
        if (!chosen.isEmpty()) AddFiles(chosen, folder_);
    });
    connect(folderButton, &QPushButton::clicked, this, [this]() { NewFolder({}); });
    connect(renameButton, &QPushButton::clicked, this, [this]() {
        const QStringList selected = SelectedPaths();
        if (!selected.isEmpty()) EditProperties(selected.front());
    });
    connect(deleteButton, &QPushButton::clicked, this, [this]() { (void)DeleteAssets(SelectedPaths()); });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
        // 树里点选文件夹：只刷新列表与路径栏（不在树的信号里重建树本身）。
        if (rebuilding_ || !current.isValid()) return;
        folder_ = current.data(kPathRole).toString();
        RebuildBreadcrumb();
        RebuildList();
    });
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) { expanded_.insert(index.data(kPathRole).toString().toLower()); });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) { expanded_.remove(index.data(kPathRole).toString().toLower()); });
    connect(files_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const QModelIndex name = index.siblingAtColumn(0);
        if (name.data(kKindRole).toInt() == 0) OpenFolder(name.data(kPathRole).toString());
        else EditProperties(name.data(kPathRole).toString());
    });
    connect(files_, &QTreeView::customContextMenuRequested, this, &AssetsModule::ShowContextMenu);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &AssetsModule::ShowContextMenu);
    Refresh();
    UpdateResponsiveLayout();
    return page_;
}

void AssetsModule::Refresh() {
    if (!page_) {
        return;
    }
    RebuildTree();
    RebuildList();
}

// 文件夹树：全部资源路径的父文件夹 + 新建的空文件夹。
void AssetsModule::RebuildTree() {
    rebuilding_ = true;
    treeModel_->clear();
    auto* root = new QStandardItem(UiKit::SystemFolderIcon(), tr("地图"));
    root->setData(QString{}, kPathRole);
    root->setData(0, kKindRole);
    root->setDropEnabled(true);
    treeModel_->appendRow(root);
    std::map<QString, QStandardItem*> byKey{{QString{}, root}};
    const std::function<QStandardItem*(const QString&)> ensure = [&](const QString& folder) -> QStandardItem* {
        const QString key = folder.toLower();
        if (const auto found = byKey.find(key); found != byKey.end()) return found->second;
        QStandardItem* parent = ensure(ParentFolder(folder));
        auto* item = new QStandardItem(UiKit::SystemFolderIcon(), FileName(folder));
        item->setData(folder, kPathRole);
        item->setData(0, kKindRole);
        item->setData(FileName(folder).toLower(), kSortRole);
        item->setDropEnabled(true);
        parent->appendRow(item);
        byKey.emplace(key, item);
        return item;
    };
    for (const auto& entry : project_.AssetEntries()) ensure(ParentFolder(Normalized(entry.path)));
    for (const QString& folder : emptyFolders_) ensure(folder);
    treeModel_->setSortRole(kSortRole);
    treeModel_->sort(0);
    // 当前文件夹已不存在时退回到最近的上级。
    while (!folder_.isEmpty() && !byKey.contains(folder_.toLower())) folder_ = ParentFolder(folder_);
    tree_->expand(root->index());
    for (const auto& [key, item] : byKey) {
        if (expanded_.contains(key)) tree_->expand(item->index());
    }
    const QModelIndex current = byKey[folder_.toLower()]->index();
    for (QModelIndex parent = current.parent(); parent.isValid(); parent = parent.parent()) tree_->expand(parent);
    tree_->setCurrentIndex(current);
    rebuilding_ = false;
    RebuildBreadcrumb();
}

void AssetsModule::RebuildList() {
    const QStringList previous = SelectedPaths();
    listModel_->clear();
    listModel_->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("大小"), tr("状态"), tr("完整路径")});
    const QString query = search_->text().trimmed();
    const auto entries = project_.AssetEntries();
    QSet<QString> missing;
    for (const QString& icon : project_.MissingDisabledIcons()) missing.insert(QtProject::EditorProject::AssetKey(icon));
    const QIcon warning = UiKit::SvgIcon(QStringLiteral("lucide/triangle-alert"), QColor(0xE8, 0xA3, 0x17));
    const auto addRow = [&](QList<QStandardItem*> row) {
        for (QStandardItem* item : row) item->setEditable(false);
        listModel_->appendRow(row);
    };
    // 当前文件夹的子文件夹在前（没有搜索时）。
    if (query.isEmpty()) {
        const QModelIndex folderIndex = tree_->currentIndex();
        if (const QStandardItem* folder = treeModel_->itemFromIndex(folderIndex)) {
            for (int row = 0; row < folder->rowCount(); ++row) {
                const QStandardItem* child = folder->child(row);
                auto* name = new QStandardItem(UiKit::SystemFolderIcon(), child->text());
                name->setData(child->data(kPathRole), kPathRole);
                name->setData(0, kKindRole);
                name->setData(QStringLiteral("0") + child->text().toLower(), kSortRole);
                name->setDropEnabled(true);
                auto* type = new QStandardItem(tr("文件夹"));
                auto* size = new QStandardItem;
                size->setData(-1, kSortRole);
                addRow({name, type, size, new QStandardItem, new QStandardItem(child->data(kPathRole).toString())});
            }
        }
    }
    int files = 0;
    qint64 bytes = 0;
    for (const auto& entry : entries) {
        const QString path = Normalized(entry.path);
        if (query.isEmpty() ? !SameFolder(ParentFolder(path), folder_) : !path.contains(query, Qt::CaseInsensitive)) continue;
        ++files;
        bytes += entry.size;
        auto* name = new QStandardItem(UiKit::SystemFileIcon(), FileName(path));
        name->setData(path, kPathRole);
        name->setData(1, kKindRole);
        name->setData(QStringLiteral("1") + FileName(path).toLower(), kSortRole);
        name->setDropEnabled(false);
        auto* type = new QStandardItem(TypeText(path));
        type->setData(type->text(), kSortRole);
        auto* size = new QStandardItem(SizeText(entry.size));
        size->setData(entry.size, kSortRole);
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QStringList states;
        if (entry.ambiguous) states << tr("来源歧义 · 禁止编辑");
        else if (entry.ownedPayload) states << tr("工程草稿");
        else if (entry.Replacement()) states << tr("待替换 · 未保存");
        else if (entry.Pending()) states << tr("待导入");
        else if (entry.originalPath != entry.path) states << tr("已移动 · 未保存");
        auto* state = new QStandardItem;
        if (missing.contains(QtProject::EditorProject::AssetKey(path))) {
            state->setIcon(warning);
            states << tr("缺少暂停图标");
            state->setToolTip(tr("没有导入 %1。\n游戏暂停或按钮禁用时会显示成绿色方块；保存地图时会自动生成（去色压暗）。")
                .arg(QtProject::EditorProject::DisabledIconPath(path)));
            name->setIcon(warning);
        }
        state->setText(states.join(QStringLiteral(" · ")));
        auto* full = new QStandardItem(path);
        full->setToolTip(entry.ownedPayload ? tr("%1\n内容保存在工程中。").arg(path)
            : entry.Pending() ? tr("%1\n磁盘来源：%2").arg(path, entry.diskPath) : path);
        addRow({name, type, size, state, full});
    }
    listModel_->setSortRole(kSortRole);
    files_->sortByColumn(files_->header()->sortIndicatorSection() < 0 ? 0 : files_->header()->sortIndicatorSection(),
        files_->header()->sortIndicatorOrder());
    files_->header()->setSectionResizeMode(QHeaderView::Interactive);
    if (files_->columnWidth(0) < 60) {
        files_->setColumnWidth(0, 260);
        files_->setColumnWidth(1, 100);
        files_->setColumnWidth(2, 80);
        files_->setColumnWidth(3, 170);
    }
    // 保留选中项。
    for (int row = 0; row < listModel_->rowCount(); ++row) {
        const QModelIndex index = listModel_->index(row, 0);
        if (previous.contains(index.data(kPathRole).toString())) {
            files_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    summary_->setText(query.isEmpty()
        ? tr("%1 个文件 · %2 · 全图 %3 个资源，%4 个缺少暂停图标").arg(files).arg(SizeText(bytes)).arg(entries.size()).arg(missing.size())
        : tr("找到 %1 个资源").arg(files));
    if (project_.directorySnapshot_ && !project_.archiveSession_)
        summary_->setText(summary_->text().replace(tr("全图"), tr("当前快照"))
            + tr(" · 目录扫描可能跳过无权限项，清单可能不完整"));
}

void AssetsModule::RebuildBreadcrumb() {
    const QScopedValueRollback rebuilding(rebuilding_, true);
    const bool hadFocus = breadcrumb_->isAncestorOf(QApplication::focusWidget());
    currentCrumb_.clear();
    while (QLayoutItem* item = breadcrumbLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList parts = folder_.isEmpty() ? QStringList{} : folder_.split(QLatin1Char('\\'));
    QString path;
    const auto crumb = [this](const QString& text, const QString& target) {
        auto* button = new QToolButton(breadcrumb_);
        button->setText(text);
        button->setToolTip(target.isEmpty() ? tr("地图根目录") : target);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        connect(button, &QToolButton::clicked, this, [this, target]() { OpenFolder(target); });
        breadcrumbLayout_->addWidget(button);
        currentCrumb_ = button;
    };
    crumb(tr("地图"), {});
    for (const QString& part : parts) {
        path = JoinPath(path, part);
        auto* separator = new QLabel(QStringLiteral("›"), breadcrumb_);
        breadcrumbLayout_->addWidget(separator);
        crumb(part, path);
    }
    breadcrumbLayout_->addStretch();
    if (hadFocus) currentCrumb_->setFocus(Qt::OtherFocusReason);
    QueueResponsiveLayout();
}

void AssetsModule::QueueResponsiveLayout() {
    if (!page_ || layoutQueued_) return;
    layoutQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        layoutQueued_ = false;
        UpdateResponsiveLayout();
    });
}

void AssetsModule::UpdateResponsiveLayout() {
    if (rebuilding_ || updatingLayout_ || !page_ || !toolbar_ || !moreButton_ || !breadcrumbScroll_) return;
    const QScopedValueRollback updating(updatingLayout_, true);
    // 按实际字体与样式测量完整动作条，不因按钮已隐藏而改变切换阈值。
    int expandedWidth = toolbarTitle_->sizeHint().width() + importButton_->sizeHint().width() +
        240 + 12 + 6 * toolbar_->layout()->spacing();
    for (const auto& control : secondaryButtons_) expandedWidth += control->sizeHint().width();
    const bool compact = toolbar_->width() < expandedWidth;
    bool moveFocus = !compact && moreButton_->hasFocus();
    for (const auto& control : secondaryButtons_) {
        moveFocus = moveFocus || (compact && control->hasFocus());
        control->setVisible(!compact);
    }
    moreButton_->setVisible(compact);
    if (moveFocus) {
        if (compact) moreButton_->setFocus(Qt::OtherFocusReason);
        else secondaryButtons_.front()->setFocus(Qt::OtherFocusReason);
    }

    // 路径内容保持完整，只有溢出时为水平滚动条预留高度；缩放不重建路径按钮。
    const bool overflow = breadcrumbLayout_->minimumSize().width() > breadcrumbScroll_->viewport()->width();
    const int height = breadcrumbLayout_->sizeHint().height() + 2 * breadcrumbScroll_->frameWidth() +
        (overflow ? breadcrumbScroll_->horizontalScrollBar()->sizeHint().height() : 0);
    if (breadcrumbScroll_->minimumHeight() != height) breadcrumbScroll_->setFixedHeight(height);
    QWidget* pathFocus = QApplication::focusWidget();
    if (!pathFocus || !breadcrumb_->isAncestorOf(pathFocus)) pathFocus = currentCrumb_;
    if (pathFocus && pathFocus->isVisible()) {
        breadcrumbLayout_->activate();
        breadcrumbScroll_->ensureWidgetVisible(pathFocus, 8, 0);
    }
}

void AssetsModule::OpenFolder(const QString& folder) {
    (void)Widget();
    folder_ = Normalized(folder);
    if (!rebuilding_) {
        RebuildTree();
        RebuildList();
    }
}

QStringList AssetsModule::SelectedPaths() const {
    QStringList paths;
    if (!files_) return paths;
    for (const QModelIndex& index : files_->selectionModel()->selectedRows(0)) {
        if (index.data(kKindRole).toInt() == 1) paths << index.data(kPathRole).toString();
    }
    return paths;
}

QStringList AssetsModule::ListedRows() const {
    QStringList rows;
    for (int row = 0; listModel_ != nullptr && row < listModel_->rowCount(); ++row) {
        QStringList cells;
        for (int column = 0; column < listModel_->columnCount(); ++column) cells << listModel_->index(row, column).data().toString();
        rows << cells.join(QLatin1Char('|'));
    }
    return rows;
}

QStringList AssetsModule::FolderPaths() const {
    QStringList paths;
    const std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int row = 0; row < treeModel_->rowCount(parent); ++row) {
            const QModelIndex index = treeModel_->index(row, 0, parent);
            paths << index.data(kPathRole).toString();
            walk(index);
        }
    };
    if (treeModel_ != nullptr) walk({});
    return paths;
}

QStringList AssetsModule::PendingPaths() const {
    QStringList paths;
    for (const auto& entry : project_.pendingImports_) paths << entry.archivePath;
    return paths;
}

QTreeView* AssetsModule::FileList() const {
    return files_.data();
}

int AssetsModule::AssetCount() const {
    return static_cast<int>(project_.AssetEntries().size());
}

bool AssetsModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "assets") {
        return false;
    }
    const auto marker = target.extra.value("stableIdentity");
    if (target.extra.contains("stableIdentity") && marker.typeId() != QMetaType::Bool) return false;
    const bool strict = marker.toBool();
    if (target.extra.contains("sourceIdentity") && !strict) return false;
    const QString wanted = Normalized(target.id);
    std::vector<QtProject::AssetEntry> entries;
    if (strict) {
        const auto source = target.extra.value("sourceIdentity");
        if (target.kind != QLatin1String("file") || source.typeId() != QMetaType::QString
            || target.id.isEmpty() || target.id.size() > 2048 || target.id.contains(QChar{})
            || source.toString().isEmpty() || source.toString().size() > 2056 || source.toString().contains(QChar{})) return false;
        entries = project_.AssetEntries(4097);
        std::size_t matches = 0;
        for (const auto& entry : entries) {
            if (!SameFolder(entry.path, wanted)) continue;
            const auto identity = entry.Pending() ? QStringLiteral("pending/") + entry.path : entry.originalPath;
            if (++matches != 1 || entry.ambiguous || entry.path != target.id || identity != source.toString()) return false;
        }
        if (matches != 1) return false;
        // Shell已完成PrepareNavigationLeave；只在此时的真实来源仍唯一时才修改视图。
    } else {
        (void)Widget();
        entries = project_.AssetEntries(); // 没有稳定身份extra的旧定位入口保持兼容。
    }
    (void)Widget();
    for (const auto& entry : entries) {
        if (!SameFolder(entry.path, wanted)) continue;
        search_->clear();
        OpenFolder(ParentFolder(Normalized(entry.path)));
        for (int row = 0; row < listModel_->rowCount(); ++row) {
            const QModelIndex index = listModel_->index(row, 0);
            const auto path = index.data(kPathRole).toString();
            if (strict ? Normalized(path) == wanted : SameFolder(path, wanted)) {
                files_->setCurrentIndex(index);
                files_->scrollTo(index);
            }
        }
        return true;
    }
    return false;
}

QJsonObject AssetsModule::SaveViewState() const {
    if (!page_) {
        return {};
    }
    QJsonArray widths;
    for (int column = 0; column < listModel_->columnCount(); ++column) widths.append(files_->columnWidth(column));
    return {{"folder", folder_}, {"search", search_->text()}, {"columns", widths}};
}

void AssetsModule::RestoreViewState(const QJsonObject& state) {
    (void)Widget();
    search_->setText(state["search"].toString());
    OpenFolder(state["folder"].toString());
    const QJsonArray widths = state["columns"].toArray();
    for (int column = 0; column < widths.size(); ++column) {
        if (widths[column].toInt() > 0) files_->setColumnWidth(column, widths[column].toInt());
    }
}

std::optional<QJsonObject> AssetsModule::CaptureNavigationBookmark() const {
    if (!page_ || !files_ || !tree_ || !search_) return {};
    const auto paths = SelectedPaths();
    if (paths.size() > 128) return {};
    QJsonArray selected;
    for (const auto& path : paths) selected.append(Normalized(path));
    const auto current = files_->currentIndex().siblingAtColumn(0);
    const auto currentPath = current.isValid() && current.data(kKindRole).toInt() == 1
        ? Normalized(current.data(kPathRole).toString()) : paths.isEmpty() ? QString{} : Normalized(paths.front());
    QJsonObject bookmark{{"schema", 1}, {"kind", "assets-view"}, {"folder", Normalized(folder_)},
        {"selected", selected}, {"current_path", currentPath}, {"filter", search_->text()},
        {"list_v", files_->verticalScrollBar()->value()}, {"list_h", files_->horizontalScrollBar()->value()},
        {"tree_v", tree_->verticalScrollBar()->value()}, {"tree_h", tree_->horizontalScrollBar()->value()}};
    if (!NavigationBookmarkDetail::Text(bookmark, "folder") || !NavigationBookmarkDetail::Text(bookmark, "current_path")
        || !NavigationBookmarkDetail::Text(bookmark, "filter", 512)
        || QJsonDocument(bookmark).toJson(QJsonDocument::Compact).size() > 16 * 1024) return {};
    return bookmark;
}

Shell::NavigationRestoreResult AssetsModule::RestoreNavigationBookmark(const QJsonObject& bookmark) {
    using Shell::NavigationRestoreResult;
    namespace Nav = NavigationBookmarkDetail;
    const auto folder = Nav::Text(bookmark, "folder"), filter = Nav::Text(bookmark, "filter", 512);
    const auto current = Nav::Text(bookmark, "current_path");
    const auto listV = Nav::Scroll(bookmark, "list_v"), listH = Nav::Scroll(bookmark, "list_h");
    const auto treeV = Nav::Scroll(bookmark, "tree_v"), treeH = Nav::Scroll(bookmark, "tree_h");
    if (!Nav::Header(bookmark, "assets-view") || !folder || !filter || !current || !listV || !listH || !treeV || !treeH
        || !bookmark.value("selected").isArray() || bookmark.value("selected").toArray().size() > 128
        || Normalized(*folder) != *folder || Normalized(*current) != *current
        || QJsonDocument(bookmark).toJson(QJsonDocument::Compact).size() > 16 * 1024) return NavigationRestoreResult::Invalid;
    // 只核当前有效成员目录；不读载荷或作者磁盘，不调用会退回上级的 OpenFolder 做验证。
    const auto entries = project_.AssetEntries();
    QSet<QString> folders{QString{}};
    const auto addFolder = [&](QString path) {
        while (!path.isEmpty()) { folders.insert(path.toLower()); path = ParentFolder(path); }
    };
    for (const auto& entry : entries) addFolder(ParentFolder(Normalized(entry.path)));
    for (const auto& empty : emptyFolders_) addFolder(Normalized(empty));
    if (!folders.contains(folder->toLower())) return NavigationRestoreResult::Invalid;
    const auto visible = [&](const QString& path) {
        const auto query = filter->trimmed();
        return query.isEmpty() ? SameFolder(ParentFolder(path), *folder) : path.contains(query, Qt::CaseInsensitive);
    };
    const auto validAsset = [&](const QString& path) {
        std::size_t count = 0;
        for (const auto& entry : entries) if (SameFolder(entry.path, path)) {
            if (entry.ambiguous) return false;
            ++count;
        }
        return !path.isEmpty() && Normalized(path) == path && count == 1 && visible(path);
    };
    QSet<QString> keys;
    for (const auto value : bookmark.value("selected").toArray()) {
        const auto path = Nav::Text(QJsonObject{{"path", value}}, "path");
        if (!path || !validAsset(*path) || keys.contains(path->toLower())) return NavigationRestoreResult::Invalid;
        keys.insert(path->toLower());
    }
    if (!current->isEmpty() && !validAsset(*current)) return NavigationRestoreResult::Invalid;
    // 所有目标通过检查后才修改视图；书签不修改资源队列、撤销栈或保存状态。
    (void)Widget();
    { const QSignalBlocker block(search_); search_->setText(*filter); }
    OpenFolder(*folder);
    files_->selectionModel()->clearSelection();
    QModelIndex currentIndex;
    for (int row = 0; row < listModel_->rowCount(); ++row) {
        const auto index = listModel_->index(row, 0);
        if (index.data(kKindRole).toInt() != 1) continue;
        const auto key = index.data(kPathRole).toString().toLower();
        if (keys.contains(key)) files_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!current->isEmpty() && SameFolder(index.data(kPathRole).toString(), *current)) currentIndex = index;
    }
    files_->selectionModel()->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
    files_->verticalScrollBar()->setValue(*listV); files_->horizontalScrollBar()->setValue(*listH);
    tree_->verticalScrollBar()->setValue(*treeV); tree_->horizontalScrollBar()->setValue(*treeH);
    return NavigationRestoreResult::Restored;
}

} // namespace War3Map::Modules

#include "ObjectModule.h"
#include "ObjectModuleShared.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectModels.h"
#include "Widgets/ObjectTreeBrowser.h"
#include "Widgets/ResponsiveToolBar.h"
#include "Icons/SvgIcons.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace War3Map::Modules {

using namespace ObjectData;
using namespace QtApp;
using namespace ObjectDetail;

ObjectModule::ObjectModule(QtProject::EditorProject& project)
    : project_(&project) {
    connect(&project, &QtProject::EditorProject::MapDocumentChanged, this, [this] {
        if (page_) UpdateObjectReferenceEntries();
    });
    connect(&project, &QtProject::EditorProject::ObjectDocumentsAboutToReplace, this, &ObjectModule::DetachDocuments);
    connect(&project, &QtProject::EditorProject::ObjectDocumentsReplaced, this, [this]() {
        if (page_) {
            ShowObjectType(currentObjectType_);
            UpdateObjectReferenceEntries();
            UpdateObjectFieldControls();
        }
        emit DocumentsChanged();
    });
}

ObjectModule::~ObjectModule() {
    delete newObjectPicker_.data();
    delete page_.data();
}
void ObjectModule::SetResourcePickerFactory(ObjectResourcePickerFactory factory) {
    resourcePickerFactory_ = std::move(factory);
    if (objectFieldDelegate_) objectFieldDelegate_->SetResourcePickerFactory(resourcePickerFactory_);
}

Shell::ModuleInfo ObjectModule::Info() const {
    return {"objects", tr("物体"), "lucide/box", QKeySequence(Qt::ALT | Qt::Key_2)};
}

QWidget* ObjectModule::Widget() {
    if (!page_) {
        BuildPage();
    }
    return page_;
}

void ObjectModule::AttachHost(Shell::ModuleHost* host) {
    host_ = host;
}

QUndoStack* ObjectModule::UndoStack() {
    // 原版目录页只读：撤销只作用于当前类别的地图覆盖，避免在看原版数据时撤掉看不见的修改。
    return EditingMapObjects() ? project_->objectUndoStacks_[static_cast<std::size_t>(currentObjectType_)] : nullptr;
}

void ObjectModule::ShowStatus(const QString& text, int timeoutMs) {
    if (host_ != nullptr) {
        host_->ShowStatusMessage(text, timeoutMs);
    }
}

bool ObjectModule::EditingMapObjects() const {
    return objectSourceTabs_ != nullptr && objectSourceTabs_->currentIndex() == 1;
}

QString ObjectModule::BreadcrumbText() const {
    static const QStringList kinds{tr("单位"), tr("技能"), tr("物品"), tr("魔法效果"), tr("科技"), tr("装饰物"), tr("可破坏物")};
    return tr("物体编辑器 › %1").arg(kinds.value(currentObjectType_, tr("未知")));
}

void ObjectModule::SetSearchText(const QString& text) {
    if (objectFilter_ != nullptr) objectFilter_->setFilterFixedString(text);
    if (standardObjectBrowser_ != nullptr) standardObjectBrowser_->SetSearchText(text);
    if (objectTree_ != nullptr) objectTree_->SetSearchText(text);
}

void ObjectModule::RefreshIcons() {
    if (standardObjectBrowser_ != nullptr) standardObjectBrowser_->RefreshIcons();
}

void ObjectModule::DetachDocuments() {
    RememberSelection();
    // 替换文档前解除模型引用，旧文档析构后不再被访问。
    if (objectFieldModel_ != nullptr) objectFieldModel_->SetDocument(nullptr, nullptr);
    if (objectListModel_ != nullptr) objectListModel_->SetDocument(nullptr);
}

void ObjectModule::ResetView() {
    if (newObjectPicker_) newObjectPicker_->hide();
    DetachDocuments();
    if (mapObjectSearch_ != nullptr) mapObjectSearch_->clear();
    lastSelectedObject_.fill({});
    currentObjectType_ = 0;
}

void ObjectModule::SelectFirstType() {
    (void)Widget();
    objectCategories_->setCurrentRow(0);
    ShowObjectType(0);
}

void ObjectModule::SelectType(int index) {
    (void)Widget();
    objectCategories_->setCurrentRow(std::clamp(index, 0, 6));
}

bool ObjectModule::HasLoadedContent() const {
    return objectListModel_ != nullptr && objectListModel_->rowCount() > 0 &&
        objectFieldModel_ != nullptr && objectFieldModel_->rowCount() > 0;
}

bool ObjectModule::Reveal(const Shell::NavigationTarget& target) {
    // 目标 ID 形如 "unit:hfoo" 或纯 rawcode（在当前类别中定位）。
    if (target.module != "objects") return false;
    if (target.extra.contains("source")) {
        const auto origin = target.extra.value("source");
        if (target.kind != QLatin1String("object") || origin.typeId() != QMetaType::QString
            || (origin.toString() != QLatin1String("map") && origin.toString() != QLatin1String("standard"))) return false;
        const int colon = target.id.indexOf(':');
        if (colon <= 0) return false;
        const auto kind = target.id.left(colon), rawcode = target.id.mid(colon + 1);
        int type = -1;
        for (int index = 0; index < 7; ++index)
            if (Kind(static_cast<ObjectType>(index)) == kind) type = index;
        if (type < 0 || rawcode.size() != 4 || !std::all_of(rawcode.begin(), rawcode.end(), [](QChar c) {
            return c.unicode() >= 32 && c.unicode() <= 126;
        })) return false;
        const auto objectType = static_cast<ObjectType>(type);
        const bool map = origin.toString() == QLatin1String("map");
        const auto* document = project_->objectDocuments_[static_cast<std::size_t>(type)].get();
        if (map ? !document || !document->FindObject({objectType, rawcode.toStdString()})
            : !project_->standardCatalog_ || !project_->standardCatalog_->Find(objectType, rawcode.toStdString())) return false;
        (void)Widget(); objectCategories_->setCurrentRow(type);
        if (currentObjectType_ != type) ShowObjectType(type);
        SetSearchText({});
        if (map) { SelectMapObject(type, rawcode); return true; }
        for (const auto* name : {"objectCatalogRaces", "objectCatalogCategories"})
            if (auto* combo = standardObjectBrowser_->findChild<QComboBox*>(name)) combo->setCurrentIndex(0);
        objectSourceTabs_->setCurrentIndex(0); standardObjectBrowser_->SetCurrentId(rawcode);
        const auto entry = standardObjectBrowser_->CurrentEntry();
        if (!entry || entry->id != rawcode || entry->kind != Kind(objectType)) return false;
        ShowStandardObject(*entry);
        return true;
    }
    (void)Widget();
    QString rawcode = target.id;
    int type = currentObjectType_;
    if (const int colon = rawcode.indexOf(':'); colon > 0) {
        const QString kind = rawcode.left(colon);
        rawcode = rawcode.mid(colon + 1);
        for (int index = 0; index < 7; ++index) {
            if (Kind(static_cast<ObjectType>(index)).compare(kind, Qt::CaseInsensitive) == 0) type = index;
        }
    }
    const auto* document = project_->objectDocuments_[static_cast<std::size_t>(type)].get();
    if (document != nullptr && document->FindObject({static_cast<ObjectType>(type), rawcode.toStdString()}) != nullptr) {
        SelectMapObject(type, rawcode);
        return true;
    }
    if (currentObjectType_ != type) {
        objectCategories_->setCurrentRow(type);
    }
    objectSourceTabs_->setCurrentIndex(0);
    standardObjectBrowser_->SetCurrentId(rawcode);
    return standardObjectBrowser_->CurrentEntry().has_value();
}

QJsonObject ObjectModule::SaveViewState() const {
    if (!page_) return {};
    QJsonObject state{{"type", currentObjectType_}, {"tab", objectSourceTabs_->currentIndex()},
        {"rawFields", showRawFields_->isChecked()}, {"showObjectIds", objectTree_->ShowIds()}};
    if (const auto entry = standardObjectBrowser_->CurrentEntry()) state.insert("standard", entry->id);
    return state;
}

void ObjectModule::RestoreViewState(const QJsonObject& state) {
    (void)Widget();
    const int type = std::clamp(state["type"].toInt(), 0, 6);
    objectCategories_->setCurrentRow(type);
    showRawFields_->setChecked(state["rawFields"].toBool());
    objectTree_->SetShowIds(state["showObjectIds"].toBool());
    objectSourceTabs_->setCurrentIndex(state["tab"].toInt());
    if (const QString standard = state["standard"].toString(); !standard.isEmpty()) {
        standardObjectBrowser_->SetCurrentId(standard);
    }
}

void ObjectModule::BuildPage() {
    auto* page = new QWidget;
    page_ = page;
    page->setObjectName("objectWorkspace");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* commandBar = new UiKit::ResponsiveToolBar(page);
    commandBar->setObjectName(QStringLiteral("objectCommandBar"));
    auto* toolbar = commandBar->ContentLayout();
    toolbar->setContentsMargins(12, 8, 12, 8);
    auto* title = new QLabel(tr("物体编辑器"));
    title->setObjectName("paneTitle");
    toolbar->addWidget(title);
    toolbar->addStretch();
    showRawFields_ = new QCheckBox(tr("显示原始字段"));
    showRawFields_->setObjectName("showRawFields");
    showRawFields_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_D));
    showRawFields_->setToolTip(tr("切换字段显示名 · Alt+D"));
    toolbar->addWidget(showRawFields_);
    auto* resourceButton = new QPushButton(tr("资源目录…"));
    resourceButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/folder-open")));
    toolbar->addWidget(resourceButton);
    connect(resourceButton, &QPushButton::clicked, this, &ObjectModule::GameRootRequested);
    auto* batchButton = new QPushButton(tr("批量修改…"));
    batchButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/layers")));
    batchButton->setObjectName("batchEditObjectsButton");
    batchButton->setToolTip(tr("选择同类地图对象，共同修改一个字段"));
    toolbar->addWidget(batchButton);
    connect(batchButton, &QPushButton::clicked, this, &ObjectModule::BatchEditObjects);
    createObjectButton_ = new QPushButton(tr("新建自定义物体"));
    createObjectButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    createObjectButton_->setProperty("toolbarPriority", 100);
    createObjectButton_->setObjectName("createCustomObjectButton");
    toolbar->addWidget(createObjectButton_);
    connect(createObjectButton_, &QPushButton::clicked, this, &ObjectModule::CreateNewCustomObject);
    layout->addWidget(commandBar);

    objectCategories_ = new QListWidget;
    objectCategories_->setObjectName("objectKinds");
    objectCategories_->addItems({tr("单位"), tr("技能"), tr("物品"), tr("魔法效果"),
        tr("科技"), tr("装饰物"), tr("可破坏物")});
    objectCategories_->setCurrentRow(0);
    objectCategories_->setFlow(QListView::LeftToRight);
    objectCategories_->setWrapping(false);
    objectCategories_->setFixedHeight(42);
    objectCategories_->setFrameShape(QFrame::NoFrame);
    objectCategories_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    objectCategories_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int i = 0; i < objectCategories_->count(); ++i)
        objectCategories_->item(i)->setSizeHint({92, 38});
    layout->addWidget(objectCategories_);

    auto* splitter = new QSplitter;
    splitter->setHandleWidth(4);
    splitter->setChildrenCollapsible(false);

    objectSourceTabs_ = new QTabWidget;
    objectSourceTabs_->setDocumentMode(true);
    objectSourceTabs_->setMinimumWidth(285);
    standardObjectBrowser_ = new ObjectCatalogWidget(project_->iconProvider_.get());
    standardObjectBrowser_->SetDirectoryVisible(false);
    standardObjectBrowser_->SetKindFilter(Kind(ObjectType::Unit), true);
    standardObjectBrowser_->SetCatalogStatus(tr("未连接 Warcraft III 资源"));
    objectSourceTabs_->addTab(standardObjectBrowser_, tr("标准对象"));

    objectListView_ = new QTableView;
    objectListView_->setObjectName("objectMapOverrides");
    objectListModel_ = new ObjectListModel(objectListView_);
    objectFilter_ = new QSortFilterProxyModel(objectListView_);
    objectFilter_->setSourceModel(objectListModel_);
    objectFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    objectFilter_->setFilterKeyColumn(-1);
    objectListView_->setModel(objectFilter_);
    objectListView_->setIconSize({32, 32});
    objectListView_->setSortingEnabled(true);
    ConfigureTable(objectListView_);
    auto* mapObjectPage = new QWidget;
    auto* mapObjectLayout = new QVBoxLayout(mapObjectPage);
    mapObjectLayout->setContentsMargins(8, 10, 8, 8);
    mapObjectLayout->setSpacing(8);
    mapObjectSearch_ = new QLineEdit;
    mapObjectSearch_->setPlaceholderText(tr("搜索地图对象"));
    mapObjectSearch_->setClearButtonEnabled(true);
    mapObjectLayout->addWidget(mapObjectSearch_);
    mapObjectLayout->addWidget(objectListView_, 1);
    connect(mapObjectSearch_, &QLineEdit::textChanged, objectFilter_, &QSortFilterProxyModel::setFilterFixedString);
    objectSourceTabs_->addTab(mapObjectPage, tr("自定义"));
    // 左侧是原版风格的目录树；上面的“标准对象 / 自定义”两页保留为隐藏的选择状态，
    // 字段显示、创建、复制与撤销仍走原流程。
    objectSourceTabs_->setParent(page);
    objectSourceTabs_->hide();
    objectTree_ = new ObjectTreeBrowser(project_->iconProvider_.get());
    objectTree_->setMinimumWidth(285);
    splitter->addWidget(objectTree_);
    connect(objectTree_, &ObjectTreeBrowser::ObjectSelected, this, &ObjectModule::OnObjectTreeSelected);
    connect(objectTree_, &ObjectTreeBrowser::ContextMenuRequested, this, &ObjectModule::ShowObjectTreeMenu);

    auto* properties = new QWidget;
    properties->setObjectName("objectInspector");
    properties->setMinimumWidth(470);
    auto* propertyLayout = new QVBoxLayout(properties);
    propertyLayout->setContentsMargins(18, 12, 18, 8);
    propertyLayout->setSpacing(10);
    auto* previewPanel = new QWidget;
    previewPanel->setObjectName("inspectorHeader");
    auto* preview = new QHBoxLayout(previewPanel);
    preview->setContentsMargins(0, 2, 0, 12);
    objectPreviewIcon_ = new QLabel;
    objectPreviewIcon_->setFixedSize(64, 64);
    objectPreviewIcon_->setAlignment(Qt::AlignCenter);
    objectPreviewIcon_->setObjectName("objectPreviewIcon");
    preview->addWidget(objectPreviewIcon_);
    auto* previewText = new QVBoxLayout;
    objectPreviewName_ = new QLabel(tr("选择对象"));
    objectPreviewName_->setTextFormat(Qt::PlainText);
    objectPreviewName_->setObjectName("workspaceTitle");
    previewText->addWidget(objectPreviewName_);
    objectPreviewSource_ = new QLabel;
    objectPreviewSource_->setTextFormat(Qt::PlainText);
    objectPreviewSource_->setObjectName("subtitle");
    objectPreviewSource_->setWordWrap(true);
    previewText->addWidget(objectPreviewSource_);
    preview->addLayout(previewText, 1);
    editStandardObject_ = new QPushButton(tr("修改此对象"));
    editStandardObject_->setObjectName("editStandardObjectButton");
    editStandardObject_->setEnabled(false);
    preview->addWidget(editStandardObject_, 0, Qt::AlignTop);
    connect(editStandardObject_, &QPushButton::clicked, this, &ObjectModule::EditSelectedStandardObject);
    propertyLayout->addWidget(previewPanel);
    auto* fieldToolbar = new QHBoxLayout;
    fieldToolbar->setSpacing(8);
    objectFieldSearch_ = new QLineEdit;
    objectFieldSearch_->setPlaceholderText(tr("搜索字段 / 值"));
    objectFieldSearch_->setClearButtonEnabled(true);
    fieldToolbar->addWidget(objectFieldSearch_, 1);
    objectFieldCategory_ = new QComboBox;
    objectFieldCategory_->setObjectName("fieldCategory");
    objectFieldCategory_->addItem(tr("全部分类"), QString{});
    objectFieldCategory_->setMinimumWidth(105);
    fieldToolbar->addWidget(objectFieldCategory_);
    propertyLayout->addLayout(fieldToolbar);
    auto* fieldActions = new QHBoxLayout;
    objectFieldCount_ = new QLabel(tr("选择对象"));
    objectFieldCount_->setObjectName("paneHint");
    fieldActions->addWidget(objectFieldCount_);
    fieldActions->addStretch();
    undoObjectEdit_ = new QPushButton(tr("撤销"));
    redoObjectEdit_ = new QPushButton(tr("重做"));
    resetObjectField_ = new QPushButton(tr("重置字段"));
    for (auto* button : {undoObjectEdit_, redoObjectEdit_, resetObjectField_}) {
        button->setEnabled(false);
        fieldActions->addWidget(button);
    }
    undoObjectEdit_->setToolTip(tr("撤销物体编辑 · Ctrl+Z"));
    redoObjectEdit_->setToolTip(tr("重做物体编辑 · Ctrl+Y"));
    resetObjectField_->setToolTip(tr("移除当前字段覆盖，恢复继承值"));
    propertyLayout->addLayout(fieldActions);

    objectDetailPages_ = new QStackedWidget;
    auto* standardFields = new QTableView;
    standardFields->setObjectName("standardObjectFields");
    standardFieldModel_ = new QStandardItemModel(0, 2, standardFields);
    standardFieldModel_->setHorizontalHeaderLabels({tr("字段"), tr("默认值")});
    standardFieldFilter_ = new StandardFieldFilter(standardFields);
    standardFieldFilter_->setSourceModel(standardFieldModel_);
    standardFieldFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    standardFieldFilter_->setFilterKeyColumn(-1);
    standardFields->setModel(standardFieldFilter_);
    standardFields->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 保持目录浏览默认不进入编辑状态；双击可编辑字段才打开就地编辑器。
    // 只有值真正提交后才在当前地图建立覆盖，Esc 不生成对象或撤销项。
    connect(standardFields, &QTableView::doubleClicked, this, [this, standardFields](const QModelIndex& index) {
        if (index.column() != 1) return;
        if (index.flags() & Qt::ItemIsEditable) standardFields->edit(index);
        else BeginStandardReferenceEdit(index);
    });
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* activate = new QShortcut(QKeySequence(key), standardFields);
        activate->setContext(Qt::WidgetShortcut);
        connect(activate, &QShortcut::activated, this, [this, standardFields] {
            const auto index = standardFields->currentIndex().siblingAtColumn(1);
            if (!index.isValid()) return;
            standardFields->setCurrentIndex(index);
            if (index.flags() & Qt::ItemIsEditable) standardFields->edit(index);
            else BeginStandardReferenceEdit(index);
        });
    }
    connect(standardFieldModel_, &QStandardItemModel::itemChanged,
        this, &ObjectModule::OnStandardFieldChanged);
    ConfigureTable(standardFields);
    // 字段名仍可拖动调整；较窄宿主优先给可编辑值留出阅读空间。
    standardFields->setColumnWidth(0, 240);
    objectDetailPages_->addWidget(standardFields);

    objectFieldView_ = new QTableView;
    objectFieldView_->setObjectName("objectProperties");
    objectFieldModel_ = new ObjectFieldModel(objectFieldView_);
    objectFieldModel_->SetUndoStack(project_->objectUndoStacks_[0]);
    objectFieldModel_->SetEditCallback([this]() { MarkCurrentObjectDocumentDirty(); });
    objectFieldView_->setModel(objectFieldModel_);
    objectFieldDelegate_ = new ObjectFieldDelegate(objectFieldView_);
    objectFieldDelegate_->SetResourcePickerFactory(resourcePickerFactory_);
    objectFieldView_->setItemDelegate(objectFieldDelegate_);
    ConfigureTable(objectFieldView_);
    for (int column : {1, 2, 4}) objectFieldView_->hideColumn(column);
    objectFieldView_->setColumnWidth(0, 240);
    objectDetailPages_->addWidget(objectFieldView_);
    propertyLayout->addWidget(objectDetailPages_, 1);
    splitter->addWidget(properties);
    splitter->setSizes({380, 900});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(standardObjectBrowser_, &ObjectCatalogWidget::EntrySelected,
        this, &ObjectModule::ShowStandardObject);
    connect(objectCategories_, &QListWidget::currentRowChanged, this, &ObjectModule::ShowObjectType);
    connect(objectSourceTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        objectDetailPages_->setCurrentIndex(index);
        if (host_ != nullptr) host_->UndoStackChanged("objects");
        if (index == 1) ShowSelectedObject();
        else if (const auto entry = standardObjectBrowser_->CurrentEntry()) ShowStandardObject(*entry);
        RebuildObjectFieldCategories();
        UpdateObjectFieldControls();
    });
    connect(objectListView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, [this]() { ShowSelectedObject(); });
    connect(objectFieldSearch_, &QLineEdit::textChanged, this, [this](const QString& text) {
        standardFieldFilter_->setFilterFixedString(text);
        objectFieldModel_->SetFieldFilter(text);
        UpdateObjectFieldControls();
    });
    connect(showRawFields_, &QCheckBox::toggled, this, &ObjectModule::RefreshObjectFieldPresentation);
    connect(objectFieldCategory_, &QComboBox::currentIndexChanged, this, [this]() {
        const auto category = objectFieldCategory_->currentData().toString();
        static_cast<StandardFieldFilter*>(standardFieldFilter_)->SetCategory(category);
        objectFieldModel_->SetCategoryFilter(category);
        UpdateObjectFieldControls();
    });
    connect(undoObjectEdit_, &QPushButton::clicked, this, [this]() { project_->objectUndoStacks_[currentObjectType_]->undo(); });
    connect(redoObjectEdit_, &QPushButton::clicked, this, [this]() { project_->objectUndoStacks_[currentObjectType_]->redo(); });
    connect(resetObjectField_, &QPushButton::clicked, this, [this]() {
        objectFieldModel_->ResetField(objectFieldView_->currentIndex().row());
        UpdateObjectFieldControls();
    });
    connect(objectFieldView_->selectionModel(), &QItemSelectionModel::currentChanged,
        this, [this]() { UpdateObjectFieldControls(); });
    connect(objectFieldModel_, &QAbstractItemModel::modelReset,
        this, [this]() { UpdateObjectFieldControls(); });
    for (auto* stack : project_->objectUndoStacks_)
        connect(stack, &QUndoStack::indexChanged, this, [this]() { UpdateObjectFieldControls(); });
    objectListView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(objectListView_, &QTableView::customContextMenuRequested,
        this, &ObjectModule::ShowObjectListContextMenu);
    objectListModel_->SetPresentationResolver([this](const ObjectRecord& record) {
        if (showRawFields_->isChecked()) return std::make_pair(U8(record.key.rawcode), QIcon{});
        for (const auto& entry : objectViewEntries_) {
            if (entry.id == U8(record.key.rawcode) && entry.kind == Kind(record.key.type))
                return std::make_pair(entry.name, project_->iconProvider_->IconForTexture(entry.iconPath));
        }
        const auto shown = ResolveProjectObjectPresentation(*project_, record.key);
        return std::make_pair(shown.name, project_->iconProvider_->IconForTexture(shown.iconPath));
    });
}


void ObjectModule::RememberSelection() {
    // 切换分类和解除文档引用前都按稳定 rawcode 记忆，不保留会随模型重置失效的行号。
    if (currentObjectType_ >= 0 &&
        currentObjectType_ < static_cast<int>(lastSelectedObject_.size()) &&
        objectListView_ != nullptr && objectFilter_ != nullptr && objectListModel_ != nullptr) {
        const QModelIndex proxyIndex = objectListView_->currentIndex();
        if (proxyIndex.isValid()) {
            const QModelIndex sourceIndex = objectFilter_->mapToSource(proxyIndex);
            const auto* obj = objectListModel_->ObjectAt(sourceIndex.row());
            if (obj != nullptr) {
                lastSelectedObject_[static_cast<std::size_t>(currentObjectType_)] =
                    QString::fromStdString(obj->key.rawcode);
            }
        }
    }
}

void ObjectModule::ShowObjectType(int index) {
    if (newObjectPicker_) newObjectPicker_->hide();
    if (index < 0 || index >= static_cast<int>(project_->objectDocuments_.size())) {
        return;
    }
    RememberSelection();
    currentObjectType_ = index;
    objectFieldModel_->SetUndoStack(project_->objectUndoStacks_[static_cast<std::size_t>(index)]);
    if (host_ != nullptr) host_->UndoStackChanged("objects");
    emit ContextChanged();
    if (standardObjectBrowser_) {
        standardObjectBrowser_->SetKindFilter(Kind(static_cast<ObjectType>(index)), true);
        standardFieldModel_->removeRows(0, standardFieldModel_->rowCount());
        objectPreviewName_->setText(tr("选择对象"));
        objectPreviewSource_->clear();
        objectPreviewIcon_->clear();
    }
    objectListModel_->SetDocument(project_->objectDocuments_[static_cast<std::size_t>(index)].get());
    objectFieldModel_->SetDocument(nullptr, nullptr);
    if (objectFilter_ != nullptr) {
        objectFilter_->sort(0);
    }
    // 恢复上次选中的物体，找不到则选第一行。
    if (objectListView_ != nullptr && objectFilter_ != nullptr &&
        objectFilter_->rowCount() > 0) {
        int targetRow = 0;
        const QString& saved = lastSelectedObject_[static_cast<std::size_t>(index)];
        if (!saved.isEmpty()) {
            for (int row = 0; row < objectFilter_->rowCount(); ++row) {
                const QModelIndex proxyIdx = objectFilter_->index(row, 0);
                if (objectFilter_->data(proxyIdx, Qt::UserRole).toString() == saved) {
                    targetRow = row;
                    break;
                }
            }
        }
        objectListView_->selectRow(targetRow);
        objectListView_->setCurrentIndex(objectFilter_->index(targetRow, 0));
        ShowSelectedObject();
    }
    if (objectSourceTabs_->currentIndex() == 0) {
        if (const auto entry = standardObjectBrowser_->CurrentEntry()) ShowStandardObject(*entry);
    }
    RebuildObjectTree();
    UpdateObjectFieldControls();
}


void ObjectModule::ShowSelectedObject() {
    if (currentObjectType_ < 0 ||
        currentObjectType_ >= static_cast<int>(project_->objectDocuments_.size()) ||
        objectListView_ == nullptr || objectFilter_ == nullptr) {
        return;
    }
    const QModelIndex proxyIndex = objectListView_->currentIndex();
    if (!proxyIndex.isValid()) {
        objectFieldModel_->SetDocument(nullptr, nullptr);
        if (objectSourceTabs_->currentIndex() == 1) {
            objectPreviewName_->setText(tr("选择地图对象"));
            objectPreviewSource_->clear();
            objectPreviewIcon_->clear();
        }
        UpdateObjectFieldControls();
        return;
    }
    const QModelIndex sourceIndex = objectFilter_->mapToSource(proxyIndex);
    const auto* object = objectListModel_->ObjectAt(sourceIndex.row());
    auto* document = project_->objectDocuments_[static_cast<std::size_t>(currentObjectType_)].get();
    objectFieldModel_->SetDocument(
        document,
        object == nullptr ? nullptr : &object->key);
    if (object != nullptr && objectSourceTabs_ && objectSourceTabs_->currentIndex() == 1) {
        for (const auto& entry : objectViewEntries_) {
            if (entry.id == U8(object->key.rawcode) && entry.kind == Kind(object->key.type)) {
                objectPreviewName_->setText(showRawFields_->isChecked() ? entry.id : entry.name);
                objectPreviewSource_->setText(tr("%1 · %2").arg(entry.id, entry.description));
                objectPreviewSource_->setToolTip(entry.details.join(QChar('\n')));
                objectPreviewIcon_->setPixmap(project_->iconProvider_->IconForTexture(entry.iconPath).pixmap(64, 64));
                break;
            }
        }
    }
    RebuildObjectFieldCategories();
    UpdateObjectFieldControls();
    SyncObjectTreeSelection();
}


void ObjectModule::MarkCurrentObjectDocumentDirty() {
    if (currentObjectType_ < 0 ||
        currentObjectType_ >= static_cast<int>(project_->objectDirty_.size())) {
        return;
    }
    project_->objectDirty_[static_cast<std::size_t>(currentObjectType_)] = true;
    project_->ObserveObjectEdit(currentObjectType_);
    emit DocumentEdited(currentObjectType_);
    UpdateObjectReferenceEntries();
    ShowStatus(tr("存在未导出的物编修改 · 源目录仍保持只读"));
}


void ObjectModule::ShowObjectListContextMenu(const QPoint& pos) {
    const QModelIndex underCursor = objectListView_->indexAt(pos);
    if (underCursor.isValid()) objectListView_->setCurrentIndex(underCursor);
    const QModelIndex selected = objectListView_->currentIndex();
    const auto* object = selected.isValid() && objectFilter_
        ? objectListModel_->ObjectAt(objectFilter_->mapToSource(selected).row()) : nullptr;
    const bool custom = object != nullptr && object->parentRawcode.has_value();
    QMenu menu(page_);
    auto* referencesAction = menu.addAction(tr("查看引用（物体管理）"));
    referencesAction->setEnabled(object != nullptr && host_ != nullptr);
    menu.addSeparator();
    auto* newAction = menu.addAction(tr("新建自定义物体"));
    auto* cloneAction = menu.addAction(tr("复制自定义物体"));
    cloneAction->setEnabled(custom);
    menu.addSeparator();
    auto* deleteAction = menu.addAction(tr("删除选中物体"));

    deleteAction->setEnabled(false);
    deleteAction->setToolTip(tr("尚未接入完整引用审计，删除暂不可用。"));
    menu.setToolTipsVisible(true);

    const QAction* chosen = menu.exec(objectListView_->viewport()->mapToGlobal(pos));
    if (chosen == referencesAction && object != nullptr) {
        host_->Navigate({"manager", "object", Kind(object->key.type) + ':' + QString::fromStdString(object->key.rawcode), {}});
    } else if (chosen == newAction) {
        CreateNewCustomObject();
    } else if (chosen == cloneAction) {
        DuplicateSelectedCustomObject();
    } else if (chosen == deleteAction) {
        DeleteSelectedObject();
    }
}


void ObjectModule::DeleteSelectedObject() {
    // P1：接通真实跨模块引用审计后再恢复删除，不能以手造报告换取删除令牌。
    project_->AddDiagnostic(tr("警告"), tr("物体数据"), tr("OBJECT_DELETE_AUDIT_UNAVAILABLE"),
        tr("物体删除尚未接入完整引用审计，当前入口已停用。"));
    ShowStatus(tr("删除不可用 · 等待引用审计"), 5000);
}

} // namespace War3Map::Modules

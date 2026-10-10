#include "ObjectModule.h"
#include "ObjectModuleShared.h"
#include "Widgets/RoundedPopups.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectModels.h"
#include "ObjectNames.h"
#include "ObjectColourPreviewSource.h"
#include "ObjectViewEntries.h"
#include "Widgets/ObjectTreeBrowser.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

// 字段展示：原版目录字段表、地图覆盖字段、分类筛选与控件状态。
namespace War3Map::Modules {

using namespace ObjectData;
using namespace QtApp;
using namespace ObjectDetail;

namespace {
constexpr int kStandardDisplayRole = Qt::UserRole + 42;
constexpr int kStandardObjectRole = Qt::UserRole + 43;
constexpr int kStandardEditableRole = Qt::UserRole + 44;
constexpr int kStandardReferenceRole = Qt::UserRole + 45;
constexpr int kStandardRawValueRole = Qt::UserRole + 46;
}


void ObjectModule::RefreshStandardCatalog() {
    auto mounted = War3Map::Game::GameDataSource::Mount(project_->war3GameRoot_);
    if (!mounted.Succeeded() || !mounted.value) return;
    // 名称与字段显示走统一字符串表（YDWE 覆盖层 → 游戏 → 英文原版）；载入期间持有共享指针。
    const auto strings = project_->CurrentWorldEditStrings();
    CatalogLoadOptions catalogOptions;
    catalogOptions.strings = strings.get();
    auto loaded = StandardObjectCatalog::Load(*mounted.value, catalogOptions);
    for (const auto& diagnostic : loaded.diagnostics) {
        project_->AddDiagnostic(tr("信息"), tr("物体目录"), U8(diagnostic.code), U8(diagnostic.message));
    }
    if (!loaded.Succeeded() || !loaded.value) {
        objectFieldModel_->SetContext(nullptr, nullptr);
        project_->standardCatalog_.reset();
        project_->objectFieldSchema_.reset();
        UpdateObjectReferenceEntries();
        standardObjectBrowser_->SetCatalogStatus(tr("无法读取原版物体目录"));
        emit DocumentsChanged();
        return;
    }
    objectFieldModel_->SetContext(nullptr, nullptr);
    project_->standardCatalog_ = std::make_unique<StandardObjectCatalog>(std::move(*loaded.value));
    ObjectFieldSchemaOptions schemaOptions;
    schemaOptions.strings = strings.get();
    auto schema = ObjectFieldSchema::Load(*mounted.value, schemaOptions);
    project_->objectFieldSchema_.reset();
    for (const auto& diagnostic : schema.diagnostics)
        project_->AddDiagnostic(tr("信息"), tr("字段定义"), U8(diagnostic.code), U8(diagnostic.message));
    if (schema.Succeeded() && schema.value)
        project_->objectFieldSchema_ = std::make_unique<ObjectFieldSchema>(std::move(*schema.value));
    objectFieldModel_->SetContext(project_->standardCatalog_.get(), project_->objectFieldSchema_.get());
    UpdateObjectReferenceEntries();
    standardObjectBrowser_->SetCatalogStatus(tr("Warcraft III · %1 个标准对象")
        .arg(project_->standardCatalog_->Entries().size()));
    ShowStatus(tr("已读取 %1 个原版对象 · 游戏资源只读")
        .arg(project_->standardCatalog_->Entries().size()));
    emit DocumentsChanged();
}


void ObjectModule::UpdateObjectReferenceEntries() {
    QVector<ObjectViewEntry> standardEntries;
    objectViewEntries_ = QtApp::BuildObjectViewEntries(*project_, &standardEntries);
    if (objectFieldModel_) {
        const QPointer<QtProject::EditorProject> project(project_);
        const auto previewFactory = [project] {
            return project ? CaptureObjectColourPreviewSource(*project)
                : UiKit::WarcraftTextSourceResolver{};
        };
        objectFieldModel_->SetColourPreviewFactory(previewFactory);
        if (!objectFieldModel_->property("colourPreviewSourceWatch").toBool()) {
            objectFieldModel_->setProperty("colourPreviewSourceWatch", true);
            const QPointer<ObjectFieldModel> model(objectFieldModel_);
            connect(project_, &QtProject::EditorProject::SourceStateChanged, objectFieldModel_, [model, previewFactory] {
                if (model) model->SetColourPreviewFactory(previewFactory); // 仅通知已打开预览核修订，不建立新快照。
            });
        }
        QHash<QString, QString> names;
        for (const auto& entry : objectViewEntries_) names.insert(entry.kind + QChar(':') + entry.id, entry.name);
        objectFieldModel_->SetValuePresentationResolver([names = std::move(names), project = project_]
            (const ObjectFieldDefinition* definition, const QString& value) {
            if (Services::StringReferenceId(value.toStdString()))
                return U8(ResolveProjectName(*project, value.toStdString()).displayName);
            if (definition && !definition->referenceTypes.empty() && !value.isEmpty() && value != QStringLiteral("_")) {
                QStringList labels;
                for (const auto& id : value.split(',', Qt::KeepEmptyParts)) {
                    QString label;
                    for (const auto type : definition->referenceTypes) {
                        const auto found = names.constFind(Kind(type) + QChar(':') + id.trimmed());
                        if (found != names.cend()) { label = *found; break; }
                    }
                    labels.push_back(label.isEmpty() ? id : label);
                }
                return labels.join(QStringLiteral("、"));
            }
            if (definition) for (const auto& choice : definition->choices)
                if (U8(choice.value) == value) return U8(choice.displayName);
            return value;
        });
    }
    if (standardObjectBrowser_) standardObjectBrowser_->SetEntries(std::move(standardEntries));
    if (objectFieldDelegate_) objectFieldDelegate_->SetReferenceContext(project_->iconProvider_.get(), objectViewEntries_);
    if (objectListModel_) objectListModel_->RefreshPresentation();
    if (objectSourceTabs_ && objectSourceTabs_->currentIndex() == 1) ShowSelectedObject();
    RebuildObjectTree();
}


void ObjectModule::ShowStandardObject(const ObjectViewEntry& entry) {
    if (!project_->standardCatalog_ || !standardFieldModel_ || objectSourceTabs_->currentIndex() != 0) return;
    const auto* object = project_->standardCatalog_->Find(static_cast<ObjectType>(currentObjectType_), entry.id.toStdString());
    if (!object) return;
    objectPreviewName_->setText(showRawFields_->isChecked() ? entry.id : entry.name);
    objectPreviewSource_->setText(tr("%1 · 标准对象").arg(entry.id));
    objectPreviewSource_->setToolTip(entry.iconPath);
    const QIcon icon = project_->iconProvider_->IconForTexture(entry.iconPath);
    objectPreviewIcon_->setPixmap(icon.pixmap(64, 64));
    if (icon.isNull()) objectPreviewIcon_->setText(tr("无图标"));
    standardFieldModel_->removeRows(0, standardFieldModel_->rowCount());
    const bool raw = showRawFields_->isChecked();
    standardFieldModel_->setHorizontalHeaderLabels({raw ? tr("原始字段") : tr("字段"), tr("默认值")});
    ObjectFieldContext context;
    context.rawcode = object->key.rawcode;
    context.baseCode = std::string(object->Field("code"));
    context.building = object->Field("isbldg") == "1";
    context.hero = !object->key.rawcode.empty() && object->key.rawcode.front() >= 'A' && object->key.rawcode.front() <= 'Z';
    context.item = object->key.type == ObjectType::Item;
    std::vector<std::pair<std::string, std::string>> fields(object->fields.begin(), object->fields.end());
    struct DisplayField {
        QString key, label, value, rawValue, category, tip;
        QIcon icon;
        bool editable = false;
        bool reference = false;
    };
    QVector<DisplayField> display;
    for (const auto& [key, value] : fields) {
        const auto match = project_->objectFieldSchema_ ? project_->objectFieldSchema_->MatchSourceField(object->key.type, key, context) : ObjectFieldMatch{};
        const auto* definition = match.definition;
        // 普通模式显示原版编辑字段；引擎内部列仍可在原始模式中检查。
        if (project_->objectFieldSchema_ && !definition && !raw) continue;
        QString category = definition ? U8(definition->categoryDisplayName) : tr("其他");
        if (category.isEmpty()) category = tr("其他");
        QString label = raw || !definition ? U8(key) : U8(definition->displayName);
        if (label.isEmpty()) label = U8(key);
        if (!raw && definition) {
            if (definition->repeat > 0 && match.modification.level)
                label += tr(" · 等级 %1").arg(*match.modification.level);
            if (match.modification.variation)
                label += tr(" · 变体 %1").arg(*match.modification.variation);
            label = category + QStringLiteral("  /  ") + label;
        }
        QString shownValue = U8(value);
        QIcon referenceIcon;
        if (!raw && definition) {
            shownValue = objectFieldModel_->PresentedValue(definition, shownValue);
            if (definition->valueType == "bool") {
                if (value == "1") shownValue = tr("是");
                else if (value == "0") shownValue = tr("否");
            }
            for (const auto& choice : definition->choices)
                if (choice.value == value) { shownValue = U8(choice.displayName); break; }
            if (!definition->referenceTypes.empty() && !value.empty() && value != "_") {
                const auto ids = U8(value).split(',', Qt::KeepEmptyParts);
                for (const auto& id : ids) {
                    for (const auto type : definition->referenceTypes) {
                        const auto* target = project_->standardCatalog_->Find(type, id.trimmed().toStdString());
                        if (target) {
                            if (referenceIcon.isNull()) referenceIcon = project_->iconProvider_->IconForTexture(U8(target->iconPath));
                            break;
                        }
                    }
                }
            }
        }
        QString tip = tr("原始字段：%1\n原始值：%2").arg(U8(key), U8(value));
        if (definition) tip += tr("\n字段 ID：%1\n类型：%2\n来源：%3")
            .arg(U8(definition->fieldId), U8(definition->valueType), U8(definition->sourceTable));
        const bool editable = !project_->projectRoot_.empty() && definition && match.safeToEdit &&
            definition->nativeValueType && definition->referenceTypes.empty();
        const bool reference = !project_->projectRoot_.empty() && definition && match.safeToEdit &&
            definition->nativeValueType && !definition->referenceTypes.empty();
        if (editable || reference) tip += tr("\n双击修改：首次提交时自动建立地图覆盖");
        display.push_back({U8(key), label, shownValue, U8(value), category, tip, referenceIcon, editable, reference});
    }
    std::sort(display.begin(), display.end(), [](const auto& left, const auto& right) {
        return std::tie(left.category, left.label, left.key) < std::tie(right.category, right.label, right.key);
    });
    for (const auto& field : display) {
        auto* keyItem = new QStandardItem(field.label);
        keyItem->setEditable(false);
        keyItem->setData(field.category, kFieldCategoryRole);
        keyItem->setData(field.key, Qt::UserRole);
        keyItem->setToolTip(field.tip);
        auto* valueItem = new QStandardItem(field.icon, field.value);
        valueItem->setEditable(field.editable);
        valueItem->setData(field.value, kStandardDisplayRole);
        valueItem->setData(entry.id, kStandardObjectRole);
        valueItem->setData(field.editable, kStandardEditableRole);
        valueItem->setData(field.reference, kStandardReferenceRole);
        valueItem->setData(field.rawValue, kStandardRawValueRole);
        valueItem->setToolTip(field.tip);
        standardFieldModel_->appendRow({keyItem, valueItem});
    }
    RebuildObjectFieldCategories();
    UpdateObjectFieldControls();
    SyncObjectTreeSelection();
}

void ObjectModule::OnStandardFieldChanged(QStandardItem* item) {
    if (!item || item->column() != 1 || !item->data(kStandardEditableRole).toBool()) return;
    const QString previous = item->data(kStandardDisplayRole).toString();
    if (item->text() == previous) return;
    const QString submitted = item->text();
    const QString rawcode = item->data(kStandardObjectRole).toString();
    const auto* keyItem = standardFieldModel_->item(item->row(), 0);
    const QString sourceField = keyItem ? keyItem->data(Qt::UserRole).toString() : QString{};
    const int type = currentObjectType_;
    const QPersistentModelIndex pending(standardFieldModel_->indexFromItem(item));
    // 提交后会重建对象目录和属性模型，不能在 itemChanged 调用栈中销毁当前 item。
    QTimer::singleShot(0, this, [this, pending, previous, submitted, rawcode, sourceField, type] {
        const auto selected = standardObjectBrowser_->CurrentEntry();
        if (type == currentObjectType_ && selected && selected->id == rawcode &&
            objectSourceTabs_->currentIndex() == 0 &&
            CommitStandardFieldValue(rawcode, sourceField, submitted)) return;
        if (pending.isValid() && pending.data(kStandardObjectRole).toString() == rawcode) {
            const QSignalBlocker restoring(standardFieldModel_);
            standardFieldModel_->setData(pending, previous, Qt::EditRole);
        }
        ShowStatus(tr("字段未修改 · 检查取值、字段定义与原生物编基线"), 7000);
    });
}

void ObjectModule::BeginStandardReferenceEdit(const QModelIndex& proxyIndex) {
    if (proxyIndex.column() != 1 || objectSourceTabs_->currentIndex() != 0 ||
        !project_->standardCatalog_ || !project_->objectFieldSchema_) return;
    const QModelIndex source = standardFieldFilter_->mapToSource(proxyIndex);
    auto* valueItem = standardFieldModel_->item(source.row(), 1);
    auto* keyItem = standardFieldModel_->item(source.row(), 0);
    const auto selected = standardObjectBrowser_->CurrentEntry();
    if (!valueItem || !keyItem || !valueItem->data(kStandardReferenceRole).toBool() || !selected) return;
    const QString rawcode = valueItem->data(kStandardObjectRole).toString();
    if (selected->id != rawcode) return;
    const QString sourceField = keyItem->data(Qt::UserRole).toString();
    const auto type = static_cast<ObjectType>(currentObjectType_);
    const auto* base = project_->standardCatalog_->Find(type, rawcode.toStdString());
    if (!base) return;
    ObjectFieldContext context;
    context.rawcode = base->key.rawcode;
    context.baseCode = std::string(base->Field("code"));
    if (context.baseCode.empty()) context.baseCode = base->key.rawcode;
    context.hero = base->category == "hero";
    context.building = base->category == "building";
    context.item = type == ObjectType::Item;
    const auto match = project_->objectFieldSchema_->MatchSourceField(type, sourceField.toStdString(), context);
    if (!match.definition || !match.safeToEdit || match.definition->referenceTypes.empty()) return;

    QVector<ObjectViewEntry> candidates;
    for (const auto& entry : objectViewEntries_) {
        for (const auto accepted : match.definition->referenceTypes) {
            if (entry.kind == Kind(accepted)) {
                candidates.push_back(entry);
                break;
            }
        }
    }
    auto* view = qobject_cast<QTableView*>(objectDetailPages_->widget(0));
    if (!view || !view->screen()) return;
    const QString rawValue = valueItem->data(kStandardRawValueRole).toString();
    const int typeIndex = currentObjectType_;
    const auto stillSelected = [this, rawcode, typeIndex] {
        const auto now = standardObjectBrowser_->CurrentEntry();
        return currentObjectType_ == typeIndex && objectSourceTabs_->currentIndex() == 0 &&
            now && now->id == rawcode;
    };
    if (!match.definition->isList) {
        auto* picker = new ObjectPickerPopup(project_->iconProvider_.get(), view);
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->SetEntries(candidates);
        picker->SetTitle(U8(match.definition->displayName));
        picker->SetCurrentId(rawValue);
        connect(picker, &ObjectPickerPopup::EntryChosen, picker,
            [this, picker, stillSelected, rawcode, sourceField, rawValue](const ObjectViewEntry& entry) {
                if (stillSelected() && (entry.id == rawValue || CommitStandardFieldValue(rawcode, sourceField, entry.id)))
                    picker->close();
            });
        auto* cellAnchor = new QWidget(view->viewport());
        cellAnchor->setGeometry(view->visualRect(proxyIndex));
        cellAnchor->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(picker, &QObject::destroyed, cellAnchor, &QObject::deleteLater);
        picker->OpenAt(cellAnchor);
        return;
    }

    auto* panel = new QFrame(view, Qt::Tool | Qt::FramelessWindowHint);
    panel->setObjectName(QStringLiteral("standardReferenceEditor"));
    panel->setWindowModality(Qt::NonModal);
    panel->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(panel);
    layout->addWidget(new QLabel(U8(match.definition->displayName), panel));
    auto* list = new OrderedObjectListWidget(project_->iconProvider_.get(), panel);
    list->SetEntries(candidates);
    list->SetKnownEntries(objectViewEntries_);
    if (match.definition->referenceTypes.size() == 1)
        list->SetKindFilter(Kind(match.definition->referenceTypes.front()));
    list->SetIds(rawValue.isEmpty() || rawValue == QStringLiteral("_")
        ? QStringList{} : rawValue.split(QChar(','), Qt::KeepEmptyParts));
    list->SetInlineCatalogEnabled(true);
    layout->addWidget(list, 1);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, panel);
    layout->addWidget(actions);
    connect(actions->button(QDialogButtonBox::Apply), &QPushButton::clicked, panel,
        [this, panel, list, stillSelected, rawcode, sourceField, rawValue] {
            const QString submitted = list->Ids().join(QChar(','));
            if (stillSelected() && (submitted == rawValue || CommitStandardFieldValue(rawcode, sourceField, submitted)))
                panel->close();
        });
    connect(actions, &QDialogButtonBox::rejected, panel, &QWidget::close);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
    connect(escape, &QShortcut::activated, panel, &QWidget::close);
    const QRect available = view->screen()->availableGeometry();
    const QSize size = QSize(860, 500).boundedTo(available.size());
    const QPoint origin = view->viewport()->mapToGlobal(view->visualRect(proxyIndex).topRight());
    panel->setGeometry(QRect(QPoint(std::clamp(origin.x(), available.left(), available.right() - size.width() + 1),
        std::clamp(origin.y(), available.top(), available.bottom() - size.height() + 1)), size));
    War3Map::UiKit::ConfigureRoundedPopup(*panel);
    panel->show();
    panel->raise();
}

void ObjectModule::RefreshObjectFieldPresentation() {
    objectFieldModel_->SetShowRawFields(showRawFields_->isChecked());
    objectListModel_->RefreshPresentation();
    RebuildObjectTree();
    if (objectSourceTabs_->currentIndex() == 1) ShowSelectedObject();
    if (objectSourceTabs_->currentIndex() == 0) {
        if (const auto entry = standardObjectBrowser_->CurrentEntry()) ShowStandardObject(*entry);
    }
    RebuildObjectFieldCategories();
    UpdateObjectFieldControls();
}


void ObjectModule::RebuildObjectFieldCategories() {
    if (!objectFieldCategory_) return;
    const auto previous = objectFieldCategory_->currentData().toString();
    QStringList categories;
    if (objectSourceTabs_->currentIndex() == 0) {
        for (int row = 0; row < standardFieldModel_->rowCount(); ++row) {
            const auto category = standardFieldModel_->item(row, 0)->data(kFieldCategoryRole).toString();
            if (!category.isEmpty() && !categories.contains(category)) categories.push_back(category);
        }
    } else categories = objectFieldModel_->Categories();
    categories.sort();
    const QSignalBlocker blocker(objectFieldCategory_);
    objectFieldCategory_->clear();
    objectFieldCategory_->addItem(tr("全部分类"), QString{});
    for (const auto& category : categories) objectFieldCategory_->addItem(category, category);
    objectFieldCategory_->setCurrentIndex(std::max(0, objectFieldCategory_->findData(previous)));
    const auto category = objectFieldCategory_->currentData().toString();
    static_cast<StandardFieldFilter*>(standardFieldFilter_)->SetCategory(category);
    objectFieldModel_->SetCategoryFilter(category);
}


void ObjectModule::UpdateObjectFieldControls() {
    if (!objectFieldCount_ || !objectFieldModel_) return;
    const bool map = objectSourceTabs_->currentIndex() == 1;
    const bool project = !project_->projectRoot_.empty();
    const auto selected = standardObjectBrowser_->CurrentEntry();
    editStandardObject_->setVisible(!map);
    editStandardObject_->setEnabled(project && selected.has_value());
    const bool hasOverride = selected && project_->objectDocuments_[currentObjectType_]
        && project_->objectDocuments_[currentObjectType_]->FindObject(
            {static_cast<ObjectType>(currentObjectType_), selected->id.toStdString()});
    editStandardObject_->setText(hasOverride ? tr("打开") : tr("修改此对象"));
    editStandardObject_->setToolTip(project ? tr("修改保存在本地图中，标准数据不变") : tr("先打开地图"));
    if (auto* create = createObjectButton_) {
        create->setEnabled(project && project_->standardCatalog_ && !project_->standardCatalog_->Entries().empty());
        create->setToolTip(project ? tr("选择基础对象并创建") : tr("先打开地图"));
    }
    const int count = map ? objectFieldModel_->rowCount() : standardFieldFilter_->rowCount();
    const int total = map ? objectFieldModel_->FieldCount() : standardFieldModel_->rowCount();
    const auto issue = map ? objectFieldModel_->InheritanceIssue() : QString{};
    const auto countText = count == total ? tr("%1 个字段").arg(count) : tr("%1 / %2 个字段").arg(count).arg(total);
    objectFieldCount_->setText(countText + QStringLiteral(" · ")
        + (!issue.isEmpty() ? tr("继承异常 · 只读") : map ? tr("品红色为已修改的字段")
            : project ? tr("双击字段直接修改 · 原版资源只读") : tr("标准对象 · 打开地图后可编辑")));
    objectFieldCount_->setToolTip(issue);
    const auto* stack = project_->objectUndoStacks_[currentObjectType_];
    undoObjectEdit_->setEnabled(map && stack && stack->canUndo());
    redoObjectEdit_->setEnabled(map && stack && stack->canRedo());
    resetObjectField_->setEnabled(map && objectFieldModel_->CanReset(objectFieldView_->currentIndex().row()));
}

} // namespace War3Map::Modules

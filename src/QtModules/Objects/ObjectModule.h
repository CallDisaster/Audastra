#pragma once

#include "Modules/EditorModule.h"
#include "Widgets/ObjectCatalogWidgets.h"
#include "Widgets/ObjectResourcePicker.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QObject>
#include <QPointer>
#include <QVector>

#include <array>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QModelIndex;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItem;
class QStandardItemModel;
class QTableView;
class QTabWidget;
class NativeWorkbenchTests;
class ProjectWatchTests;
class ObjectDirectEditTests;
class ObjectBatchUiTests;
class ObjectFieldDelegate;
class ObjectFieldModel;
class ObjectListModel;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp { class ObjectTreeBrowser; }
namespace War3Map::ObjectData { class LniObjectDocument; }

namespace War3Map::Modules {

// 物体编辑器模块：七类物编的原版目录（只读）与地图覆盖（可编辑）。
// 文档、原版目录、字段定义与撤销栈都在工程层；这里只保存视图状态。
class ObjectModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
    // P1 过渡：原生工作台回归仍直接检查物编控件与模型，按模块测试替换后移除。
    friend class ::NativeWorkbenchTests;
    friend class ::ProjectWatchTests;
    friend class ::ObjectDirectEditTests;
    friend class ::ObjectBatchUiTests;

public:
    explicit ObjectModule(QtProject::EditorProject& project);
    ~ObjectModule() override;

    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    // 当前类别的地图覆盖撤销栈；原版目录页只读，没有撤销目标。
    QUndoStack* UndoStack() override;

    // 组装处调用：工程打开 / 关闭 / 保存 / 资源变化后的刷新。
    void ResetView();
    void DetachDocuments();
    void ShowObjectType(int index);
    void SelectFirstType();
    void SelectType(int index);
    void UpdateObjectReferenceEntries();
    void RefreshStandardCatalog();
    void RefreshIcons();
    void SetSearchText(const QString& text);
    void SetResourcePickerFactory(ObjectResourcePickerFactory factory);
    [[nodiscard]] bool HasLoadedContent() const;
    [[nodiscard]] int CurrentObjectType() const noexcept { return currentObjectType_; }
    [[nodiscard]] bool EditingMapObjects() const;
    [[nodiscard]] QString BreadcrumbText() const;

signals:
    // 字段被编辑：组装处核对修订、调度恢复点并刷新导出按钮。
    void DocumentEdited(int typeIndex);
    // 创建 / 撤销改变了文档结构：组装处刷新导出按钮与状态栏。
    void DocumentsChanged();
    void GameRootRequested();
    // 当前类别变化（面包屑）。
    void ContextChanged();

private:
    void BuildPage();
    void ShowStatus(const QString& text, int timeoutMs = 0);
    // 视图（ObjectModule.cpp）
    void RememberSelection();
    void ShowSelectedObject();
    void MarkCurrentObjectDocumentDirty();
    void ShowObjectListContextMenu(const QPoint& pos);
    void DeleteSelectedObject();
    // 字段展示（ObjectFieldsView.cpp）
    void ShowStandardObject(const QtApp::ObjectViewEntry& entry);
    void OnStandardFieldChanged(QStandardItem* item);
    void BeginStandardReferenceEdit(const QModelIndex& proxyIndex);
    void RefreshObjectFieldPresentation();
    void RebuildObjectFieldCategories();
    void UpdateObjectFieldControls();
    void ShowObjectFieldContextMenu(const QPoint& pos);
    void AutofillObjectFieldLevels(int row);
    // 原版风格目录树（ObjectTreeActions.cpp）
    void RebuildObjectTree();
    void OnObjectTreeSelected(const QString& id, bool custom);
    void SyncObjectTreeSelection();
    void ShowObjectTreeMenu(const QPoint& globalPos);
    // 创建与复制（ObjectCreation.cpp）
    bool EnsureObjectDocument(int typeIndex);
    void SelectMapObject(int typeIndex, const QString& rawcode);
    void RefreshObjectDocument(int typeIndex, const QString& preferredRawcode);
    void CommitObjectCreation(int typeIndex, ObjectData::LniObjectDocument next,
        const QString& rawcode, const QString& label);
    [[nodiscard]] QString SuggestObjectId(int typeIndex, const QString& baseRawcode) const;
    bool CreateObjectFromBase(int typeIndex, const QString& baseRawcode, const QString& rawcode, const QString& name);
    bool CloneCustomObject(int typeIndex, const QString& sourceRawcode, const QString& newRawcode);
    void DuplicateSelectedCustomObject();
    void EditSelectedStandardObject();
    bool CommitStandardFieldValue(const QString& rawcode, const QString& sourceField, const QString& value);
    void CreateNewCustomObject();
    // 多个地图对象的同一可编辑字段以单条撤销命令提交。
    void BatchEditObjects();
    bool CommitBatchFieldValue(const std::vector<ObjectData::ObjectKey>& targets,
        const ObjectData::ModificationKey& field, const QString& value);

    // 字段沿用旧主窗口命名，便于既有回归按原语义检查。
    QtProject::EditorProject* project_ = nullptr;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QListWidget* objectCategories_ = nullptr;
    QTableView* objectListView_ = nullptr;
    QTableView* objectFieldView_ = nullptr;
    ObjectListModel* objectListModel_ = nullptr;
    ObjectFieldModel* objectFieldModel_ = nullptr;
    ObjectFieldDelegate* objectFieldDelegate_ = nullptr;
    QSortFilterProxyModel* objectFilter_ = nullptr;
    QLineEdit* objectFieldSearch_ = nullptr;
    QLineEdit* mapObjectSearch_ = nullptr;
    QCheckBox* showRawFields_ = nullptr;
    QComboBox* objectFieldCategory_ = nullptr;
    QLabel* objectFieldCount_ = nullptr;
    QPushButton* resetObjectField_ = nullptr;
    QPushButton* undoObjectEdit_ = nullptr;
    QPushButton* redoObjectEdit_ = nullptr;
    QPushButton* editStandardObject_ = nullptr;
    QPushButton* createObjectButton_ = nullptr;
    QTabWidget* objectSourceTabs_ = nullptr;
    QStackedWidget* objectDetailPages_ = nullptr;
    QtApp::ObjectCatalogWidget* standardObjectBrowser_ = nullptr;
    // 左侧目录树（标准 / 自定义 → 种族 → …）；上面两页保留为隐藏的选择状态。
    QtApp::ObjectTreeBrowser* objectTree_ = nullptr;
    bool syncingObjectTree_ = false;
    QPointer<QtApp::ObjectPickerPopup> newObjectPicker_;
    QStandardItemModel* standardFieldModel_ = nullptr;
    QSortFilterProxyModel* standardFieldFilter_ = nullptr;
    QLabel* objectPreviewIcon_ = nullptr;
    QLabel* objectPreviewName_ = nullptr;
    QLabel* objectPreviewSource_ = nullptr;
    QVector<QtApp::ObjectViewEntry> objectViewEntries_;
    std::array<QString, 7> lastSelectedObject_; // 每个分类记住上次选中的 rawcode
    int currentObjectType_ = 0;
    ObjectResourcePickerFactory resourcePickerFactory_;
};

} // namespace War3Map::Modules

#pragma once

#include "Modules/EditorModule.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <array>
#include <optional>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QModelIndex;
class QStandardItem;
class QStandardItemModel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QTreeView;
class QUndoStack;
namespace War3Map::QtProject { class EditorProject; struct AssetEntry; struct AssetState; }

namespace War3Map::Modules {

// 资源模块（类似 Windows 资源管理器）：左侧是按归档路径组成的虚拟文件夹树，右侧是当前文件夹的
// 详细信息列表（名称 / 类型 / 大小 / 状态 / 完整路径），上方是路径栏。
// 地图原有成员与待导入文件一起显示；改名、移动（拖到文件夹）、新建文件夹、删除都可撤销，保存时写入归档与导入清单。
// 按钮图标缺少暂停图标（DISBTN / DISPAS）时在状态列显示黄色感叹号，保存时自动生成。
class AssetsModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT

public:
    explicit AssetsModule(QtProject::EditorProject& project);
    ~AssetsModule() override;

    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& bookmark) override;
    QUndoStack* UndoStack() override;

    // 导入磁盘文件；folder 为空时按文件类型建议路径（按钮图标进标准文件夹），否则放进该文件夹。
    void AddFiles(const QStringList& diskPaths, const QString& folder = {});
    // 打开文件夹（归档内路径，空为根）。
    void OpenFolder(const QString& folder);
    [[nodiscard]] QString CurrentFolder() const { return folder_; }
    // 资源操作（均可撤销）。返回空字符串表示成功，否则为原因。
    QString MoveAssets(const QStringList& paths, const QString& folder);
    QString RenameAsset(const QString& path, const QString& newPath);
    QString RenameFolder(const QString& folder, const QString& newName);
    QString DeleteAssets(const QStringList& paths);
    void NewFolder(const QString& name);
    // 双击 / F2：导入文件属性对话框。
    void EditProperties(const QString& path);

    // 测试与探针：当前列表各行的文字（列之间用 | 连接）、文件夹树的全部文件夹路径、待导入的归档路径。
    [[nodiscard]] QStringList ListedRows() const;
    [[nodiscard]] QStringList FolderPaths() const;
    [[nodiscard]] QStringList PendingPaths() const;
    [[nodiscard]] int AssetCount() const;
    [[nodiscard]] int PendingCount() const { return static_cast<int>(PendingPaths().size()); }
    [[nodiscard]] QTreeView* FileList() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Refresh();
    void RebuildTree();
    void RebuildList();
    void RebuildBreadcrumb();
    void QueueResponsiveLayout();
    void UpdateResponsiveLayout();
    void ShowContextMenu(const QPoint& pos);
    // 执行一次资源编辑并登记撤销；失败时恢复原状态并提示。
    QString Record(const QString& text, const std::function<QString(QtProject::AssetState&)>& edit);
    [[nodiscard]] QStringList SelectedPaths() const;
    [[nodiscard]] QString FolderAt(const QModelIndex& index) const;
    bool HandleDrop(QObject* watched, QEvent* event);
    void Status(const QString& text) const;

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QPointer<QWidget> toolbar_;
    QPointer<QLabel> toolbarTitle_;
    QPointer<QPushButton> importButton_;
    std::array<QPointer<QPushButton>, 3> secondaryButtons_;
    QPointer<QToolButton> moreButton_;
    QPointer<QTreeView> tree_;
    QPointer<QTreeView> files_;
    QPointer<QLineEdit> search_;
    QPointer<QWidget> breadcrumb_;
    QPointer<QScrollArea> breadcrumbScroll_;
    QPointer<QToolButton> currentCrumb_;
    QHBoxLayout* breadcrumbLayout_ = nullptr;
    QPointer<QLabel> summary_;
    QStandardItemModel* treeModel_ = nullptr;
    QStandardItemModel* listModel_ = nullptr;
    QUndoStack* undoStack_ = nullptr;
    QString folder_;
    // 新建但还没有文件的文件夹（只在本次会话显示）。
    QSet<QString> emptyFolders_;
    QSet<QString> expanded_;
    bool rebuilding_ = false;
    bool layoutQueued_ = false;
    bool updatingLayout_ = false;
};

} // namespace War3Map::Modules

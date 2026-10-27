#pragma once

#include <QHash>
#include <QIcon>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <vector>

class QLineEdit;
class QCheckBox;
class QModelIndex;
class QSortFilterProxyModel;
class QStandardItem;
class QStandardItemModel;
class QTreeView;

namespace War3Map::QtApp {

class IconProvider;

// 原版风格的物体目录树（对应 WE 物体编辑器左侧）：
//   标准单位 (834) → 人族 (108) → 对战 → 单位 / 建筑 / 英雄 / 特殊
//   自定义单位 (462) → …
// 文件夹带数量；自定义物体与修改过的标准物体按原版用品红色显示；图标按需从游戏资源读取。
class ObjectTreeBrowser final : public QWidget {
    Q_OBJECT

public:
    struct Object {
        QString id;
        QString name;
        QString iconPath;
        QStringList folders;   // 根下的文件夹路径
        bool custom = false;   // 放在“自定义”根下
        bool modified = false; // 标准物体被地图修改过
        QString tooltip;
        QIcon fallbackIcon;    // 没有自身图标时显示（装饰物/可破坏物的类别图标）
    };

    explicit ObjectTreeBrowser(IconProvider* icons, QWidget* parent = nullptr);

    // 换类别（typeKey 用于分别记住各类别的展开状态）或刷新同一类别；保持当前选中与展开状态。
    void SetObjects(const QString& typeKey, const QString& standardRoot, const QString& customRoot,
        std::vector<Object> objects);
    // 选中（不发出 ObjectSelected）；找不到时清除选中。
    void Select(const QString& id, bool custom);
    [[nodiscard]] std::optional<std::pair<QString, bool>> Current() const;
    void SetSearchText(const QString& text);
    void SetShowIds(bool enabled);
    [[nodiscard]] bool ShowIds() const;
    [[nodiscard]] QTreeView* View() const noexcept { return view_; }

signals:
    void ObjectSelected(const QString& id, bool custom);
    void ContextMenuRequested(const QPoint& globalPos);

private:
    void Rebuild();
    void RememberExpansion();
    void RestoreExpansion();
    void UpdateFolderIcon(const QModelIndex& proxyIndex, bool expanded);
    [[nodiscard]] QString FolderKey(const QStandardItem* item) const;

    IconProvider* icons_ = nullptr;
    QLineEdit* search_ = nullptr;
    QCheckBox* showIds_ = nullptr;
    QTreeView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* filter_ = nullptr;
    QString typeKey_;
    QString standardRoot_;
    QString customRoot_;
    std::vector<Object> objects_;
    QHash<QString, QSet<QString>> expanded_; // 类别 → 展开的文件夹键
    bool silent_ = false;
};

} // namespace War3Map::QtApp

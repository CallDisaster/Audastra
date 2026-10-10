#pragma once

#include <QFrame>
#include <QMetaType>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QPushButton;
class QSplitter;
class QToolButton;
class QTreeWidget;

namespace War3Map::QtApp {

class IconProvider;
class ObjectEntryModel;
class ObjectEntryFilter;

// 仅用于视图。目录数据、真实图标路径与写入事务由调用方提供。
struct ObjectViewEntry {
    QString kind;
    QString id;
    QString name;
    QString iconPath;
    QString category;
    QString race;
    QString description;
    QStringList details;
    bool available = true;
};

// 同一目录可嵌入资源页或选择浮窗。单击/Enter 发出 EntryChosen，方向键仅更新高亮。
class ObjectCatalogWidget final : public QWidget {
    Q_OBJECT

public:
    enum class ViewMode { Icons, List };

    explicit ObjectCatalogWidget(IconProvider* icons, QWidget* parent = nullptr);
    void SetEntries(const QVector<ObjectViewEntry>& entries);
    void SetKindFilter(const QString& kind, bool locked = false);
    void SetDirectoryVisible(bool visible);
    void SetCurrentId(const QString& id);
    void SetSearchText(const QString& text);
    void SetCatalogStatus(const QString& status);
    void FocusSearch();
    void RefreshIcons();
    void SetViewMode(ViewMode mode);
    [[nodiscard]] ViewMode CurrentViewMode() const noexcept { return viewMode_; }
    [[nodiscard]] std::optional<ObjectViewEntry> CurrentEntry() const;

signals:
    void EntrySelected(const War3Map::QtApp::ObjectViewEntry& entry);
    void EntryChosen(const War3Map::QtApp::ObjectViewEntry& entry);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void RebuildFilters(bool rebuildDirectory = true);
    void ApplyFilters();
    void UpdateStatus();
    void ChooseCurrent();

    ObjectEntryModel* entries_ = nullptr;
    ObjectEntryFilter* filter_ = nullptr;
    QTreeWidget* directory_ = nullptr;
    QListView* view_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* kinds_ = nullptr;
    QComboBox* races_ = nullptr;
    QComboBox* categories_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* empty_ = nullptr;
    QToolButton* iconsButton_ = nullptr;
    QToolButton* listButton_ = nullptr;
    QString kind_;
    QString catalogStatus_;
    bool kindLocked_ = false;
    ViewMode viewMode_ = ViewMode::Icons;
};

// Qt::Tool + NonModal：外点关闭并继续传递原事件，不使用 exec()/输入抓取。
class ObjectPickerPopup final : public QFrame {
    Q_OBJECT

public:
    explicit ObjectPickerPopup(IconProvider* icons, QWidget* parent = nullptr);
    void SetEntries(const QVector<ObjectViewEntry>& entries);
    void SetKindFilter(const QString& kind);
    void SetCurrentId(const QString& id);
    void SetTitle(const QString& title);
    void OpenAt(QWidget* anchor);
    [[nodiscard]] ObjectCatalogWidget* Catalog() const noexcept { return catalog_; }

signals:
    void EntryChosen(const War3Map::QtApp::ObjectViewEntry& entry);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void PositionAtAnchor();
    ObjectCatalogWidget* catalog_ = nullptr;
    QLabel* title_ = nullptr;
    QPointer<QWidget> anchor_;
};

// 固定对象有序列表；不引入运行时变量/函数，也不直接写物编文档。
// SetIds 不发信号。用户的每次操作发一次 IdsChanged，调用方将其纳入撤销事务。
class OrderedObjectListWidget final : public QWidget {
    Q_OBJECT

public:
    explicit OrderedObjectListWidget(IconProvider* icons, QWidget* parent = nullptr);
    void SetEntries(const QVector<ObjectViewEntry>& entries);
    // 仅用于标明旧 ID 已存在但类型不适用；候选仍只从 SetEntries 提供。
    void SetKnownEntries(const QVector<ObjectViewEntry>& entries);
    void SetKindFilter(const QString& kind);
    void SetIds(const QStringList& ids);
    void SetReadOnly(bool readOnly);
    // 字段面板把有序列表和候选目录并排展示；旧独立弹层调用者保持原行为。
    void SetInlineCatalogEnabled(bool enabled);
    void RefreshIcons();
    [[nodiscard]] QStringList Ids() const { return ids_; }

signals:
    void IdsChanged(const QStringList& ids);
    void EntryOpenRequested(const War3Map::QtApp::ObjectViewEntry& entry);

private:
    void RebuildList(int selectedRow = -1);
    void UpdateActions();
    void OpenPicker(bool replace);
    void AcceptEntry(const ObjectViewEntry& entry);
    void RemoveCurrent();
    void MoveCurrent(int direction);
    [[nodiscard]] const ObjectViewEntry* Resolve(const QString& id) const;

    IconProvider* icons_ = nullptr; // 生命周期由主窗口管理，长于控件。
    QVector<ObjectViewEntry> entries_;
    QVector<ObjectViewEntry> knownEntries_;
    QStringList ids_;
    QString kind_;
    QListWidget* list_ = nullptr;
    QSplitter* content_ = nullptr;
    ObjectCatalogWidget* inlineCatalog_ = nullptr;
    QLabel* inlineMode_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* replace_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    ObjectPickerPopup* picker_ = nullptr;
    int replacementRow_ = -1;
    QStringList pickerBaseline_;
    bool readOnly_ = false;
    bool inlineCatalogEnabled_ = false;
    bool popupSessionActive_ = false;
};

} // namespace War3Map::QtApp

Q_DECLARE_METATYPE(War3Map::QtApp::ObjectViewEntry)

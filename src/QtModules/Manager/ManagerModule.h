#pragma once

#include "ManagerRows.h"

#include "Modules/EditorModule.h"

#include <QHash>
#include <QObject>
#include <QPointer>

#include <array>
#include <cstdint>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace War3Map::Modules {

// 物体管理：地图概况、七类物体、触发器、函数、变量；
// 右侧列出被引用 / 放置 / 使用了 / 调用的函数，双击跳到对应模块。只读，不修改任何文档。
class ManagerModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT

public:
    explicit ManagerModule(QtProject::EditorProject& project);
    ~ManagerModule() override;

    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    [[nodiscard]] std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    [[nodiscard]] Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    QUndoStack* UndoStack() override { return nullptr; }

    // 页面可见时按引用索引代数判断是否需要重建当前页。
    void Refresh(bool force = false);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void BuildPage();
    void ShowPage(Manager::Page page);
    void PopulateTable();
    void PopulateOverview();
    void ShowDetail();
    void AddUsageSection(const QString& title, const std::vector<const Services::Usage*>& usages, const QString& empty);
    void OpenItem(QTreeWidgetItem* item);
    [[nodiscard]] const Manager::Row* CurrentRow() const;
    void RebuildNavigationIdentities();
    [[nodiscard]] std::optional<QJsonObject> FunctionNavigationIdentity(const QString& key) const;
    [[nodiscard]] std::optional<QJsonObject> CurrentNavigationIdentity(Manager::Page page, const QString& key) const;

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QListWidget* pages_ = nullptr;
    QStackedWidget* center_ = nullptr;
    QListWidget* overview_ = nullptr;
    QTreeWidget* table_ = nullptr;
    QComboBox* filter_ = nullptr;
    QCheckBox* rawValues_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* count_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QLabel* detailSubtitle_ = nullptr;
    QLabel* detailWarning_ = nullptr;
    QPushButton* openButton_ = nullptr;
    QTreeWidget* detail_ = nullptr;

    Manager::Page current_ = Manager::Page::Overview;
    std::vector<Manager::Row> rows_;
    std::vector<Manager::OverviewLine> overviewLines_;
    std::uint64_t rowsGeneration_ = 0;
    Manager::Page rowsPage_ = Manager::Page::Count;
    bool rowsAllFunctions_ = false;
    // 与实际呈现的行一起冻结，不能用新同名变量替旧行补身份。
    QHash<QString, QJsonObject> navigationIdentities_;
    std::array<QString, static_cast<int>(Manager::Page::Count)> filters_{};
    std::array<QString, static_cast<int>(Manager::Page::Count)> selection_{};
    std::vector<Shell::NavigationTarget> detailTargets_;
    bool populating_ = false;
};

} // namespace War3Map::Modules

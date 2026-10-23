#pragma once

#include "GameUiSources.h"
#include "Modules/EditorModule.h"
#include "War3MapCore/GameUi/FrameDraw.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSplitter;
class QStandardItemModel;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::UiKit { class ProblemList; }
namespace War3Map::Modules {
class GameUiCanvas;

// 游戏界面工作区：按游戏界面定义与种族皮肤展开界面树，显示原版贴图、文字和字体，
// 列出每个节点在游戏中的名字、类型、来源与位置。只读；不修改地图。
class GameUiModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
public:
    explicit GameUiModule(QtProject::EditorProject& project);
    ~GameUiModule() override;

    [[nodiscard]] Shell::ModuleInfo Info() const override;
    [[nodiscard]] QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    [[nodiscard]] bool Reveal(const Shell::NavigationTarget& target) override;
    [[nodiscard]] QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;

    // 界面与将来的命令行/MCP 适配器共用的查询和操作。
    [[nodiscard]] const GameUi::FrameTree* Tree() const noexcept { return tree_.get(); }
    [[nodiscard]] const GameUi::FrameDrawList& DrawList() const noexcept { return drawList_; }
    [[nodiscard]] QStringList Roots() const { return roots_; }
    [[nodiscard]] QStringList AvailableTemplates() const;
    bool AddRoot(const QString& name);
    bool RemoveRoot(const QString& name);
    void SetRace(const QString& race);
    void SetExpansion(bool expansion);
    bool SelectFrame(const QString& name);
    // 只显示该节点（隐藏其同级节点），父节点一并显示。
    void ShowOnly(std::uint32_t node);
    [[nodiscard]] std::uint32_t SelectedNode() const noexcept { return selected_; }
    [[nodiscard]] bool IsLoading() const noexcept { return definitionsPending_ || assetsPending_; }
    [[nodiscard]] QImage RenderImage(const QSize& size) const;
    void Reload();
    // 测试使用：以给定定义代替后台读取，同步重建（不读取贴图与字体）。
    void LoadDefinitionsForTesting(GameUiDefinitions definitions);

signals:
    void SceneChanged();
    void AssetsChanged();

private:
    void BuildPage();
    void BuildToolbar(QWidget* page, QVBoxLayout* layout);
    void ScheduleReload();
    void StartDefinitionsJob();
    void ApplyDefinitions(GameUiDefinitions definitions);
    void RebuildScene();
    void RebuildDrawList();
    void StartAssetsJob();
    void ApplyAssets(std::vector<GameUiAsset> assets);
    void PopulateTree();
    void ApplyFilter();
    void UpdateProperties();
    void UpdateProblems();
    void SelectNode(std::uint32_t node);
    void OnItemChanged(QTreeWidgetItem* item, int column);
    [[nodiscard]] QString NodeKey(std::uint32_t node) const;

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QComboBox* raceBox_ = nullptr;
    QComboBox* versionBox_ = nullptr;
    QComboBox* aspectBox_ = nullptr;
    QComboBox* addRootBox_ = nullptr;
    QPushButton* showContent_ = nullptr;
    QPushButton* showBounds_ = nullptr;
    QPushButton* showNames_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeWidget* nodes_ = nullptr;
    GameUiCanvas* canvas_ = nullptr;
    QTreeWidget* properties_ = nullptr;
    QCheckBox* rawFields_ = nullptr;
    UiKit::ProblemList* problems_ = nullptr;
    QStandardItemModel* problemModel_ = nullptr;
    QSplitter* mainSplit_ = nullptr;
    QSplitter* centerSplit_ = nullptr;

    QStringList roots_;
    // 页面创建前收到的视图状态，创建页面时套用。
    QJsonObject pendingView_;
    QString race_ = QStringLiteral("Human");
    bool expansion_ = true;
    std::shared_ptr<GameUiReader> reader_;
    QString sourceNote_;
    std::optional<GameUiDefinitions> definitions_;
    std::unique_ptr<GameUi::FrameTree> tree_;
    GameUi::FrameDrawList drawList_;
    std::vector<Diagnostic> sceneDiagnostics_;
    std::vector<Diagnostic> loadDiagnostics_;
    QSet<QString> hiddenKeys_;
    QHash<QString, QImage> textures_;
    QHash<QString, QString> fontFamilies_;
    QHash<QString, QString> assetOrigins_;
    QHash<QString, QString> assetErrors_;
    QSet<QString> requestedAssets_;
    std::uint64_t generation_ = 0;
    bool definitionsPending_ = false;
    bool assetsPending_ = false;
    bool assetsRerun_ = false;
    bool reloadQueued_ = false;
    bool populating_ = false;
    std::uint32_t selected_ = GameUi::kNoNode;
    QString selectedKey_;
};

} // namespace War3Map::Modules

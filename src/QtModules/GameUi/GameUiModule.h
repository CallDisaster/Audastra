#pragma once

#include "GameUiSources.h"
#include "Modules/EditorModule.h"
#include "War3MapCore/GameUi/CastTriggers.h"
#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/ScriptSimulation.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QList>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QJsonArray;
class QLabel;
class QLineEdit;
class QPoint;
class QPushButton;
class QSplitter;
class QStandardItemModel;
class QTabWidget;
class QTableWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::UiKit { class ProblemList; }
namespace War3Map::Modules {
class GameUiCanvas;

// 屏幕单位矩形换算为指定画面高度下的像素矩形（左上为原点），画面宽 = 高 × 宽高比。
[[nodiscard]] QRect GameUiPixelRect(const GameUi::FrameRect& rect, const GameUi::FrameTreeOptions& options, double aspect,
    int height = 1080);

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
    // 可作为描述模板框架的模板名（AvailableTemplates 去掉脚本界面与游戏代码区域两个汇总根）。
    [[nodiscard]] QStringList FrameTemplates() const;
    bool AddRoot(const QString& name);
    bool RemoveRoot(const QString& name);
    void SetRace(const QString& race);
    void SetExpansion(bool expansion);
    bool SelectFrame(const QString& name);
    // 只显示该节点（隐藏其同级节点），父节点一并显示。
    void ShowOnly(std::uint32_t node);
    [[nodiscard]] std::uint32_t SelectedNode() const noexcept { return selected_; }
    [[nodiscard]] QString Race() const { return race_; }
    [[nodiscard]] bool Expansion() const noexcept { return expansion_; }
    // 画面宽高比（宽/高）；页面未创建时为默认 16:9。
    [[nodiscard]] double Aspect() const;
    void SetAspect(double aspect);
    // 问题列表的内容：游戏目录、地图层、定义与布局诊断、读不出的贴图与字体。
    [[nodiscard]] std::vector<Diagnostic> Problems() const;
    // 定义来自地图界面目录的模板名（按名字排序）。
    [[nodiscard]] QStringList MapTemplates() const;
    // 地图脚本创建的界面（模拟中为模拟后的样子）；定义未读入时为空。
    [[nodiscard]] const GameUi::ScriptFramePlan* ScriptFrames() const;
    // 预览模拟：可模拟的函数、当前函数（未模拟为空）。function 为空时停止模拟；失败时 error 给出原因。
    [[nodiscard]] QStringList SimulationFunctions() const;
    [[nodiscard]] QString SimulationFunction() const;
    bool SetSimulation(const QString& function, QString* error = nullptr);
    // 合并输入值并重新模拟。
    void SetSimulationValues(const std::map<std::string, std::string>& values);
    [[nodiscard]] const GameUi::SimulationResult* Simulation() const;
    // 地图导入的自有界面描述（按编号排序；定义未读入时为空）与各自的预览状态、数值。
    [[nodiscard]] const std::vector<GameUiDescriptionFile>& Descriptions() const;
    [[nodiscard]] const GameUi::UiDescription* Description(const QString& id) const;
    [[nodiscard]] GameUi::UiInstanceState DescriptionPreview(const QString& id) const;
    // 切换预览状态（空为基础状态）、合并预览数值（键为 编号 或 编号[下标]）。不合法时整批不改并给出原因。
    bool SetDescriptionState(const QString& id, const QString& state, QString* error = nullptr);
    bool SetDescriptionValues(const QString& id, const std::map<std::string, std::string>& values, QString* error = nullptr);
    // 也画脚本或描述设为隐藏的框架。
    [[nodiscard]] bool ShowHidden() const noexcept { return showHidden_; }
    void SetShowHidden(bool show);
    // 编写地图界面描述。编辑先按 audastra.ui 的规则检查，再作为导入文件 AudastraUI\<编号>.json 放进地图的待保存修改，
    // 与导入文件同一撤销栈和保存流程。失败时给出原因，什么都不改。edits 的写法见 UiDescriptionEdit.h。
    // preset 为起始模板编号（UiPresets.h，如 castbar、party），空为空白描述。
    bool CreateDescription(const QString& id, const QString& label, QString* error = nullptr, const QString& preset = {});
    bool EditDescription(const QString& id, const QString& edits, const QString& undoLabel, QString* error = nullptr);
    // 在所选描述框架下（选中描述根节点时为顶层）添加一个框架并选中它；返回新框架编号，失败为空。
    QString AddDescriptionFrame(GameUi::UiFrameType type, QString* error = nullptr);
    // 在所选描述框架下添加游戏或地图界面文件里的模板框架。
    QString AddDescriptionTemplate(const QString& templateName, QString* error = nullptr);
    bool RemoveSelectedDescriptionFrame(QString* error = nullptr);
    // 给所选描述框架加一个锚点（取第一个没用过的对齐点）、删去第 index 个锚点。
    bool AddDescriptionAnchor(QString* error = nullptr);
    bool RemoveDescriptionAnchor(int index, QString* error = nullptr);
    // 平移描述框架的全部锚点（屏幕单位）；重复实例平移的是整组。
    bool MoveDescriptionFrame(std::uint32_t node, double dx, double dy, QString* error = nullptr);
    // 由界面描述生成自然 T（UiTriggers.h），整批写进触发器编辑器的 AudastraUI 类别，走触发器撤销栈；
    // 生成的触发器用到的基础模板随同放进地图的待保存修改。preview 为真时只给出生成结果，不改地图。
    // 函数库没有启用 AudastraUI、描述生成不了或写入被拒绝时给出原因，什么都不改。
    struct TriggerGeneration {
        QString featureText;
        QStringList triggers;
        QStringList variables;
        QStringList templateFiles;  // 本次放进地图的基础模板
        std::vector<Diagnostic> warnings;
    };
    std::optional<TriggerGeneration> GenerateDescriptionTriggers(const QString& id, bool preview, QString* error = nullptr);
    // 读条系统（CastTriggers.h）：整批写进触发器编辑器的“读条系统”类别，走触发器撤销栈。地图里已有登记触发器时保留它，
    // abilities 不写入（registrationKept 为真）。bar 为绑定的界面描述编号，其界面触发器须已生成；空为不绑定。
    // 需要 YDWE 的“运行触发器（带参数）”。preview 为真时只给出生成结果，不改地图。
    struct CastSystemRequest {
        std::vector<GameUi::CastAbility> abilities;
        QString bar;
        QString visible, progress, text, remaining, channel, interrupted;  // 界面的数值与状态编号
    };
    struct CastSystemGeneration {
        QString featureText;
        QStringList triggers;
        QStringList variables;
        bool registrationKept = false;
    };
    std::optional<CastSystemGeneration> GenerateCastSystem(const CastSystemRequest& request, bool preview, QString* error = nullptr);
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
    void BuildSimulationPanel(QWidget* parent);
    void RunSimulation();
    void UpdateSimulationPanel();
    void ResetSimulation();
    void BuildDescriptionPanel(QWidget* parent);
    void UpdateDescriptionPanel();
    void PruneDescriptionPreview(const GameUi::UiDescription& description);
    void BuildDescriptionData(QTabWidget* tabs);
    void UpdateDescriptionData(const GameUi::UiDescription& description);
    void SyncDescriptionStateRow(const GameUi::UiDescription& description, bool refill);
    void FillStateOverrides();
    // 改面板所示描述的一个列表（values、states、events，或 set＝所选状态的覆盖）后整组写回；change 返回 false 时不改，
    // 带原因时报告并恢复表格。
    void ChangeDescriptionList(const QString& list, const std::function<bool(QJsonArray&, QString*)>& change,
        const QString& what, std::function<void()> after = {});
    // 修改会重建属性树与表格（删掉正在发出信号的单元），放到下一轮事件里提交；失败时报告并恢复。
    void CommitDescriptionEdits(const QString& id, const QJsonArray& edits, const QString& undoLabel, std::function<void()> after = {});
    QList<QWidget*> BuildAuthoringControls(QWidget* toolbar);
    void ShowCastSystemDialog();
    void UpdateAuthoringControls();
    bool StageDescription(GameUi::UiDescription description, bool create, const QString& undoLabel, QString* error);
    QString AddFrameObject(QJsonObject frame, const char* idPrefix, QString* error);
    // 属性树“编辑”一组（GameUiFrameProperties.cpp）。
    void InstallPropertyEditors();
    void AddEditableRows(QTreeWidgetItem* group, const GameUi::UiFrame& frame);
    void OnPropertyEdited(QTreeWidgetItem* item, int column);
    void ShowPropertyMenu(const QPoint& position);
    [[nodiscard]] const GameUi::UiFrame* SelectedDescriptionFrame(QString* descriptionId = nullptr) const;
    // 选择类字段的可选项（显示文字、取值）；不是选择类字段时为空。
    [[nodiscard]] QList<std::pair<QString, QString>> PropertyChoices(const QString& key) const;
    [[nodiscard]] bool PropertyFreeText(const QString& key) const;
    void ReportAuthoring(const QString& error);
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
    QPushButton* showHiddenButton_ = nullptr;
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

    std::optional<GameUi::ScriptSimulator> simulator_;
    std::optional<GameUi::SimulationResult> simulation_;
    std::optional<GameUi::ScriptFramePlan> simulatedPlan_;
    std::map<std::string, std::string> simulationValues_;
    std::vector<Diagnostic> simulationDiagnostics_;
    QWidget* simulationPanel_ = nullptr;
    QComboBox* simulationBox_ = nullptr;
    QTableWidget* simulationInputs_ = nullptr;
    QStringList simulationKeys_;

    std::map<std::string, GameUi::UiInstanceState> descriptionStates_;
    bool showHidden_ = false;
    QWidget* descriptionPanel_ = nullptr;
    QLabel* descriptionTitle_ = nullptr;
    QComboBox* descriptionStateBox_ = nullptr;
    QTableWidget* descriptionInputs_ = nullptr;
    QString descriptionPanelFor_;  // 面板当前对应的描述编号
    bool descriptionRaw_ = false;  // 面板按原始字段显示
    QTabWidget* descriptionTabs_ = nullptr;
    QTableWidget* valueTable_ = nullptr;
    QTableWidget* stateTable_ = nullptr;
    QTableWidget* overrideTable_ = nullptr;
    QWidget* overridePage_ = nullptr;
    QTableWidget* eventTable_ = nullptr;
    int stateRow_ = 0;  // 状态页所选状态

    bool stagingOwnImport_ = false;  // 自己放入描述文件引发的导入变化：已就地更新，不整体重读
    std::vector<Diagnostic> authoringDiagnostics_;
    QToolButton* newDescriptionButton_ = nullptr;
    QToolButton* addFrameButton_ = nullptr;
    QPushButton* removeFrameButton_ = nullptr;
    QPushButton* generateTriggersButton_ = nullptr;
    QPushButton* castSystemButton_ = nullptr;
};

} // namespace War3Map::Modules

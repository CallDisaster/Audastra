#pragma once

#include "GameUiSources.h"
#include "Modules/EditorModule.h"
#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/ScriptSimulation.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/GameUi/UiUnits.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "Widgets/ObjectResourcePicker.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
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
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QAbstractItemView;
class QModelIndex;
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
namespace War3Map::Trigger { class TriggerDocument; }
namespace War3Map::UiKit { class ProblemList; }
namespace War3Map::Modules {
class GameUiCanvas;
struct GameUiTriggerSimulation;
struct GameUiTriggerSimulationJob;

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
    // 新建、导入后选中：界面树还没按新描述重建时，等重建出这个框架再选中并滚到可见。
    void SelectFrameWhenReady(const QString& name);
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
    // 节点树每行的“可见”“可选”：不可见的节点不画，画布上也点不中；不可选的照常画，画布上的点击、右键与悬停穿过它。
    // 设置一个节点时对它下面所有节点做同样的设置，之后子节点仍可单独改。原版界面默认不可选，自己的界面默认可选；
    // 两项按节点身份记住，按地图保存。节点树里点击任何节点都能选中，不受“可选”限制。
    void SetNodeVisible(std::uint32_t node, bool visible);
    void SetNodeSelectable(std::uint32_t node, bool selectable);
    [[nodiscard]] bool NodeVisible(std::uint32_t node) const;
    [[nodiscard]] bool NodeSelectable(std::uint32_t node) const;
    // 画布上能否点中：自身可选、自身与各级父节点都画出来；界面的根不点中（它铺满整个画面）。
    [[nodiscard]] bool NodePickable(std::uint32_t node) const;
    // 原版界面的默认“可选”。切换时清掉原版节点各自的设置。
    void SetOriginSelectable(bool selectable);
    [[nodiscard]] bool OriginSelectable() const noexcept { return originSelectable_; }
    // 新界面的建议编号（ui1、ui2…）；检查新界面编号，合法时返回空，否则返回原因。
    [[nodiscard]] QString SuggestDescriptionId() const;
    [[nodiscard]] QString CheckNewDescriptionId(const QString& id) const;
    // 当前界面：最近选中或新建的那份，新控件默认放进它；没有界面时为空。
    [[nodiscard]] QString CurrentDescription() const;
    // 编写地图界面描述。编辑先按 audastra.ui 的规则检查，再作为导入文件 AudastraUI\<编号>.json 放进地图的待保存修改，
    // 与导入文件同一撤销栈和保存流程。失败时给出原因，什么都不改。edits 的写法见 UiDescriptionEdit.h。
    // starter 为真时新界面自带一个面板（StarterUiFrame）并选中它，与新建同一步撤销。
    bool CreateDescription(const QString& id, const QString& label, QString* error = nullptr, bool starter = false);
    bool EditDescription(const QString& id, const QString& edits, const QString& undoLabel, QString* error = nullptr);
    // 从文件导入一份界面描述（例如 examples/game-ui 里的示例）。withTriggers 时把同目录的 triggers.txt 按功能写法
    // 写进以界面显示名命名的类别，经共用触发器事务发布，描述在发布前放入；任一步失败都不改地图。
    bool ImportDescriptionFile(const QString& path, bool withTriggers, QString* error = nullptr);
    // 界面上的导入：同目录有 triggers.txt 时问一次是否一并写入，失败写进问题列表。
    void ImportExample(const QString& path);
    // 在所选描述框架下（选中描述根节点时为顶层）添加一个框架并选中它；kind 为 UiFrameKinds 的编号，模板类型给模板名。
    // 返回新框架编号，失败为空。
    QString AddDescriptionFrame(const QString& kind, QString* error = nullptr, const QString& templateName = {});
    // 在描述 description 的框架 parent 下（空为顶层）添加；at 为新框架中心（屏幕单位，左下为原点），空为居中于父框架。
    // description 为空时新建一份界面（建议编号）再放进去，与新建同一步撤销。
    QString CreateDescriptionFrame(const QString& kind, const QString& description, const QString& parent,
        std::optional<QPointF> at, const QString& templateName = {}, QString* error = nullptr);
    // 把控件移到同级的最上层（最后画）或最下层。
    bool RaiseDescriptionFrame(std::uint32_t node, bool top, QString* error = nullptr);
    // 在所选描述框架下添加游戏或地图界面文件里的模板框架。
    QString AddDescriptionTemplate(const QString& templateName, QString* error = nullptr);
    // 类型预览：只含这一个框架的界面树与绘制清单；用到的贴图与字体按需读取，读到后发出 AssetsChanged。
    struct FramePreview {
        std::unique_ptr<GameUi::FrameTree> tree;
        GameUi::FrameDrawList list;
        std::optional<GameUi::FrameRect> focus;  // 框架所占区域
    };
    [[nodiscard]] FramePreview PreviewFrame(const GameUi::UiFrame& frame);
    // 整份界面的预览（创建卡片里的示例）；放大到顶层控件合起来的范围。
    [[nodiscard]] FramePreview PreviewDescription(const GameUi::UiDescription& description);
    [[nodiscard]] const QHash<QString, QImage>& Textures() const noexcept { return textures_; }
    [[nodiscard]] const QHash<QString, QString>& FontFamilies() const noexcept { return fontFamilies_; }
    // 右键创建：打开类型选取，放进 node 所在的描述框架（不在描述里时为所选或第一份描述的顶层）；
    // 没有描述时返回空。at 为新框架中心。
    QPointer<QWidget> OpenKindPicker(std::uint32_t node, std::optional<QPointF> at, const QPoint& global);
    // 右键“设置界面”：选中该描述框架并在 global 旁打开设置卡片（与右侧“编辑”同一套字段与检查），卡片跟随选择，
    // 选中的不是描述框架时关闭。不是描述框架时返回空。
    QPointer<QWidget> OpenFrameSettings(std::uint32_t node, const QPoint& global);
    // 贴图字段用物编同一个资源选择器（游戏文件、导入文件、自定义路径，带预览）；没有时贴图字段直接手写。
    void SetResourcePickerFactory(ObjectResourcePickerFactory factory) { resourcePickerFactory_ = std::move(factory); }
    bool RemoveSelectedDescriptionFrame(QString* error = nullptr);
    bool CopySelectedDescriptionFrame(QString* error = nullptr);
    // 粘贴到所选控件下；没选中自己的界面时粘贴到当前界面的顶层。
    bool PasteDescriptionFrame(QString* error = nullptr);
    [[nodiscard]] bool HasClipboardFrame() const;
    [[nodiscard]] QString OriginNodeName(std::uint32_t node) const;
    [[nodiscard]] QString OriginDescriptionId(const QString& name) const;
    bool EditOriginNode(const QString& name, const QJsonObject& fields, QString* error = nullptr);
    bool RestoreOriginNode(const QString& name, QString* error = nullptr);
    bool MoveOriginNode(std::uint32_t node, double dx, double dy, QString* error = nullptr);
    bool CopyOriginTemplate(std::uint32_t node, QString* error = nullptr);
    // 给所选描述框架加一个锚点（取第一个没用过的对齐点）、删去第 index 个锚点。
    bool AddDescriptionAnchor(QString* error = nullptr);
    bool RemoveDescriptionAnchor(int index, QString* error = nullptr);
    // 平移描述框架的全部锚点（屏幕单位）；重复实例平移的是整组。
    bool MoveDescriptionFrame(std::uint32_t node, double dx, double dy, QString* error = nullptr);
    // 换父控件（parent 为空时放到界面顶层，排在最后）。以父控件为目标的锚点按新旧父控件上对应点的位置差改偏移，
    // 画面上位置不变；两边有一边还没排出位置时偏移不改。不能放进自己或自己的子控件。
    bool ReparentDescriptionFrame(const QString& description, const QString& frame, const QString& parent, QString* error = nullptr);
    // 放到 parent 下第 index 个（空为排在最后；同一父控件下只调整先后，后面的画在上面）。换父控件时画面上位置不变。
    // 节点树拖放与助手工具共用。
    bool PlaceDescriptionFrame(const QString& description, const QString& frame, const QString& parent, std::optional<std::size_t> index,
        QString* error = nullptr);
    // 改显示名（frame 为空时改界面的名称）；编号不变，触发器与事件不受影响。
    bool RenameDescriptionNode(const QString& description, const QString& frame, const QString& label, QString* error = nullptr);
    // 节点树拖放：把 dragged 放到 target 上方（-1）、里面（0）或下方（1）。只能在同一份界面里移动。
    bool DropOnNodeTree(std::uint32_t dragged, std::uint32_t target, int where, QString* error = nullptr);
    [[nodiscard]] bool CanDropOnNodeTree(std::uint32_t dragged, std::uint32_t target, int where) const;
    // 节点树里改名（F2）：编辑框里是显示名，确认后按显示名保存。
    void RenameNodeItem(std::uint32_t node);
    // 控件（阵列时为第一个）或界面根（frame 为空）当前排出的位置；还没排出时为空。
    [[nodiscard]] std::optional<GameUi::FrameRect> DescriptionFrameRect(const QString& description, const QString& frame) const;
    // 铺满父控件：两个锚点左上、右下对齐父控件，去掉宽高；取消时改为居中于父控件、宽高取当前大小，画面上不动。
    bool SetDescriptionFrameFill(const QString& description, const QString& frame, bool fill, QString* error = nullptr);
    [[nodiscard]] static bool FillsParent(const GameUi::UiFrame& frame);
    // 按矩形摆放控件（屏幕单位，阵列时为第一个实例）：每个锚点按自己对齐点在新旧矩形上的位置差改偏移，
    // 锚点没定住的方向改宽高；没有锚点的控件按左下角相对屏幕放下。锚点目标也在这一批里时扣掉目标的移动。
    // 整批检查、同一步撤销。画布改大小、对齐分布与助手工具共用。
    bool SetDescriptionFrameRects(const QString& description, const QList<std::pair<QString, GameUi::FrameRect>>& rects,
        const QString& undoLabel, QString* error = nullptr);
    // 画布上拖控制柄：node 为所拖的实例，rect 为它的新矩形；阵列按同样的变化改第一个实例。
    bool ResizeDescriptionFrame(std::uint32_t node, const GameUi::FrameRect& rect, QString* error = nullptr);
    // 尺寸与偏移的显示单位：魔兽界面单位，或参考分辨率下的像素（UiUnits.h）。属性里输入带 px 时总按像素换算。
    void SetPixelUnits(bool pixels);
    [[nodiscard]] bool PixelUnits() const noexcept { return pixelUnits_; }
    void SetReferenceResolution(GameUi::UiResolution resolution);
    // 画布参考图：路径为空时不显示；读不出时清掉并报告。
    void SetReferenceImage(const QString& path, double opacity);
    [[nodiscard]] QString ReferenceImagePath() const { return referencePath_; }
    [[nodiscard]] GameUi::UiResolution ReferenceResolution() const noexcept { return resolution_; }
    // 控件事件与触发器（GameUiFrameEvents.cpp）。一个控件每种事件对应一个界面事件，可由多个触发器用“界面事件”订阅。
    struct FrameEventLink {
        QString eventId;  // 空为还没有这个事件
        QString key;      // 触发器事件“界面事件”里填的事件名
        QList<std::pair<StableId, QString>> triggers;  // 订阅它的触发器（编号、名称），按触发器顺序
    };
    [[nodiscard]] FrameEventLink FrameEventTriggers(const QString& description, const QString& frame, GameUi::UiEventKind kind) const;
    // 可订阅界面事件的触发器：不含生成的界面触发器与自定义代码触发器，按触发器顺序。
    [[nodiscard]] QList<std::pair<StableId, QString>> EventTriggerCandidates() const;
    // trigger 为空时新建响应触发器（已有同名的直接用它）；为 0 时解除：删去这个事件和各触发器里订阅它的那一行；
    // 其余为让该触发器订阅它。事件不在时先加上。界面触发器随之重新生成，与描述、触发器的修改同一步撤销。
    // 返回响应的触发器（解除时为 0）；失败时什么都不改。
    std::optional<StableId> LinkFrameEvent(const QString& description, const QString& frame, GameUi::UiEventKind kind,
        std::optional<StableId> trigger, QString* error = nullptr);
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
    // 给界面事件建一个响应触发器（事件“界面事件”已填好事件名），放进以界面显示名命名的类别，返回触发器编号；
    // 已有同名触发器时直接返回它，不覆盖作者写的动作。
    std::optional<StableId> CreateEventTrigger(const QString& id, const QString& eventId, QString* error = nullptr);
    // 给模拟计时器建一个触发器（事件“界面模拟计时器到期”已填好），规则同上。
    std::optional<StableId> CreateSimulationTrigger(const QString& id, const QString& timerId, QString* error = nullptr);
    // 生成的界面触发器要用原生运行库。按规格在首次生成时由作者确认，保存时不再问：approved 为本工程已批准，
    // packages 为本机可信目录里的包。界面在生成后询问；Agent 拿到结果后须经作者同意再用 map_runtime_set 启用。
    struct RuntimeRequirement {
        bool approved = false;
        Services::MapRuntimeSettings settings;
        std::vector<Services::MapRuntimePackageInfo> packages;
        QString problem;  // 查询失败的原因
    };
    [[nodiscard]] RuntimeRequirement QueryRuntimeRequirement() const;
    // 界面模拟（GameUiTriggerSimulation.cpp）：界面 id 的模拟计时器按周期到期，执行作者用“界面模拟计时器到期”订阅它的触发器，
    // 再把这份界面的数值与状态变量读回预览。第一次开始或单步时在后台按当前触发器（含未保存的修改）生成整图脚本并初始化，
    // 之后全局量保留；改了触发器要重置。触发器停下时暂停，位置在问题列表里。不能开始时给出原因。
    struct TriggerSimulationStatus {
        QString description;  // 正在模拟的界面；空为没有
        bool preparing = false;
        bool running = false;
        bool ready = false;  // 脚本已生成并初始化
        double time = 0.0;   // 模拟经过的秒数
        std::uint32_t fired = 0;
        std::vector<std::pair<std::string, std::string>> subscriptions;  // （界面编号, 计时器编号）
    };
    bool StartTriggerSimulation(const QString& id, QString* error = nullptr);
    void PauseTriggerSimulation();
    // 推进到下一个计时器到期；还没准备好时先准备，准备好后推进。
    bool StepTriggerSimulation(const QString& id, QString* error = nullptr);
    // 停止并丢掉生成的脚本；预览回到起始值与基础状态。
    void ResetTriggerSimulation();
    [[nodiscard]] TriggerSimulationStatus TriggerSimulationState() const;
    [[nodiscard]] const std::vector<Diagnostic>& TriggerSimulationProblems() const;
    // 测试使用：以给定脚本代替后台生成，同步准备好界面 id 的模拟（拦截、初始化与起始值同正常流程）。
    bool PrepareTriggerSimulationForTesting(const QString& id, std::vector<GameUi::MapScriptText> scripts, QString* error = nullptr);
    [[nodiscard]] bool IsLoading() const noexcept { return definitionsPending_ || assetsPending_; }
    [[nodiscard]] QImage RenderImage(const QSize& size) const;
    void Reload();
    // 测试使用：以给定定义代替后台读取，同步重建（不读取贴图与字体）。
    void LoadDefinitionsForTesting(GameUiDefinitions definitions);

signals:
    void SceneChanged();
    void AssetsChanged();
    void TriggerSimulationChanged();

protected:
    // 节点树的可见、可选两列：按下即切换，不改选中行；拖放交给界面修改。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 可见改动后重建绘制清单；可点中表与节点树图标随之更新。
    void ApplyNodeVisibility();
    void UpdatePickable();
    void RefreshNodeToggles();
    // 自己的界面里的节点（含界面的根）。
    [[nodiscard]] bool OwnNode(std::uint32_t node) const;
    // 节点自身与各级父节点都画出来（作者设的可见与脚本、描述设的隐藏）。
    [[nodiscard]] bool NodeShown(std::uint32_t node) const;
    // 可见、可选按地图保存：换地图时存下当前地图的，再取出新地图的。
    void SyncMapView();
    void StoreMapView();
    void SetCurrentDescription(const QString& id);
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
    // 节点树：拖放落点（-1 上方、0 里面、1 下方）、F2 改名的提交与取消、类型列中文名与原名切换。
    [[nodiscard]] int DropPosition(QTreeWidgetItem* target, const QPoint& position) const;
    void OnNodeItemChanged(QTreeWidgetItem* item, int column);
    void OnNodeEditorClosed();
    void RefreshTypeColumn();
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
    void BuildTriggerSimulationPage(QTabWidget* tabs);
    // refill 为真时重填计时器与起始值表格，否则只更新按钮与状态。
    void UpdateTriggerSimulationPage(bool refill = false);
    bool BeginTriggerSimulation(const QString& id, QString* error);
    bool PrepareTriggerSimulation(const QString& id, QString* error);
    // 要拦截的界面触发器（脚本变量名），同时记下各自的动作。
    std::vector<std::string> PlanTriggerSimulation(const GameUi::UiDescription& description, const Trigger::TriggerDocument& document);
    void FinishTriggerSimulation(std::uint64_t generation, GameUiTriggerSimulationJob& job);
    // 推进到 until（空为下一个到期的那一刻），执行到期的计时器后读回数值。
    void AdvanceTriggerSimulation(std::optional<double> until);
    void ApplyTriggerSimulationValues(const std::vector<std::string>& intercepted);
    // 停止并丢掉生成的脚本，不动预览；返回原来模拟的界面编号。
    QString StopTriggerSimulation();
    // 预览设为模拟起始值与基础状态。
    void ShowSimulationStart(const QString& id);
    // 改面板所示描述的 simulation 段后整体写回（set_simulation）。
    void ChangeSimulation(const std::function<bool(QJsonObject&, QString*)>& change, const QString& what);
    // 改面板所示描述的一个列表（values、states、events，或 set＝所选状态的覆盖）后整组写回；change 返回 false 时不改，
    // 带原因时报告并恢复表格。
    void ChangeDescriptionList(const QString& list, const std::function<bool(QJsonArray&, QString*)>& change,
        const QString& what, std::function<void()> after = {});
    // 修改会重建属性树与表格（删掉正在发出信号的单元），放到下一轮事件里提交；失败时报告并恢复。
    void CommitDescriptionEdits(const QString& id, const QJsonArray& edits, const QString& undoLabel, std::function<void()> after = {});
    QList<QWidget*> BuildAuthoringControls(QWidget* toolbar);
    // 编号、显示名与“创建”一行，放进菜单（工具栏“新建界面”与右键菜单共用）。
    void AddNewDescriptionForm(QMenu* menu);
    // 画布与节点树的右键菜单：新建界面描述、创建界面、删除，节点树另有只显示、全部显示、移除根界面。
    void ShowContextMenu(std::uint32_t node, std::optional<QPointF> at, const QPoint& global, bool fromTree);
    void AddOriginProperties();
    bool OnOriginPropertyEdited(QTreeWidgetItem* item);
    [[nodiscard]] GameUi::UiSkin MakeSkin() const;
    void UpdateAuthoringControls();
    bool StageDescription(GameUi::UiDescription description, bool create, const QString& undoLabel, QString* error);
    // 由描述生成触发器，与 extra 同一个触发器事务（同一步撤销）写入；stage 在事务里暂存别的修改（导入时的描述本身）。
    std::optional<TriggerGeneration> PublishDescriptionTriggers(const GameUi::UiDescription& description, bool preview,
        std::vector<Trigger::GraphEditOperation> extra, const QString& undoLabel, const std::function<Status()>& stage, QString* error);
    // 属性树“编辑”一组与设置卡片（GameUiFrameProperties.cpp）：两处用同一套字段、选择器与修改。
    void InstallPropertyEditors(QTreeWidget* tree);
    // 自己控件的可改字段，分组为 基本 / 位置与大小 / 外观 / 事件 / 阵列，挂在 root 下。
    void AddEditableRows(QTreeWidgetItem* root, const GameUi::UiFrame& frame);
    void AddEventRows(QTreeWidgetItem* group, const QString& description, const GameUi::UiFrame& frame);
    // 分组行：记下组名，展开状态取作者上次的选择（没有时 expandedByDefault）。
    void MarkPropertyGroup(QTreeWidgetItem* group, const QString& key, bool expandedByDefault = true);
    // 长度字段按当前单位显示与解析（horizontal 为横向）。
    [[nodiscard]] QString LengthText(double units, bool horizontal) const;
    [[nodiscard]] std::optional<double> ParseLength(const QString& text, bool horizontal) const;
    void OnPropertyEdited(QTreeWidgetItem* item, int column);
    void ShowPropertyMenu(QTreeWidget* tree, const QPoint& position);
    void OpenResourcePicker(QTreeWidget* tree, QTreeWidgetItem* item);
    void ShowResourcePicker(Services::ObjectResourceKind kind, const QString& current, QAbstractItemView* view, const QModelIndex& index,
        std::function<bool(const QString&)> commit);
    void OpenOverrideResourcePicker(int row, int column);
    std::optional<StableId> CreateHandlerTrigger(const GameUi::UiDescription& description, const std::string& role, const std::string& key,
        const std::string& event, const std::string& comment, const QString& undoLabel, QString* error);
    void RefreshFrameSettings();
    [[nodiscard]] const GameUi::UiFrame* SelectedDescriptionFrame(QString* descriptionId = nullptr) const;
    // 选择类字段的可选项（显示文字、取值）；不是选择类字段时为空。
    [[nodiscard]] QList<std::pair<QString, QString>> PropertyChoices(const QString& key) const;
    [[nodiscard]] bool PropertyFreeText(const QString& key) const;
    void ReportAuthoring(const QString& error);
    // 生成成功后：本工程还没批准运行库时问作者一次，没有可用包时在问题列表里说明。
    void OfferMapRuntime(bool choosePackage = false);
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
    QPushButton* showGrid_ = nullptr;
    QToolButton* referenceButton_ = nullptr;
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
    GameUi::FrameDrawList previewDrawList_;  // 类型预览用到的贴图与字体，随画面一起读取
    std::vector<Diagnostic> sceneDiagnostics_;
    std::vector<Diagnostic> loadDiagnostics_;
    QSet<QString> hiddenKeys_;               // 作者设为不可见的节点
    QHash<QString, bool> selectOverrides_;  // 作者单独设过“可选”的节点
    bool originSelectable_ = false;
    QPushButton* originSelectableButton_ = nullptr;
    QString mapViewKey_;  // 可见、可选当前对应的地图；空为没有地图
    QJsonArray mapViews_;  // 各地图的可见、可选，最近的在前
    QString currentDescription_;
    QHash<QString, bool> groupExpanded_;  // 属性分组的展开状态
    bool pixelUnits_ = false;
    GameUi::UiResolution resolution_;
    QString referencePath_;  // 画布参考图（只在本机视图状态里记路径）
    double referenceOpacity_ = 0.5;
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
    QTreeWidgetItem* ownGroupItem_ = nullptr;     // 节点树分组“我的界面”
    QTreeWidgetItem* originGroupItem_ = nullptr;  // 节点树分组“原版界面”
    std::optional<bool> originGroupExpanded_;      // 作者展开或收起过“原版界面”后按作者的
    QTreeWidgetItem* renameItem_ = nullptr;        // 正在 F2 改名的行
    QString renameDisplay_;
    std::uint32_t selected_ = GameUi::kNoNode;
    QString selectedKey_;
    QString pendingSelection_;
    QString propertiesKey_;

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
    std::unique_ptr<GameUiTriggerSimulation> triggerSim_;
    QPushButton* simRun_ = nullptr;
    QPushButton* simStep_ = nullptr;
    QPushButton* simReset_ = nullptr;
    QLabel* simStatus_ = nullptr;
    QTableWidget* simTimers_ = nullptr;
    QTableWidget* simStarts_ = nullptr;

    bool stagingOwnImport_ = false;  // 自己放入描述文件引发的导入变化：已就地更新，不整体重读
    std::vector<Diagnostic> authoringDiagnostics_;
    std::optional<std::uint64_t> runtimeOfferDeclined_;  // 作者拒绝启用运行库时的工程代号：同一工程不再追问
    QPushButton* newDescriptionButton_ = nullptr;
    QPushButton* addFrameButton_ = nullptr;
    QPushButton* removeFrameButton_ = nullptr;
    QPushButton* generateTriggersButton_ = nullptr;
    QPushButton* runtimeSettingsButton_ = nullptr;
    ObjectResourcePickerFactory resourcePickerFactory_;
    QPointer<QFrame> settingsCard_;
    QPointer<QTreeWidget> settingsFields_;
    QPointer<QLabel> settingsTitle_;
};

} // namespace War3Map::Modules

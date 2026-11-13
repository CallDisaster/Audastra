#pragma once

#include "Modules/EditorModule.h"

#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/GuiConversionSnapshots.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerSelection.h"
#include "War3MapCore/Types.h"

#include <QIcon>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QModelIndex;
class QPoint;
class QAction;
class QLabel;
class QMenu;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRect;
class QSortFilterProxyModel;
class QToolButton;
class QTabWidget;
class QTreeView;
class QVBoxLayout;
class TriggerEcaModel;
class TriggerExplorerModel;
struct EcaMoveRequest;
struct EcaSlot;
struct TriggerArgumentEditRequest;
struct TriggerValueChoice;
struct TriggerValueObject;
class QUndoCommand;
class TriggerBlueprint;
class TriggerSentenceDelegate;
class TriggerValuePanel;
struct TriggerDirectoryItem;
struct TriggerDirectoryMove;
namespace TriggerBlueprintDetail { struct PinTarget; }
namespace War3Map::QtProject { class EditorProject; struct RemovalCodeNotice; namespace detail { struct TriggerScalarUiNotice; } }
namespace War3Map { class MapDocument; }
namespace War3Map::Trigger { struct TriggerRecord; class TriggerTypeSystem; class UiCatalog; }
namespace War3Map::Services { class CodeCheckProjectCache; struct CodeCheckLuaSources; }
namespace War3Map::UiKit { class CodeView; class CodeProblemsView; }

namespace War3Map::Modules {

class TriggerLuaScripts;
class TriggerReplaceDialog;

// 触发器模块：左侧触发器目录 + 右侧属性区与自然语言逻辑。
// 文档、函数目录与撤销栈都在工程层；这里只保存视图状态（选中项、剪贴板、搜索）。
class TriggerModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT

public:
    explicit TriggerModule(QtProject::EditorProject& project);
    ~TriggerModule() override;

    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    bool PrepareNavigationLeave() override { return CommitPendingEdits(); }
    void CancelNavigationRestoration() override;
    QUndoStack* UndoStack() override;

    // 保存、关闭、恢复点之前提交属性区草稿（例如仍在编辑的注释）。
    bool CommitPendingEdits();
    [[nodiscard]] bool HasPendingCodeDraft() const;
    [[nodiscard]] bool HasPendingNavigationDraft() const;
    void DiscardPendingCodeDraft();
    [[nodiscard]] QPlainTextEdit* CodeEditor() const;
    [[nodiscard]] TriggerLuaScripts* LuaScripts() const noexcept { return luaScripts_; }
    bool ShowGlobalCustomCode();
    void ShowFindReplace();
    void NewCustomTrigger();
    bool CreateCustomTrigger(const QString& name);
    bool ConvertCurrentTriggerToCustomCode();
    // 代码转回 T：未改动时按转换前快照原样恢复，否则按代码重建并逐词核对；不一致时指出行号。
    bool ConvertCurrentCodeToTrigger();
    [[nodiscard]] Script::MapScriptInputs CodeConversionInputs();
    // 撤销 / 重做或图标资源变化后刷新，保留当前选中的节点。
    void RefreshAfterUndo();
    void RefreshEditor(StableId preferredEcaId = 0);
    void SetSearchText(const QString& text);
    // 启动探针：触发器目录与逻辑区都已载入真实内容。
    [[nodiscard]] bool HasLoadedContent() const;
    [[nodiscard]] QPlainTextEdit* CommentEditor() const;

    // 触发器目录（TriggerDirectoryActions.cpp）：对应原型 TriggerTree。全部经撤销栈，可由测试直接调用。
    // 删除前确认：title、正文与引用位置（最多列 8 条），返回是否继续。测试可替换为非模态实现。
    using ConfirmHandler = std::function<bool(const QString& title, const QString& body, const QStringList& references)>;
    void SetConfirmHandler(ConfirmHandler handler) { confirm_ = std::move(handler); }
    void NewTrigger();
    // 新建注释条目（原版“新建注释”：WTG isComment = 1，只有说明文字）。
    void NewComment();
    void NewCategory();
    void CopyCurrentTrigger();
    void PasteTrigger();
    void DuplicateCurrentTrigger();
    void DeleteCurrentItem();
    void ApplyDirectoryMove(const TriggerDirectoryMove& move);
    // 就地重命名：校验失败返回 false 并在状态栏说明；通过后排队执行（不在模型回调中重置模型）。
    bool RenameDirectoryItem(const TriggerDirectoryItem& item, const QString& name);

    // 多选（TriggerSelectionView.cpp）：Windows 式 Ctrl / Shift 选取。当前项仍决定属性区、重命名与参数编辑，
    // 批量命令作用于整个选区：选中的类别代表其中全部触发器，选中的块语句代表其中的子语句。
    // 选区按稳定 ID 记住，目录与逻辑区刷新、撤销后仍保留。
    [[nodiscard]] std::vector<TriggerDirectoryItem> SelectedDirectoryItems() const;
    // 选中这些项（目录显示顺序无关），第一项为当前项。
    void SelectDirectoryItems(const std::vector<TriggerDirectoryItem>& items);
    // 选中的语句（显示顺序，祖先也被选中时只保留祖先，分组头 / 分支头不算）；没有选区时为当前语句。
    [[nodiscard]] std::vector<StableId> SelectedEcaIds() const;
    // 选中这些语句，第一条为当前语句。
    void SelectEcas(const std::vector<StableId>& ecaIds);
    // 目录批量（TriggerDirectoryBatch.cpp）：剪切在粘贴时移动；允许 / 初始打开对选中的全部触发器生效（注释条目除外）。
    void CutSelectedTriggers();
    void SetSelectedTriggerFlags(std::optional<bool> enabled, std::optional<bool> initiallyOn);

    // 语句批量（TriggerEcaActions.cpp / TriggerEcaBatch.cpp）：都作用于选中的语句，每次操作一步撤销。
    void DeleteSelectedEca();
    void CopySelectedEca(bool cut);
    // 粘贴到当前语句之后（同一分支）；块语句分支不接受的语句类型整体拒绝。
    void PasteEca();
    // 副本放在各段选中语句之后。
    void DuplicateSelectedEca();
    void SetSelectedEcaEnabled(bool enabled);

    // 语句结构（TriggerEcaStructure.cpp）：对应原型传统视图的上移 / 下移、拖放、添加到子块。
    // 多选时只能整体移动同一分支里相邻的一段语句。
    void MoveSelectedEca(int delta);
    void ApplyEcaMove(const EcaMoveRequest& request);
    // 在位置 slot 中 beforeEcaId 之前（0 = 末尾）插入语句；返回新语句的稳定 ID（失败为 0）。
    StableId InsertEca(const EcaSlot& slot, StableId beforeEcaId, const std::string& functionName);

    // 取值面板（TriggerValueActions.cpp）：点击语句中的参数或按 Enter 打开，选择后作为一次修改入栈。
    void OpenValuePanel(StableId ecaId, std::size_t position, const QRect& anchor);
    bool EditScalarArgument(StableId ecaId, std::size_t position, const QRect& anchor);
    bool ApplyValueChoice(StableId ecaId, std::size_t position, const TriggerValueChoice& choice);
    // 参数的期望类型（Null 按类型系统解析：类型选择器 / 前一个参数；局部变量按所在触发器）。
    [[nodiscard]] std::string ResolvedArgumentType(const Trigger::EcaNode& eca, std::size_t position) const;
    [[nodiscard]] TriggerValuePanel* ValuePanel() const;

    // 视图切换（TriggerBlueprintActions.cpp）：传统视图与蓝图共享选中项与全部语句操作。
    void SetBlueprintVisible(bool visible);
    [[nodiscard]] bool BlueprintVisible() const;
    [[nodiscard]] TriggerBlueprint* Blueprint() const { return blueprint_; }
    // 最近一次蓝图右键菜单的条目文字（测试用）。
    [[nodiscard]] const QStringList& LastBlueprintMenu() const noexcept { return lastBlueprintMenu_; }

signals:
    // 注释逐字编辑未入撤销栈，但需要调度恢复点。
    void DraftEdited();
    // 请求组装处切换 UI 数据根（涉及本机设置与确认对话框）。
    void UiRootRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void BuildPage();
    void UpdateLogicColumns();
    void BuildShortcuts();
    void ReleaseDocument();
    void BindDocument();
    void OnTriggerDocumentChanged();
    void OnEditTrackingBaselineChanged(quint64 generation, quint64 revision);
    void RememberScalarUiBinding();
    void InvalidateScalarUiBinding();
    [[nodiscard]] bool CanRefreshScalarUi() const;
    void UpdateSummary();
    [[nodiscard]] const Trigger::TriggerRecord* CurrentTrigger() const;
    void SelectTrigger(StableId triggerId);
    void ShowSelectedTrigger();
    void ShowSelectedEca();
    void PopulateProperties(const Trigger::TriggerRecord* trigger);
    void CommitProperties();
    bool CommitCodeDraft();
    bool ApplyCodePlan(const Trigger::TriggerCodeEditPlan& plan, const QString& label,
        Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot = {});
    void LoadCodeTarget(Trigger::CodeBlockTarget target);
    void RefreshCodeFromDocument();
    void UpdateCodeDraftState();
    void ConfigureCodeChecking();
    void ConfigureLuaAssist();
    [[nodiscard]] bool ClassicCodeAllowed() const;
    [[nodiscard]] bool CodeAuthoringAllowed() const;
    void RefreshCodeSourceState();
    void ShowStatus(const QString& text, int timeoutMs);
    void RestoreNavigationCodeView(UiKit::CodeView* view, const QJsonObject& state);

    // 触发器目录（TriggerDirectoryActions.cpp）
    // 目录键盘：Delete、Insert / Ctrl+T、Ctrl+G、Ctrl+C / V / D、Enter（F2 与方向键由视图处理）。
    bool HandleExplorerKey(QEvent* event);
    void ShowTreeContextMenu(const QPoint& pos);
    void AddNewItemActions(QMenu& menu);
    [[nodiscard]] std::optional<TriggerDirectoryItem> CurrentDirectoryItem() const;
    [[nodiscard]] std::optional<std::int32_t> CurrentCategoryId() const;
    void SelectDirectoryItem(const TriggerDirectoryItem& item, bool edit = false);
    void DeleteDirectoryItem(const TriggerDirectoryItem& item);
    void AddDirectoryTrigger(bool comment);
    void DeleteCategory(std::int32_t categoryId);
    void PasteSnapshot(const Trigger::TriggerSnapshot& snapshot, std::int32_t categoryId, StableId afterTriggerId);
    // 粘贴出的触发器名称：原名加“ 副本”，重名时编号。
    [[nodiscard]] std::string PasteTriggerName(const std::string& base) const;
    void SetTriggerFlags(StableId triggerId, bool enabled, bool initiallyOn, const QString& label);
    // 把触发器里旧版本编辑器写法的触发器引用改成 WE 写法（LegacyTriggerReferences.h），一条撤销命令。
    void FixLegacyTriggerReferences(StableId triggerId);
    void PushDirectoryChange(Trigger::TriggerDirectoryLayout before, const QString& label);
    void ApplyRename(const TriggerDirectoryItem& item, const std::string& name);
    // 自定义代码触发器的改名、复制：走与 Agent 共用的代码身份服务（同步 InitTrig 与代码里的生成符号），一条撤销命令。
    // 成功时返回结果触发器（复制为新触发器，改名为原触发器）。
    std::optional<StableId> ApplyCustomCodeIdentity(bool copy, StableId triggerId, const std::string& name,
        std::int32_t categoryId, StableId afterTriggerId);
    // 这些触发器（gg_trg_ 名称）在它们之外的引用：最多渲染 8 行，total 为总数。
    [[nodiscard]] QStringList TriggerReferenceLines(const std::vector<StableId>& triggerIds, std::size_t& total) const;
    [[nodiscard]] bool Confirm(const QString& title, const QString& body, const QStringList& references);
    // 删除前按与 Agent 删除门禁同一审计查代码里的引用；作者取消或检查期间地图有变化时返回空（已提示原因）。
    [[nodiscard]] std::optional<QtProject::RemovalCodeNotice> CheckRemovalCode(const std::vector<StableId>& triggerIds);
    // 目录整体刷新：保留折叠状态、当前项与滚动位置；selectTriggerId 非 0 时改选该触发器。
    void RefreshExplorer(StableId selectTriggerId = 0);
    void ApplyExplorerExpansion();
    // 展开的类别显示打开的文件夹图标。
    void UpdateCategoryIcons();
    // 信息条（允许 / 初始打开 / 注释）
    void UpdateMetaBar(const Trigger::TriggerRecord* trigger);
    [[nodiscard]] QString ScriptRebuildNotice();
    // 提示与转换共用的工具链查询缓存，按工具/UI 根失效；include 文件是否存在仍每次实查。
    void EnsureScriptCapability(const std::filesystem::path& toolRoot);
    [[nodiscard]] const std::vector<std::filesystem::path>& ScriptSystemIncludes(const std::filesystem::path& toolRoot);
    // 函数库管理（TriggerTreeActions.cpp）：应用后按新的有效层序重新载入触发器。
    void OpenUiLibraries();
    [[nodiscard]] std::string TriggerReplaceRevision() const;
    [[nodiscard]] Status PrepareTriggerReplaceSources();
    void InvalidateTriggerReplace(bool sourcesChanged = false);

    // ECA 编辑（TriggerEcaActions.cpp）
    void ShowEcaContextMenu(const QPoint& pos);
    void ShowEcaMenuAt(const QModelIndex& index, const QPoint& globalPos);
    void BuildBlueprint(QWidget* pane, QVBoxLayout* layout, QHBoxLayout* heading);
    void SyncBlueprint();
    // 蓝图端口连线：把语句移到端口对应的位置（类型不符时提示）；右键菜单（TriggerBlueprintActions.cpp）。
    void ConnectBlueprint(StableId ecaId, const EcaSlot& slot, StableId beforeEcaId);
    void ShowBlueprintMenu(StableId ecaId, const std::vector<TriggerBlueprintDetail::PinTarget>& targets, const QPoint& globalPos);
    // 语句本身的命令（编辑参数 / 启用 / 复制粘贴 / 定位 / 删除），传统视图与蓝图的右键菜单共用。
    void AddEcaCommands(QMenu& menu, StableId ecaId, std::vector<std::pair<QAction*, std::function<void()>>>& actions);
    void AddEca(int ecaType);
    void EditSelectedEca();
    void EditEcaParameters(StableId ecaId);
    // 应用一组参数修改（先挂接函数、再改标量），整图验证不过全部回滚；prefix 为同一撤销宏中先压入的命令。
    bool ApplyArgumentEdits(StableId ecaId, const std::vector<TriggerArgumentEditRequest>& requests, const QString& label,
        std::unique_ptr<QUndoCommand> prefix, std::optional<StableId>* newNestedEcaId);
    void EditFirstArgument();
    [[nodiscard]] std::vector<TriggerValueObject> ObjectsFor(const std::string& uiType) const;
    // 函数列表的分类图标来源（工程的游戏 / YDWE / 地图图标服务）。
    [[nodiscard]] std::function<QIcon(const QString& categoryId)> CategoryIcons() const;
    // 打开语句选择器，把所选语句插入 slot（TriggerEcaStructure.cpp）。
    void ChooseAndInsertEca(const EcaSlot& slot, StableId beforeEcaId);
    void AddEcaStructureMenu(QMenu& menu, const QModelIndex& index, std::vector<std::pair<QAction*, std::function<void()>>>& actions);
    // 编辑后的整图验证；失败时写诊断并返回 false，调用方负责回滚。
    [[nodiscard]] bool ValidateOrReport();
    // 改过的参数的引用资格（ApplyArgumentEdits 内调用）；返回空表示通过，否则为拒绝原因。
    [[nodiscard]] QString CheckChangedReferences(StableId ecaId, const std::vector<StableId>& changed);

    // 多选（TriggerSelectionView.cpp）
    void ConfigureMultiSelection();
    void OnExplorerCurrentChanged(const QModelIndex& current);
    void CaptureExplorerSelection();
    void CaptureEcaSelection();
    // 按记住的稳定 ID 恢复选区，current 为当前项；选区只有当前项时与单选行为相同。
    void RestoreExplorerSelection(const QModelIndex& current);
    void RestoreEcaSelection(const QModelIndex& current);
    [[nodiscard]] Trigger::DirectorySelection NormalizedDirectorySelection() const;
    // 选区代表的全部触发器（目录显示顺序）。
    [[nodiscard]] std::vector<StableId> SelectedTriggerIds() const;
    // 选中的全部语句行（含被选中祖先之下单独选中的子语句），用于允许 / 禁用。
    [[nodiscard]] std::vector<StableId> SelectedEcaRowIds() const;
    // Shift 单独点击或 Shift+方向键：从锚点重新选一段（不保留之前 Ctrl 选的项）；Ctrl+A 全选。
    bool HandleSelectionInput(QObject* watched, QEvent* event);

    // 目录批量（TriggerDirectoryBatch.cpp）
    struct TriggerPasteGroup {
        std::int32_t categoryId = 0;
        StableId afterTriggerId = 0;
        std::vector<Trigger::TriggerSnapshot> snapshots;
    };
    void AddDirectorySelectionActions(QMenu& menu, const std::vector<TriggerDirectoryItem>& items,
        std::vector<std::pair<QAction*, std::function<void()>>>& actions);
    void DeleteDirectoryItems(const std::vector<TriggerDirectoryItem>& items);
    void PasteTriggerGroups(std::vector<TriggerPasteGroup> groups, const QString& label);
    void PasteCutTriggers();
    void ApplyDirectoryMoves(const TriggerDirectoryMove& move);

    // 语句批量（TriggerEcaBatch.cpp）
    struct EcaPastePlan {
        StableId anchorEcaId = 0; // 粘贴到它之后、同一分支
        std::vector<const Trigger::TriggerDocumentEditor::EcaSubtreeSnapshot*> snapshots;
    };
    void DeleteEcas(const std::vector<StableId>& ecaIds);
    bool PasteEcaPlans(const std::vector<EcaPastePlan>& plans, const QString& label);
    void MoveSelectedEcaBlock(int delta);
    void ApplyEcaMoves(const EcaMoveRequest& request);

    // 地图脚本 war3map.j 只读查看（TriggerMapScript.cpp）
    void BuildMapScriptView(QWidget* pane, QVBoxLayout* layout);
    [[nodiscard]] std::optional<QByteArray> ReadMapScriptBytes() const;
    void ShowMapScript(std::optional<QByteArray> prepared = std::nullopt);
    void LeaveMapScript();

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QTabWidget* workspaceTabs_ = nullptr;
    TriggerLuaScripts* luaScripts_ = nullptr;
    int lastWorkspaceTab_ = 0;
    QLabel* summary_ = nullptr;
    QTreeView* explorerView_ = nullptr;
    QTreeView* ecaView_ = nullptr;
    QPointer<QWidget> logicViewport_;
    static constexpr int kLogicTypeColumnWidth = 96;
    static constexpr int kMaximumLogicColumnWidth = 8192;
    bool manualLogicColumns_ = false;
    bool updatingLogicColumns_ = false;
    bool logicColumnsUpdateQueued_ = false;
    TriggerExplorerModel* explorerModel_ = nullptr;
    TriggerEcaModel* ecaModel_ = nullptr;
    std::shared_ptr<const QtProject::detail::TriggerScalarUiNotice> pendingScalarUiNotice_;
    QPointer<QUndoStack> scalarUiStack_;
    std::uint64_t pendingScalarUiModelEpoch_ = 0;
    const Trigger::TriggerDocument* scalarUiDocument_ = nullptr;
    const Trigger::UiCatalog* scalarUiCatalog_ = nullptr;
    std::uint64_t scalarUiGeneration_ = 0, scalarUiRevision_ = 0, scalarUiNavigationGeneration_ = 0;
    QSortFilterProxyModel* filter_ = nullptr;
    QPushButton* addEventButton_ = nullptr;
    QPushButton* addConditionButton_ = nullptr;
    QPushButton* addActionButton_ = nullptr;
    QPushButton* editEcaButton_ = nullptr;
    QLineEdit* explorerSearch_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QCheckBox* initiallyOnCheck_ = nullptr;
    QCheckBox* runOnInitCheck_ = nullptr;
    QWidget* propertiesBox_ = nullptr;
    QLabel* metaNote_ = nullptr;
    QString reportedScriptNotice_; // 已写入错误列表的“不能保存”警告，避免同一地图重复写入
    bool scriptCapabilityCached_ = false;
    std::filesystem::path scriptCapabilityToolRoot_;
    std::filesystem::path scriptCapabilityUiRoot_;
    QString scriptCapabilityNotice_;
    QString scriptCapabilityDetails_;
    std::vector<std::filesystem::path> scriptCapabilityIncludes_;
    QToolButton* commentToggle_ = nullptr;
    QPlainTextEdit* commentEdit_ = nullptr;
    UiKit::CodeView* scriptView_ = nullptr;
    UiKit::CodeProblemsView* codeProblems_ = nullptr;
    QWidget* codeTools_ = nullptr;
    QComboBox* codeLanguage_ = nullptr;
    // Lua 代码块：接口目录与地图 Lua 文件，补全与检查共用；不是 Lua 时为空。
    std::shared_ptr<const Services::CodeCheckLuaSources> codeLuaSources_;
    // 代码悬停的函数库说明，按函数库身份缓存。
    QHash<QString, QString> codeSymbolNotes_;
    std::string codeSymbolNotesCatalog_;
    QWidget* codeActions_ = nullptr;
    QPushButton* applyCodeButton_ = nullptr;
    QPushButton* discardCodeButton_ = nullptr;
    QPushButton* convertCodeButton_ = nullptr;
    QPushButton* recoverCodeButton_ = nullptr;
    // 本次会话转换为代码前的 GUI 快照，按触发器身份保存；文档释放时清空。
    QLabel* codeDraftNote_ = nullptr;
    std::optional<Trigger::CodeBlockTarget> codeTarget_;
    std::optional<Trigger::TriggerCodeEditPlan> codeBaselinePlan_;
    // Core 原正文只用于密封未改计划；Qt 显示基线允许 QTextDocument 的 Unicode 规范化。
    QString codeBaselineText_;
    QString codeDisplayBaselineText_;
    std::optional<std::uint64_t> codeDisplayLoadGeneration_;
    bool sourceDisplayLossless_ = true;
    std::string codeSourceSettingsSha256_;
    bool codeNativeLua_ = false;
    bool suppressCodeSignals_ = false;
    bool publishingCode_ = false;
    bool codeDraftPending_ = false;
    bool showingGlobalCode_ = false;
    QLabel* logicTitle_ = nullptr;
    QLabel* logicHint_ = nullptr;
    QPushButton* mapScriptButton_ = nullptr;
    UiKit::CodeView* mapScriptView_ = nullptr;
    UiKit::CodeProblemsView* mapScriptProblems_ = nullptr;
    QWidget* mapScriptTools_ = nullptr;
    QCheckBox* mapScriptRawBytes_ = nullptr;
    QByteArray mapScriptBytes_;
    TriggerSentenceDelegate* sentenceDelegate_ = nullptr;
    TriggerBlueprint* blueprint_ = nullptr;
    QToolButton* traditionalButton_ = nullptr;
    QToolButton* blueprintButton_ = nullptr;
    bool syncingSelection_ = false;
    QStringList lastBlueprintMenu_;
    // 参数中的四字码 → 物体名称；传统视图与蓝图共用。
    std::function<Trigger::ReferencePresentation(std::string_view, std::string_view)> referenceResolver_;
    QCheckBox* showRawValues_ = nullptr;
    QPointer<TriggerValuePanel> valuePanel_;
    QPointer<TriggerReplaceDialog> triggerReplaceDialog_;
    // 取消后仍等旧 owned worker 真正结束；关闭/重建面板也不会并行准备两份大图。
    std::shared_ptr<std::atomic_bool> triggerReplaceBusy_ = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::stop_source> triggerReplaceStop_;
    std::unique_ptr<Trigger::TriggerTypeSystem> triggerReplaceTypes_;
    const Trigger::UiCatalog* triggerReplaceCatalog_ = nullptr;
    std::string triggerReplaceCatalogSha_;
    std::unique_ptr<MapDocument> triggerReplaceStrings_;
    std::string triggerReplacePreparedRevision_;
    std::uint64_t triggerReplaceMapRevision_ = 0;
    bool triggerReplaceMapPrepared_ = false;
    std::uint64_t triggerReplaceSourceEpoch_ = 0;
    bool showingMapScript_ = false;
    bool suppressPropertySignals_ = false;
    StableId propertyTriggerId_ = 0;
    QString propertyCommentBaseline_;
    // ECA 子树剪贴板（应用内），多选时按显示顺序。
    std::vector<Trigger::TriggerDocumentEditor::EcaSubtreeSnapshot> clipboard_;
    // 触发器剪贴板：整个触发器的快照（目录顺序），源触发器删除后仍可粘贴。
    std::vector<Trigger::TriggerSnapshot> triggerClipboard_;
    // 剪切的触发器：粘贴时移到当前类别（文档在粘贴前不变）。
    std::vector<StableId> triggerCut_;
    // 多选：目录与逻辑区按稳定 ID 记住的选区；恢复期间的选区变化不回写。
    Trigger::DirectorySelection explorerSelection_;
    std::vector<StableId> ecaSelection_;
    bool restoringExplorerSelection_ = false;
    bool restoringEcaSelection_ = false;
    // 撤销 / 重做后的刷新：保留多选，不因当前语句改选。
    bool keepEcaSelection_ = false;
    // 折叠的类别（按 categoryId）；刷新目录后据此恢复。默认全部展开，与原型一致。
    std::set<std::int32_t> collapsedCategories_;
    bool refreshingExplorer_ = false;
    // 目录当前项（不从视图索引反推：文档刚改动、模型尚未刷新时视图索引会指向错误的行）。
    StableId selectedTrigger_ = 0;
    StableId displayedTrigger_ = 0;
    // 当前项所在类别：当前触发器被删除（例如撤销新建）后退回到这个类别。
    std::optional<std::int32_t> selectionCategory_;
    std::optional<std::int32_t> selectedCategory_;
    ConfirmHandler confirm_;
    QJsonObject pendingViewState_;
    // 只用于丢弃晚到的视图恢复；不持有文档正文、模型索引或裸回调控件。
    std::uint64_t navigationRestoreGeneration_ = 0;
    std::uint64_t navigationDocumentGeneration_ = 0;
    QMetaObject::Connection navigationTextLoaded_;
};

} // namespace War3Map::Modules

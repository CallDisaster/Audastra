#pragma once

#include "War3MapCore/Types.h"
#include "EditorProject.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Services/CommandRegistry.h"
#include "War3MapCore/Services/ProjectSearch.h"
#include "War3MapCore/Services/MapSizeAnalysis.h"
#include "War3MapCore/Services/LuaTestReloadController.h"
#include "ProjectSearchCapture.h"
#include "War3MapCore/Services/ProjectChanges.h"
#include "Theme/ThemeManager.h"
#include "Widgets/CommandPalette.h"
#include "EditorParts.h"
#include "Shell/ShellWindow.h"
#include "RecentMapsStore.h"
#include "RecentMapsList.h"
#include <QPointer>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace War3Map::Modules { class AssetsModule; class ObjectModule; class TriggerModule; class VariablesModule; }
namespace War3Map::Modules { class WorldFrameRecorder; }
namespace War3Map::QtFeedback { class FeedbackClient; class ProgramIssueReporter; }
namespace War3Map::QtUpdate { class UpdateClient; struct PreparedUpdate; }
namespace War3Map::Script { class MapTestLaunchLease; }
namespace War3Map::Services { struct MapCreateOptions; }
namespace War3Map::Agent { struct RecoveryPreferences; }
namespace War3Map::QtProject { class LegacyMapBackupCheck; }

class QCloseEvent;
class QDialog;
class QDockWidget;
class ChangesPanel;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;
class QTableView;
class QTabWidget;
class QTextBrowser;
class QTreeView;
class QTimer;
class QThread;
class QFileSystemWatcher;
class QToolBar;
class QUndoStack;
class ObjectFieldModel;
class ObjectFieldDelegate;
class ObjectListModel;
class TriggerEcaModel;
class TriggerExplorerModel;

namespace War3Map {
class EditorSession;
class MapArchiveSession;
class MapDocument;
class DirectoryWorkspace;

namespace QtApp {
class IconProvider;
class AiWorkspace;
class LiveEditorTools;
class GlobalSearchDialog;
class MapSizeDialog;
}

namespace ObjectData {
class LniObjectDocument;
class NativeObjectDocument;
class StandardObjectCatalog;
class ObjectFieldSchema;
}

namespace Trigger {
class TriggerDocument;
class UiCatalog;
struct EcaNode;
struct TriggerRecord;
}
} // namespace War3Map

class QAction;

// 编辑器主窗口：外壳（顶栏模块切换器 + 停靠区）加工程生命周期的组装。
// 模块、工程与撤销栈在 EditorParts 中先于外壳构造；这里只负责顶栏操作、状态栏与打开 / 保存流程。
class MainWindow final : private EditorParts, public War3Map::Shell::ShellWindow {
    friend class NativeWorkbenchTests;
    friend class ProjectWatchTests;
public:
    enum class StartupMode {
        Normal,
        HeadlessProbe,
        StartupProbe,
    };

    explicit MainWindow(QWidget* parent = nullptr);
    explicit MainWindow(StartupMode startupMode, QWidget* parent = nullptr);
    // 测试/嵌入宿主可注入独立的最近地图目录，构造时不读取默认用户历史。
    MainWindow(StartupMode startupMode, QWidget* parent, const QString& recentMapsDataDirectory);
    ~MainWindow() override;

    // 自动化和命令行入口使用同一条只读打开路径，不绕过 GUI 的安全门禁。
    bool OpenWorkspacePath(const std::filesystem::path& path);
    void ActivateModule(const War3Map::Shell::ModuleId& id) override;
    void Navigate(const War3Map::Shell::NavigationTarget& target) override;

    // 无界面启动门禁：必须同时载入触发器模型、单位对象列表及其字段模型。
    [[nodiscard]] bool HasLoadedTriggerAndObjectModels() const noexcept;

    // 直接打开 w3x/w3m 地图归档（MPQ）：只读提取到临时工作目录后走标准管线。
    bool OpenMapArchivePath(const std::filesystem::path& archivePath);
    // 只有真实用户打开成功才登记；默认外部/探针/工程私有基线入口不登记历史。
    bool OpenUserMapArchivePath(const std::filesystem::path& archivePath);
    bool OpenCampaignPath(const std::filesystem::path& path);

    // 将当前工作区重打包为 .w3x（另存为）。
    bool SaveAsMapArchive(War3Map::Services::TerrainMinimapSaveChoice minimapChoice =
        War3Map::Services::TerrainMinimapSaveChoice::Automatic);
    // 无模态旁路写入；仅在完整输出成功后提交来源路径、基线和脏状态。
    bool SaveMapArchiveToPath(const std::filesystem::path& outputPath,
        War3Map::Services::TerrainMinimapSaveChoice minimapChoice = War3Map::Services::TerrainMinimapSaveChoice::Automatic);
    // 地图保存（MapSaveActions.cpp）：background 为真时在工作线程执行并显示进度框。
    bool RunMapSave(const std::filesystem::path& target, bool inPlace, bool background, bool testSnapshot = false,
        bool testHotReload = false,
        War3Map::Services::TerrainMinimapSaveChoice minimapChoice = War3Map::Services::TerrainMinimapSaveChoice::Automatic);
    bool SaveEditorProjectToPath(const std::filesystem::path& outputPath, bool recovery = false);
    bool OpenEditorProjectPath(const std::filesystem::path& path);

    // 会话级资源入口；命令行使用时不改变用户保存的路径。
    bool OpenGameResources(const std::filesystem::path& path);
    bool UseUiDataRoot(const std::filesystem::path& path);
    // 已载入内容的逐行摘要（无界面探针输出与验收记录）。
    [[nodiscard]] QStringList LoadedStateSummary() const;
    // 无界面验收：做一组真实触发器编辑后另存到 outputPath（AcceptanceProbe.cpp）。
    bool ProbeEditAndSave(const std::filesystem::path& outputPath, QStringList& log);
    bool ProbeMapDataEditAndSave(const std::filesystem::path& outputPath, QStringList& log,
        bool projectOnly = false);
    // 七类物编各改一个自定义对象后另存并回读核对（AcceptanceObjectProbe.cpp）。
    bool ProbeObjectEditAndSave(const std::filesystem::path& outputPath, QStringList& log);
    // 世界页交互探针：屏幕外窗口内持续播放、合成点选/框选/镜头旋转并记录（WorldProbeActions.cpp）。
    bool CaptureUi(const std::filesystem::path& directory, QStringList& log);
    bool ProbeWorldInteraction(int seconds, QStringList& log, bool passive = false, int readyTimeoutSeconds = 180,
        const std::filesystem::path& captureOutput = {}, bool frameOnly = false, bool modelListOnly = false,
        const QString& focusHostKey = {}, std::optional<std::uint32_t> sequenceIndex = {},
        std::optional<std::uint64_t> elapsedMs = {}, std::optional<std::uint64_t> globalMs = {});
    // 作者主动开启的可见被动测量；后台参数只用于隔离诊断，不构成可见验收。
    bool EnableWorldMeasurement(const QString& newOutputDirectory, int backgroundSeconds = 0, bool cameraTour = false);

private:
    void SetupCampaignActions();
    void OpenCampaignChapterPath(const QString& path);
    War3Map::QtApp::AiWorkspace* aiWorkspace_ = nullptr;
    void SetupAgentWorkspace(bool readUserConfiguration = true);
    // 仅离屏探针的 friend 测试使用：同工作区 HTTP，临时端口与内存凭据。
    quint16 StartAgentHttpProbe(const QByteArray& token, War3Map::QtApp::LiveEditorTools*& tools, QString& error);
    void ShowAgentWindow();
    void ToggleAgentWorkspace();
    [[nodiscard]] bool AgentWorkspaceVisible() const;
    void SyncAssistantAction();
    // 三种呈现复用同一会话；mode 对应 AgentWindow::PresentationMode。
    void PresentAgentWorkspace(int mode);
    void HideAgentWorkspace();
    void ShowApplicationSettings();
    void SetupProductServices();
    void ReportProgramSaveFailures(int firstDiagnostic);
    void ShowFeedback();
    void ShowUpdates();
    void BeginProductUpdate(const War3Map::QtUpdate::PreparedUpdate& prepared);
    [[nodiscard]] bool CommitProductUpdateExit();
    [[nodiscard]] QString FeedbackConversationSnippet() const;
    War3Map::QtFeedback::FeedbackClient* feedbackClient_ = nullptr;
    std::shared_ptr<War3Map::QtFeedback::ProgramIssueReporter> programIssueReporter_;
    War3Map::QtUpdate::UpdateClient* updateClient_ = nullptr;
    std::function<bool()> productUpdateExit_;
    QThread* productWorker_ = nullptr;
    QPointer<QDialog> productUpdateNotice_;
    void RefreshAgentMapContext();
    void StopAgentWorkspace();
    War3Map::Modules::WorldFrameRecorder* worldFrameRecorder_ = nullptr;
    struct ArchiveOpenBinding;
    bool OpenWorkspacePathImpl(const std::filesystem::path& path, ArchiveOpenBinding* archive);
    // 顶栏、菜单、状态栏与诊断面板（MainWindowChrome.cpp）。
    void BuildHeader();
    void UpdateWindowTitle();
    void SetupStartupPage();
    void SetupRecentMapsMenu(QMenu* fileMenu);
    void BindRecentMapsList(War3Map::Shell::RecentMapsList* list);
    void RefreshRecentMaps();
    void PublishRecentMaps(const War3Map::QtProject::RecentMapsResult& result);
    void RecordUserMap(const std::filesystem::path& path);
    void ShowRecentMapsManager();
    void ShowBaseWorkspace();
    void DismissStartupPage();
    void ShowAbout();
    void ShowHelp();
    void BuildStatusBar();
    void BuildDiagnosticsView();
    void ConnectModules();
    void SetProjectTitle(const QString& title, const QString& tooltip);
    void RefreshVariablesPage();
    void ApplyTheme();
    void OpenWorkspace();
    void OpenMapArchive();
    void CreateNewMap();
    void SaveAsMap();
    void SaveMapInPlace();
    void TestMap();
    // 启动单人测试；成功返回空串，否则返回原因。interactive 为 false 时不弹窗（供助手调用）。
    QString StartMapTest(bool interactive);
    // 本局测试仍对应当前地图：生成的触发器 Lua 要求修订不变；手写 Lua 文件按内容比对，应用草稿后仍可热重载。
    [[nodiscard]] bool LuaTestSessionCurrent() const;
    QJsonObject LuaTestReload(bool submit);
    void CreateLuaTutorialMap();
    void RunMapCreate(War3Map::Services::MapCreateOptions options, const QString& output);
    // 作者手写 Lua（exec-lua 入口）的热重载：按文件把测试开始后改过的 .lua 送进游戏。
    QJsonObject LuaFileReload(bool submit);
    void SnapshotLuaFileBaseline();
    void PollLuaTestReload();
    void SaveEditorProject();
    void SaveEditorProjectAs();
    void OpenEditorProject();
    void CheckGameBuild();
    void ShowCapabilities();
    void LoadMapData();
    void ShowMapData();
    void ScheduleRecovery();
    void ApplyRecoveryPreferences(const War3Map::Agent::RecoveryPreferences& preferences);
    void WriteRecovery();
    [[nodiscard]] bool WaitForRecoveryExit();
    void RestoreRecovery();
    void SetupProjectNotices();
    void ResetProjectWatch();
    void RefreshProjectWatch();
    bool CheckProjectSources();
    void ShowRecoveryNotice();
    void UpdateProjectNotice();
    [[nodiscard]] std::optional<War3Map::EditScopeSnapshot> CaptureEditScopes() const;
    void ResetEditTracking();
    bool ObserveEditState(const std::string& description);
    void ObserveObjectEdit(int typeIndex);
    void ObserveTriggerEdit();
    void UpdateEditDirtyFlags();
    void CommitMapEditBaseline();
    void SelectUiDataRoot();
    void SelectLuaMapTarget();
    void ClearLuaMapTarget();
    void SelectGameRoot();
    void SelectExtraIconDir();
    void LoadUiDataRootFromSettings();
    void LoadGameRootFromSettings();
    bool ApplyGameRoot(const std::filesystem::path& war3Root, bool announce);
    void ValidateWorkspace();
    void ExportCandidate();
    void UpdateExportAvailability();
    void SetupCommandPalette();
    void SetupMapFileDrop();
    void ShowCommandPalette(const QString& initial = {});
    void ShowGlobalSearch();
    void ShowMapSizeAnalysis();
    [[nodiscard]] War3Map::Result<War3Map::Services::MapSizeReport> QueryMapSizeAnalysis(std::string_view expectedRevision);
    void ActivateMapSizeRow(const War3Map::Services::MapSizeRow& row, std::string_view expectedRevision);
    void ActivateSearchImportedFile(const War3Map::Services::ProjectSearchRow& row);
    [[nodiscard]] War3Map::Result<War3Map::Services::ProjectSearchPage> QueryProjectSearchPage(
        std::string_view expectedRevision, std::string_view query,
        const War3Map::Services::ProjectSearchOptions& options, std::size_t pageLimit,
        const War3Map::Services::ProjectSearchCursor* cursor = nullptr);
    void RegisterCommandAction(const std::string& id, QAction* action, const QString& group);
    void ExecuteCommand(const std::string& id);
    void InvalidateCommandContext();
    [[nodiscard]] QString RefreshCommandMapSnapshot();
    void SetupChangesPanel();
    void RefreshChangesPanel();
    void ScheduleChangesRefresh();
    void ShowChangesPanel();
    void RevertProjectChange(const War3Map::Services::ProjectChangeRow& row);
    void RevealProjectChange(const War3Map::Services::ProjectChangeRow& row);
    void CheckChangesSave();
    [[nodiscard]] std::string CommandContextRevision() const;
    [[nodiscard]] std::vector<CommandPalette::Entry> QueryCommandPalette(const QString& query, bool commandsOnly);
    void ActivateSearchResult(const War3Map::Services::ProjectSearchRow& row);
    void UpdateBreadcrumb();
    void UpdateStatusBar();
    void AddDiagnostic(
        const QString& level,
        const QString& subsystem,
        const QString& code,
        const QString& message);
    void ResetProjectState();
    [[nodiscard]] bool ConfirmDiscardPendingEdits(const QString& action, bool allowDiscard = true);

    QLineEdit* globalSearch_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* saveMapAction_ = nullptr;
    QAction* saveMapInPlaceAction_ = nullptr;
    QAction* buildMapAction_ = nullptr;
    QAction* testMapAction_ = nullptr;
    std::shared_ptr<War3Map::Script::MapTestLaunchLease> mapTestLease_;
    QAction* luaReloadAction_ = nullptr;
    QAction* luaAutoReloadAction_ = nullptr;
    std::optional<War3Map::Script::LuaTriggerUnit> luaTestGenerated_;
    std::optional<std::vector<std::string>> luaTestInstalledBindings_;
    std::string luaTestMapBinding_;
    std::shared_ptr<War3Map::Services::LuaTestReloadController> luaReloadController_;
    std::shared_ptr<War3Map::Services::LuaTestMailboxSession> luaRuntimeMailbox_;
    std::uint64_t luaTestGeneration_ = 0;
    std::filesystem::path luaTestSource_; // 启动测试时打开的地图
    std::optional<std::array<std::uint64_t, 2>> luaReloadSubmittedRevision_;
    QTimer* luaReloadPoll_ = nullptr;
    std::uint32_t luaRuntimeDiagnosticSequence_ = 0;
    QString luaReloadLastStatus_;
    struct LuaFilePending { QString path; std::string module, source, sha256; };
    bool luaFileReload_ = false;
    std::map<QString, std::string> luaFileBaseline_; // AssetKey → 游戏中当前正文的 SHA
    std::vector<LuaFilePending> luaFileQueue_;       // 首项为已发送、等待游戏确认的文件
    std::uint32_t luaFileSequence_ = 0;
    QString luaFileLastMessage_;
    QAction* exportAction_ = nullptr;
    QAction* mapDataAction_ = nullptr;
    QToolBar* projectNoticeBar_ = nullptr;
    QLabel* projectNoticeText_ = nullptr;
    QPushButton* projectNoticeAction_ = nullptr;
    QPushButton* projectNoticeDismiss_ = nullptr;
    [[nodiscard]] std::string ProjectFingerprint() const;
    StartupMode startupMode_ = StartupMode::Normal;
    War3Map::QtProject::LegacyMapBackupCheck* legacyMapBackupCheck_ = nullptr;
    bool autoRecoveryEnabled_ = true;
    bool writingRecovery_ = false;
    bool recoveryRequested_ = false;
    QThread* recoveryWorker_ = nullptr;
    std::shared_ptr<War3Map::QtProject::ProjectSaveJob> recoveryJob_;
    bool recoveryExitWaiting_ = false;
    // 仅自有 HeadlessProbe 夹具注入启动前异常；正常用户会话不调用。
    std::function<void(int)> recoveryStartupStageHook_;
    std::unique_ptr<War3Map::QtProject::RecentMapsStore> recentMapsStore_;
    QVector<War3Map::Shell::RecentMapListEntry> recentMapRows_;
    QString recentMapsMessage_;
    QPointer<War3Map::Shell::RecentMapsList> startupRecentMaps_;
    QPointer<War3Map::Shell::RecentMapsList> menuRecentMaps_;
    QPointer<War3Map::Shell::RecentMapsList> managedRecentMaps_;
    QPointer<QDialog> recentMapsManager_;
    QMenu* recentMapsMenu_ = nullptr;
    QStackedWidget* workspacePages_ = nullptr;
    QWidget* startupPage_ = nullptr;
    bool startupDismissed_ = false;
    War3Map::UiKit::ThemeManager* themeManager_ = nullptr;
    CommandPalette* commandPalette_ = nullptr;
    QPointer<War3Map::QtApp::GlobalSearchDialog> globalSearchDialog_;
    QPointer<War3Map::QtApp::MapSizeDialog> mapSizeDialog_;
    std::unique_ptr<War3Map::QtApp::ProjectSearchCapture> projectSearchCapture_;
    War3Map::Services::CommandRegistry commandRegistry_;
    std::string commandWindowId_;
    std::uint64_t commandContextGeneration_ = 1;
    std::string commandMapRevision_;
    std::unique_ptr<War3Map::MapDocument> commandMapDocument_;
    std::unique_ptr<War3Map::Services::MapDataQuerySnapshot> commandMapSnapshot_;
    QDockWidget* changesDock_ = nullptr;
    ChangesPanel* changesPanel_ = nullptr;
    QAction* changesAction_ = nullptr;
    bool changesRefreshQueued_ = false;
    bool changesChecking_ = false;
    bool mapDropQueued_ = false;
    QLabel* breadcrumb_ = nullptr;
    QLabel* statusModCount_ = nullptr;
    QLabel* statusConfig_ = nullptr;

    using PendingImport = War3Map::QtProject::PendingImport;
};

#pragma once

#include "Icons/IconProvider.h"

#include "War3MapCore/EditRevision.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Types.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Services/ObjectDeletionAssessment.h"
#include "War3MapCore/Services/ProjectChanges.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"
#include "War3MapCore/ProjectStore.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"
#include "War3MapCore/Services/MapScriptTargetSelection.h"
#include "War3MapCore/Trigger/GuiConversionSnapshots.h"

#include <array>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

class QFileSystemWatcher;
class QJsonArray;
class QStandardItemModel;
class QTimer;
class QTemporaryDir;
class QUndoStack;
class QUndoCommand;

namespace War3Map {
class DirectoryWorkspace;
class EditorSession;
class MapArchiveSession;
class MapDocument;
namespace Services { struct CodeCheckProjectSnapshot; struct CodeCheckResult; }
namespace MapData { class MapDataEditSession; }

namespace ObjectData {
class LniObjectDocument;
class NativeObjectDocument;
class ObjectFieldSchema;
class StandardObjectCatalog;
} // namespace ObjectData

namespace Game {
class WorldEditStrings;
} // namespace Game

namespace Trigger {
class MapObjectReferenceSnapshot;
class TriggerDocument;
class UiCatalog;
class TriggerGraphScalarEditReceipt;
} // namespace Trigger
} // namespace War3Map

namespace War3Map::QtProject {

class TriggerGraphSnapshotUndoCommand;
namespace detail { struct TriggerScalarUiNotice; struct TriggerUiBaseline; }

inline constexpr std::size_t kObjectTypeCount = 7;

// 会话级待导入文件。
struct PendingImport {
    QString archivePath; // 归档内路径
    QString diskPath;    // 磁盘来源路径
    qint64 size = 0;     // 文件大小
    std::shared_ptr<const std::vector<std::byte>> ownedPayload; // 工程拥有的不可变正文，不借用临时磁盘文件。
    bool runtimeLuaDraft = false; // 未确认运行时档案的草稿；改名和替换不能清除此保存门禁。
    [[nodiscard]] bool operator==(const PendingImport&) const = default;
};

// 地图已有成员的路径修改（保存时生效）：to 非空为改名 / 移动，为空为删除。from 是源归档里的原路径。
struct AssetEdit {
    QString from;
    QString to;
    [[nodiscard]] bool operator==(const AssetEdit&) const = default;
};

// 资源浏览器的一行：当前（含未保存修改）的归档路径。
struct AssetEntry {
    QString path;         // 当前归档路径
    QString originalPath; // 源归档中的路径；待导入文件为空
    qint64 size = 0;
    QString diskPath;     // 待导入文件的磁盘来源
    bool ambiguous = false; // 规范路径具有多份来源时保留诊断，不挑第一份写入。
    std::shared_ptr<const std::vector<std::byte>> ownedPayload;
    bool runtimeLuaDraft = false;
    [[nodiscard]] bool Pending() const noexcept { return ownedPayload || !diskPath.isEmpty(); }
    [[nodiscard]] bool Replacement() const noexcept { return Pending() && !originalPath.isEmpty(); }
};

// 资源编辑的完整状态（撤销 / 重做用）。
struct AssetState {
    std::vector<PendingImport> imports;
    std::vector<AssetEdit> edits;
    [[nodiscard]] bool operator==(const AssetState&) const = default;
};

// 地图保存（写回或另存）成功后的结果。
struct MapCopySaveResult {
    std::filesystem::path target;
    std::size_t memberCount = 0;
    bool savedTriggerChanges = false; // 触发器已保存但游戏脚本未重建（工具链不可用）
    bool scriptRebuilt = false;       // 游戏脚本已重建并通过语法检查
    bool inPlace = false;             // 写回了打开的地图
    std::filesystem::path backup;     // 写回时原图的备份
    double seconds = 0;               // 整个保存耗时
};

struct MapSaveJob; // MapSaveJob.h
class ProjectSaveJob; // ProjectSaveJob.h

// 结构验证结果（诊断已写入诊断模型）。
struct ValidationSummary {
    bool opened = false;
    int objectTypesPassed = 0;
    int objectTypesFailed = 0;
    bool triggersValidated = false;
    bool triggersPassed = false;
    [[nodiscard]] bool Passed() const { return opened && objectTypesFailed == 0 && (!triggersValidated || triggersPassed); }
};

// 当前打开的地图工程：文档、会话、修订与本机资源。界面模块只通过它读写数据，
// 自身只保存视图状态。
// P1 拆分过渡期：字段暂时公开，工程生命周期（打开 / 保存 / 恢复 / 监视）迁入后改为私有接口。
// 来源监视器已由工程 QObject 拥有；其余 QObject 裸指针仍借用 QtApp 对象树所有权，不能重复删除；
// 下一步迁入生命周期服务时一并迁移其创建/停止/析构顺序。
class EditorProject final : public QObject {
    Q_OBJECT
public:
    EditorProject();
    ~EditorProject();
    EditorProject(const EditorProject&) = delete;
    EditorProject& operator=(const EditorProject&) = delete;

    // 由工程层计算来源/文档/导入字节身份，多个 UI 模块共用同一快照。
    [[nodiscard]] std::optional<EditScopeSnapshot> CaptureEditScopes() const;
    [[nodiscard]] std::optional<EditScopeSnapshot> CaptureMapEditScopes() const;
    [[nodiscard]] std::string ProjectFingerprint() const;
    // 保存准备阶段使用 CaptureEditScopes 已冻结的导入 SHA，避免再次读盘产生混合身份。
    [[nodiscard]] std::string ProjectFingerprint(std::span<const std::string> importSha256) const;

    // 不依赖任何窗口的编辑修订生命周期；保存输出验证成功后才提交地图基线。
    void ResetEditTracking();
    [[nodiscard]] bool ObserveEditState(const std::string& description);
    // 仅用于已知只修改公共地图成员的事务；保存/恢复边界仍用完整ObserveEditState。
    [[nodiscard]] bool ObserveMapDataEdit(const std::string& description);
    void ObserveObjectEdit(int typeIndex);
    void ObserveTriggerEdit();
    // 撤销/重做已知结果状态的触发器指纹时提供给下一次观察，免去整图写出；只在同一文档、
    // 同一撤销栈且栈位置一致时采用。完整核验模式下仍实算比对。
    void OfferKnownTriggerFingerprint(int stackIndex, std::string fingerprint);
    [[nodiscard]] std::optional<std::string> ObservedTriggerFingerprint() const;
    void UpdateEditDirtyFlags();
    void CommitMapEditBaseline();

    // 文件事件只标疑似变更；显式复核保留全部诊断，不触发自动重载或保存。
    void ResetProjectWatch();
    void RefreshProjectWatch();
    [[nodiscard]] Status CheckProjectSources();
    // 调用方先提交当前 UI 草稿；输出与基线提交均由工程层负责。
    [[nodiscard]] Result<ProjectSaveResult> SaveProjectToPath(
        const std::filesystem::path& target, bool recovery = false);
    [[nodiscard]] Result<std::unique_ptr<ProjectSaveJob>> PrepareProjectSave(
        const std::filesystem::path& target, bool recovery = false,
        std::optional<std::string> expectedRecoveryTargetSha256 = std::nullopt,
        std::shared_ptr<ProjectSaveControl> control = {});
    static void RunProjectSave(ProjectSaveJob& job);
    [[nodiscard]] Result<ProjectSaveResult> CommitProjectSave(ProjectSaveJob& job);
    // UI-only；仅尚未 Run 的任务能取消。取消结束 manual 票据，不触碰新 generation。
    [[nodiscard]] Status CancelPreparedProjectSave(ProjectSaveJob& job);
    // 可与 worker 并发；只请求 recovery 在发布前协作取消，不读写文档或保存票据。
    [[nodiscard]] bool RequestProjectSaveCancellation(ProjectSaveJob& job) noexcept;
    // 调用方先清理旧命令；所有模块在替换前解除对旧文档的绑定。
    void ReplaceTriggerDocument(std::unique_ptr<Trigger::TriggerDocument> document);

    // 诊断（ProjectDiagnostics.cpp）：所有环节写入同一模型，界面只显示。
    [[nodiscard]] static QString DiagnosticLevel(DiagnosticSeverity severity);
    void AddDiagnostic(const QString& level, const QString& subsystem, const QString& code, const QString& message);
    void AddDiagnostics(const std::vector<Diagnostic>& diagnostics);
    void ClearDiagnostics();

    // 触发器工作区（TriggerWorkspace.cpp）。
    [[nodiscard]] static bool HasUiCatalogConfig(const std::filesystem::path& candidate);
    [[nodiscard]] std::filesystem::path ResolveUiDataRoot() const;
    [[nodiscard]] std::filesystem::path UiToolRoot() const;
    // 按当前 UI Catalog 合并图标映射（分类→纹理、函数→分类）并设置函数库图标的磁盘来源；
    // 不预设任何第三方库。游戏目录、UI 数据根或目录变化后调用。
    void RefreshUiIconMappings();
    // 函数目录与文档一起替换，替换前后发出 TriggerDocumentAboutToChange / TriggerDocumentChanged。
    void ReplaceTriggerWorkspace(std::unique_ptr<Trigger::UiCatalog> catalog,
        std::unique_ptr<Trigger::TriggerDocument> document);
    // 从工作区 map/ 读取 WTG/WCT；失败原因写入 triggerStatus_，诊断写入诊断模型。
    bool LoadTriggers();

    // 显示文本字符串表（ProjectStrings.cpp）：YDWE 覆盖层 → 游戏 MPQ → 英文原版，触发器与物编共用。
    // 数据根或游戏目录变化后重新载入；只影响显示，不改变保存与脚本生成。
    [[nodiscard]] std::shared_ptr<const Game::WorldEditStrings> CurrentWorldEditStrings();
    // 缓存首次查询时订阅真实命令栈，覆盖Undo后新Push复用相同index/count的分支。
    [[nodiscard]] std::uint64_t ContentCacheEpoch() const;
    // 只冻结当前语义结构并按修订缓存；磁盘、基础库与预处理在后台检查入口读取。
    [[nodiscard]] std::shared_ptr<const Services::CodeCheckProjectSnapshot> CaptureCodeCheckSnapshot() const;
    [[nodiscard]] bool IsCodeCheckBindingCurrent(std::string_view binding) const;
    void AdoptCodeCheckIndex(std::shared_ptr<const Services::CodeCheckResult> index);
    [[nodiscard]] std::shared_ptr<const Services::CodeCheckResult> CurrentCodeCheckIndex() const;
    void ApplyWorldEditStrings(Trigger::UiCatalog& catalog);
    // 游戏目录或数据根变化后调用：重新挂到当前目录，变化时发出 WorldEditStringsChanged。
    void RefreshWorldEditStrings();

    // UI 函数库两层启用（UiLibraryState.cpp）。全局设置按数据根保存在本机设置中，不修改 config。
    struct ParsedTriggers {
        std::unique_ptr<Trigger::UiCatalog> catalog;
        std::unique_ptr<Trigger::TriggerDocument> document;
    };
    [[nodiscard]] static UiLibrary::GlobalSettings ReadUiLibrarySettings(const std::filesystem::path& uiRoot);
    static void WriteUiLibrarySettings(const std::filesystem::path& uiRoot, const UiLibrary::GlobalSettings& settings);
    [[nodiscard]] Result<Trigger::UiCatalog> LoadEffectiveUiCatalog(const std::filesystem::path& uiRoot);
    // 地图引用了本机已关闭的包时，仅对本图启用实际用到的包后重新解析。
    [[nodiscard]] std::optional<ParsedTriggers> ParseWithLocallyDisabledLibraries(
        const std::filesystem::path& uiRoot, const std::vector<std::byte>& wtg, const std::vector<std::byte>& wct);
    void RecordUiLibraryUsage();
    [[nodiscard]] QJsonArray UiLibraryUsageJson() const;
    // 保存全局设置并按新的有效层序重新载入触发器（调用方先确认放弃未保存的触发器修改）。
    bool ApplyUiLibrarySettings(const UiLibrary::GlobalSettings& settings, std::vector<std::string> mapOnly);
    // 函数库热重载（UiLibraryWatch.cpp，架构 §4.6）：监视当前数据根下各包的 .txt/.j/.cfg 与 config；
    // 内容指纹变化后，没有未导出的触发器修改时重新载入触发器（重新绑定定义），否则只发出提示。
    void WatchUiLibraries();
    // 与上次载入时的包清单比较，返回内容变化 / 新增 / 消失的包 ID。
    [[nodiscard]] std::vector<std::string> DetectUiLibraryChanges() const;

    // 地图数据（ProjectMapData.cpp）：按当前来源重新读取只读地图文档。
    void LoadMapDocument(bool afterMapSave = false);
    // 从当前来源（归档会话或展开目录）读取地图成员；来源不明确时报错，不在多个来源间猜测。
    [[nodiscard]] Result<std::vector<std::byte>> ReadMapMember(const std::string& name) const;
    // 只用于当前已打开地图；保持自然语言T，明确选择完整Lua保存后端。
    [[nodiscard]] Status ConfigureLuaMapTarget(const std::filesystem::path& configuration);
    [[nodiscard]] Status ClearLuaMapTarget();
    // 仅从已扫描的展开目录快照读取成员；map/<name> 与 <name> 同时存在时视为来源不明确。
    [[nodiscard]] static Result<std::vector<std::byte>> ReadWorkspaceMember(const DirectoryWorkspace& workspace, const std::string& name);
    [[nodiscard]] MapData::MapDataEditSession* MapEdits() noexcept { return mapEdits_.get(); }
    [[nodiscard]] const MapData::MapDataEditSession* MapEdits() const noexcept { return mapEdits_.get(); }
    // 工程级引用索引（ProjectReferences.cpp）：文档替换或撤销栈变化后在下次查询时重建。
    [[nodiscard]] const Services::ProjectReferenceIndex& References() const;
    [[nodiscard]] Services::ObjectDeletionAssessment AssessDeletion(
        const ObjectData::ObjectKey& subject) const;
    [[nodiscard]] qint64 LastReferenceBuildMs() const noexcept { return referenceBuildMs_; }
    // 每次重建递增；视图据此判断缓存的行是否过期。
    [[nodiscard]] std::uint64_t ReferenceGeneration() const { (void)References(); return referenceGeneration_; }

    // 载入状态摘要（ProjectSummary.cpp）：无界面探针与验收记录共用。
    [[nodiscard]] QStringList DescribeLoadedState() const;
    // 地图保存分三段，调用方先提交界面草稿；任何一段失败时当前基线与修改保持不变。
    //   PrepareMapSave（界面线程，MapCopySave.cpp）：序列化修改、决定是否重建脚本，生成保存任务；
    //   RunMapSave（可在工作线程，MapSaveRun.cpp）：重建脚本、打包、验证、写回或另存，只读任务里的输入；
    //   CommitMapSave（界面线程）：写入诊断，切换到新基线。运行期间 savingMap_ 为真，界面不得修改工程。
    // inPlace 为真时写回打开的地图（原图移入备份目录），target 被忽略。
    [[nodiscard]] std::unique_ptr<MapSaveJob> PrepareMapSave(const std::filesystem::path& target, bool inPlace);
    [[nodiscard]] Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>> CaptureTriggerMapObjectReferences() const;
    // 同一冻结准备路径，无输出目标；正式预检仍可在专属临时目录构建脚本。
    [[nodiscard]] std::unique_ptr<MapSaveJob> PrepareMapPreflight();
    // 精确当前绑定由宿主复核；经共用逆计划作为一次普通地图数据编辑入栈。
    [[nodiscard]] Status RevertProjectChange(const Services::ProjectChangeRow& row,
        const Services::MapDataQueryBinding& currentBinding);
    [[nodiscard]] Result<Services::ProjectContentChangesContext> CaptureContentChangesContext(
        const Services::MapDataQueryBinding& binding) const;
    static void RunMapSave(MapSaveJob& job);
    [[nodiscard]] std::optional<MapCopySaveResult> CommitMapSave(MapSaveJob& job);
    // 同步执行三段（无界面探针与测试使用）。
    [[nodiscard]] std::optional<MapCopySaveResult> SaveMapCopy(const std::filesystem::path& outputPath);
    [[nodiscard]] std::optional<MapCopySaveResult> SaveMapInPlace();
    // 是否有尚未写入地图的修改（触发器、物编、资源）。
    [[nodiscard]] bool HasUnsavedMapChanges() const;
    // 游戏脚本重建（MapScriptRebuild.cpp）：按当前触发器与编辑后的物编生成整图脚本，经注入 / Wave / 模板 /
    // jasshelper + pjass 检查。准备阶段在界面线程决定工具链是否可用（不可用返回 Skipped），执行阶段在任务里运行。
    enum class ScriptRebuild { Rebuilt, Skipped, Failed };
    [[nodiscard]] ScriptRebuild PrepareScriptRebuild(MapSaveJob& job);
    bool savingMap_ = false;
    qint64 sourceWatchQuietUntil_ = 0; // 写回地图后短时间内忽略来源监视事件（自己的替换操作）
    // 结构验证与旁路候选（ProjectValidation.cpp）。
    ValidationSummary ValidateDocuments();
    [[nodiscard]] bool HasPendingCandidateEdits() const;
    bool ExportCandidate(const std::filesystem::path& outputPath);

    // 导入队列（ImportQueue.cpp）。用户编辑发 ImportsEdited（调度恢复点），任何变化发 ImportsChanged。
    [[nodiscard]] static QString SuggestImportPath(const QString& diskPath);
    // folder 非空时放进该文件夹，否则按文件类型建议路径。
    int AddImportFiles(const QStringList& diskPaths, const QString& folder = {});
    int AddImportFiles(AssetState& state, const QStringList& diskPaths, const QString& folder = {}) const;
    void RemoveImports(std::vector<int> rows);
    void ClearImports();
    // 资源路径编辑（AssetEdits.cpp）：改名 / 移动 / 删除地图已有成员或待导入文件。
    // 返回空字符串表示成功，否则为不能修改的原因（非法路径、重名、编辑器管理的文件）。
    [[nodiscard]] QString MoveAsset(const QString& from, const QString& to);
    [[nodiscard]] QString RemoveAsset(const QString& path);
    // 私有候选状态规划供资源与 Lua 页面共用；这些重载不发布信号或修改当前工程。
    [[nodiscard]] QString MoveAsset(AssetState& state, const QString& from, const QString& to) const;
    [[nodiscard]] QString RemoveAsset(AssetState& state, const QString& path) const;
    // 搜索等只读调用可请求当前序列的有界前缀；原无参数调用仍取完整资源列表。
    [[nodiscard]] std::vector<AssetEntry> AssetEntries(
        std::size_t maxEntries = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] std::vector<AssetEntry> AssetEntries(const AssetState& state,
        std::size_t maxEntries = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] bool HasPendingAssetEdits() const noexcept { return !pendingImports_.empty() || !assetEdits_.empty(); }
    // 归档路径是否可以由用户放置资源；不可以时返回原因。
    [[nodiscard]] static QString CheckAssetPath(const QString& path);
    [[nodiscard]] static QString AssetKey(const QString& path);
    [[nodiscard]] AssetState CaptureAssets() const;
    [[nodiscard]] QUndoStack* AssetUndoStack() const noexcept { return assetUndoStack_; }
    [[nodiscard]] QString RecordAssetEdit(const QString& label,
        const std::function<QString(AssetState&)>& prepare);
    // 收据已准备好的完整状态通过一次 swap 发布；正文保存仍走原导入队列。
    void PublishAssetState(AssetState state);
    // 按钮图标对应的暂停图标路径（游戏按“CommandButtonsDisabled\DIS + 文件名”查找）；不是 BTN/PAS 图标时为空。
    [[nodiscard]] static QString DisabledIconPath(const QString& iconPath);
    // 缺少暂停图标的按钮图标（地图与游戏数据里都没有对应的 DIS 文件）；保存时自动生成。
    [[nodiscard]] QStringList MissingDisabledIcons() const;
    void RestoreAssets(const AssetState& state);
    // 队列或归档会话被整体替换（打开 / 重置 / 保存）后调用，界面据此刷新。
    void NotifyImportsChanged(bool userEdit = false);

    // 只优化同次成功标量发布的 UI 刷新；不是编辑、观察或保存资格，不能由 DTO 创建。
    using ScalarUiNotice = std::shared_ptr<const detail::TriggerScalarUiNotice>;
    [[nodiscard]] ScalarUiNotice ActiveScalarUiNotice(const Trigger::TriggerDocument* boundDocument,
        const Trigger::UiCatalog* boundCatalog, std::uint64_t boundGeneration, std::uint64_t boundRevision,
        const QObject* consumer) const;
    [[nodiscard]] bool ScalarUiNoticeMatchesIndex(const ScalarUiNotice& notice,
        const QUndoStack* stack, int index, const QObject* consumer) const;
    // 仅供冷路径 UI 重新绑定：冻结的成功 Reset 来源事实，不授编辑或观察权限。
    [[nodiscard]] bool IsEditTrackingUiBaselineCurrent(std::uint64_t generation, std::uint64_t revision) const;

signals:
    void ObjectDocumentsAboutToReplace();
    void ObjectDocumentsReplaced();
    void SourceStateChanged();
    void TriggerDocumentAboutToChange();
    void TriggerDocumentChanged();
    void EditTrackingBaselineChanged(quint64 generation, quint64 revision);
    void ImportsChanged();
    void MapDocumentChanged();
    void ImportsEdited();
    // 函数库文件在磁盘上变化；reloaded 表示已按新定义重新载入触发器。
    void UiLibrariesChanged(const QStringList& packages, bool reloaded);
    // 显示文本字符串表换了（游戏目录或数据根变化）；界面重绘即可，文档不变。
    void WorldEditStringsChanged();

public:
    // 先声明以便后析构：归档/文档/图标释放后才删除恢复提取目录。
    std::shared_ptr<QTemporaryDir> restoredFiles_;
    // 归档打开派生的地图工作目录，独立于 .forge 的解包源；只回收本会话新建目录。
    std::shared_ptr<QTemporaryDir> mapWorkspace_;

    // ── 工作区与来源 ──
    std::filesystem::path projectRoot_;
    std::filesystem::path sourceArchivePath_; // 打开的 w3x/w3m 源归档路径
    // 工程/恢复点记录的作者原地图路径。原图已变或缺失时会话只能打开临时基线，此处保留原路径供另存为与下一次恢复点沿用。
    std::filesystem::path recordedSourceMapPath_;
    // 当前地图来源是否为恢复工程解出的临时基线（不是作者的地图，“保存地图”不能写回这里）。
    [[nodiscard]] bool MapSourceIsRecoveryCopy() const;
    // 写入工程清单的原地图路径：临时基线会话沿用记录值，否则为当前打开的地图。
    [[nodiscard]] std::filesystem::path AuthorMapPath() const;
    std::unique_ptr<EditorSession> session_;
    std::shared_ptr<MapArchiveSession> archiveSession_;
    std::unique_ptr<MapDocument> mapDocument_;
    std::unique_ptr<MapData::MapDataEditSession> mapEdits_;
    QUndoStack* mapDataUndoStack_ = nullptr; // 设置、实例、声音共用一次序列化编辑历史。
    std::unique_ptr<DirectoryWorkspace> directorySnapshot_;

    // ── 触发器 ──
    // 用户持久化配置的外部 UI 数据根；切换项目时不清空。
    std::filesystem::path uiDataRoot_;
    // 当前项目实际使用的 UI 根；可以是项目内 ui/config 层或本次显式选择。
    std::filesystem::path activeUiDataRoot_;
    // 工程内嵌定义用于编辑；地图脚本依赖本机实际编译器头文件。
    std::filesystem::path compilerDataRoot_;
    // 一般来源档案的owned逐文件声明；保存时进入同一Capture/Verify/Replay，不授予读取资格。
    std::vector<Script::ScriptProfileSourceEncoding> scriptSourceEncodings_;
    std::vector<Script::ScriptProfileIncludeAccess> scriptSourceIncludeAccess_;
    std::filesystem::path scriptSourceEncodingsFile_; // 配置被移除时撤下该文件提交的声明。
    std::optional<Services::LuaMapTargetRequest> luaMapTarget_;
    std::filesystem::path luaMapTargetSource_;
    std::uint64_t luaMapTargetGeneration_ = 0;
    std::unique_ptr<Trigger::UiCatalog> uiCatalog_;
    std::unique_ptr<Trigger::TriggerDocument> triggerDocument_;
    Trigger::GuiConversionSnapshots guiConversionSnapshots_;
    QUndoStack* triggerUndoStack_ = nullptr;
    bool triggerDirty_ = false;
    bool restoredTriggerOverlay_ = false;

    // ── 物体数据 ──
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kObjectTypeCount> objectDocuments_;
    // native 物体基线文档（另存为写回的回灌目标），与 objectDocuments_ 同索引。
    std::array<std::unique_ptr<ObjectData::NativeObjectDocument>, kObjectTypeCount> nativeObjectDocuments_;
    std::array<bool, kObjectTypeCount> objectDirty_{};
    std::array<QUndoStack*, kObjectTypeCount> objectUndoStacks_{};
    std::unique_ptr<ObjectData::StandardObjectCatalog> standardCatalog_;
    std::unique_ptr<ObjectData::ObjectFieldSchema> objectFieldSchema_;

    // ── 导入资源 ──
    std::vector<PendingImport> pendingImports_;
    std::vector<AssetEdit> assetEdits_;
    QUndoStack* assetUndoStack_ = nullptr;

    // ── 工程文件、修订与恢复 ──
    QTimer* recoveryTimer_ = nullptr;
    std::filesystem::path recoveryPath_;
    std::string recoveryFileSha256_; // 上一有效恢复回执；下一次更新不在 UI 重新散列整包。
    std::string lastProjectFingerprint_;
    std::filesystem::path projectFilePath_;
    std::string projectFileSha256_;
    std::string restoredProjectJournalJson_;
    EditRevision editRevision_;
    bool editTrackingReady_ = false;
    bool editTrackingFailed_ = false;

    // ── 来源监视 ──
    QFileSystemWatcher* projectWatch_ = nullptr;
    bool sourceChangeSuspected_ = false;
    bool sourceChangeConfirmed_ = false;
    bool projectWatchLimited_ = false;
    bool projectNoticeDismissed_ = false;
    std::filesystem::path recoveryNoticePath_;

    // ── 本机资源 ──
    std::filesystem::path war3GameRoot_; // 魔兽争霸 3 安装目录（图标资源）
    std::filesystem::path extraIconDir_; // 额外磁盘图标目录（YDWE 自带纹理回退）
    std::unique_ptr<QtApp::IconProvider> iconProvider_;
    std::shared_ptr<const Game::WorldEditStrings> worldEditStrings_;
    std::filesystem::path worldEditStringsToolRoot_; // 载入字符串表时的数据根与游戏目录
    std::filesystem::path worldEditStringsGameRoot_;

    // 触发器未就绪时给界面显示的原因；成功载入后为空。
    QString triggerStatus_;

    // ── UI 函数库 ──
    std::vector<UiLibrary::PackageInfo> uiLibraryPackages_;
    UiLibrary::GlobalSettings uiLibraryGlobal_;
    std::vector<std::string> uiLibraryMapOnly_;      // 仅对当前地图启用的包
    std::optional<UiLibrary::MapUsage> uiLibraryUsage_;
    QFileSystemWatcher* uiLibraryWatcher_ = nullptr;
    QTimer* uiLibraryDebounce_ = nullptr;

private:
    // 只在成功打开/保存提交时捕获一次，不在修改面板刷新时复制整份文档。
    void CaptureContentChangesBaseline();
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kObjectTypeCount> changeObjectBaselines_;
    std::array<bool, kObjectTypeCount> changeObjectBaselineKnown_{};
    std::unique_ptr<Trigger::TriggerDocument> changeTriggerBaseline_;
    bool changeTriggerBaselineKnown_ = false;
    std::uint64_t changeBaselineGeneration_ = 0;
    EditScopeSnapshot changeContentBaselineScopes_;
    [[nodiscard]] std::string ReferenceStamp() const;
    mutable std::unique_ptr<Services::ProjectReferenceIndex> referenceIndex_;
    mutable std::string referenceStamp_;
    mutable qint64 referenceBuildMs_ = 0;
    mutable std::uint64_t referenceGeneration_ = 0;
    mutable std::string codeCheckStamp_;
    [[nodiscard]] std::string CodeCheckSourceStamp() const;
    mutable std::shared_ptr<const Services::CodeCheckProjectSnapshot> codeCheckSnapshot_;
    std::shared_ptr<const Services::CodeCheckResult> codeCheckIndex_;
    mutable std::uint64_t contentCacheEpoch_ = 0;
    mutable std::uint64_t contentCacheEditGeneration_ = (std::numeric_limits<std::uint64_t>::max)();
    mutable std::uint64_t contentCacheEditRevision_ = (std::numeric_limits<std::uint64_t>::max)();
    mutable std::vector<QPointer<QUndoStack>> contentCacheStacks_;

public:

    // ── 诊断 ──
    QStandardItemModel* diagnosticsModel_ = nullptr;

private:
    void InitializeDiagnostics();
    friend class TriggerGraphSnapshotUndoCommand;
    // 仅标量 Undo 命令调用：实际 Core Replay 成功后立即复用同次已验证的配对摘要。
    // 不接收摘要 DTO 或外置 success 标记，不在两步骤间发信号或处理事件。
    [[nodiscard]] Status ReplayScalarAndObserve(Trigger::TriggerDocument* expectedDocument,
        const Trigger::UiCatalog* expectedCatalog, std::uint64_t expectedGeneration,
        const Trigger::TriggerGraphScalarEditReceipt& receipt, bool forward);
    // 只由本命令在实际 Core 成功且 FirstPublication 已置位的直线尾段调用。
    // scope 自有存储在 emit 删除 Project 后也可安全关闭，不回写已释放 QObject 成员。
    void PublishScalarTriggerChange(const QUndoCommand* command, std::uint64_t beforeGeneration,
        std::uint64_t beforeRevision, bool forward);
    std::shared_ptr<detail::TriggerScalarUiNotice> scalarUiNotice_;
    struct KnownTriggerFingerprint { const void* document = nullptr; const QUndoStack* stack = nullptr; int index = -1; std::string value; };
    std::optional<KnownTriggerFingerprint> knownTriggerFingerprint_;
    std::shared_ptr<const detail::TriggerUiBaseline> editTrackingUiBaseline_;
    [[nodiscard]] std::unique_ptr<MapSaveJob> PrepareMapSaveImpl(
        const std::filesystem::path& target, bool inPlace, bool contentOnly);
    [[nodiscard]] std::optional<std::string> CaptureTriggerFingerprint() const;
};

} // namespace War3Map::QtProject

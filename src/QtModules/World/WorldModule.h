#pragma once

#include "Modules/EditorModule.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainPreviewMesh.h"
#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/Services/WorldSelection.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "WorldEnvironment.h"
#include "WorldSelectionProperties.h"
#include "WorldAnimationDiagnostics.h"
#include "WorldPlacementModels.h"
#include "WorldPlacementPending.h"
#include "WorldInstanceTiming.h"
#include "RenderPluginFailure.h"

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QJsonObject>
#include <QImage>
#include <map>

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QDoubleSpinBox;
class QSlider;
class QSpinBox;
class QSplitter;
class QMainWindow;
class QDockWidget;
class QStackedWidget;
class QThread;
class QTreeView;
class QModelIndex;
class QVariantAnimation;
class QVBoxLayout;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp { class ObjectCatalogWidget; struct ObjectViewEntry; }
namespace War3Map::UiKit { class ElidedLabel; class SlidingHighlight; }
namespace War3Map::Render { struct TerrainGroundMesh; struct AnimatedModelScene; struct CapturedFrameRgba; class AnimationDiagnosticQuery; }

namespace War3Map::Modules {
class WorldViewport;
class WorldAnimationPlayer;
class WorldPlacementPreview;
class WorldSelectionPresenter;
class WorldBuildingSplatPresenter;
class WorldModelBugPlaceholderPresenter;
class WorldPremPresenter;
class WorldOutlinerModel;
class WorldAssetBrowser;
struct WorldTerrainCache;
struct WorldTerrainLoaded;
struct WorldPlacementPresentationCandidate;
struct WorldPathingHelperCache;
struct WorldPathingHelperPrepared;

using WorldTerrainMesh = Render::TerrainPreviewMesh;

// 地形快照到预览网格的只读适配。大地图按固定步长抽样，始终保留末行/末列。
[[nodiscard]] Result<WorldTerrainMesh> BuildWorldTerrainMesh(const MapData::TerrainGridSnapshot& terrain);
// 诊断列表中第一条错误的文本（没有错误时给通用说明）。
[[nodiscard]] QString WorldDiagnosticText(const std::vector<Diagnostic>& diagnostics);

struct WorldModelDiagnosticsCapture {
    QImage image;
    QJsonObject binding;
    QString exportText;
};

class WorldModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
public:
    explicit WorldModule(QtProject::EditorProject& project, const War3RenderPluginApiV2* backend = nullptr);
    ~WorldModule() override;
    [[nodiscard]] Shell::ModuleInfo Info() const override;
    [[nodiscard]] QWidget* Widget() override;
    [[nodiscard]] QUndoStack* UndoStack() override; // 与实例、区域、设置共用地图数据撤销栈
    void AttachHost(Shell::ModuleHost* host) override { host_ = host; }
    [[nodiscard]] bool Reveal(const Shell::NavigationTarget& target) override;
    [[nodiscard]] QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    [[nodiscard]] std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    [[nodiscard]] Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    [[nodiscard]] bool PrepareNavigationLeave() override;
    [[nodiscard]] WorldViewport* Viewport() const noexcept { return viewport_; }
    [[nodiscard]] WorldAnimationPlayer* AnimationPlayer() const noexcept { return animation_; }
    // 读取真实控件/播放器状态；保留的旧画面不能替代服务就绪或完成帧证据。
    [[nodiscard]] QJsonObject PlaybackDiagnosticSnapshot() const;
    // GUI线程只读captured字段/当前接受场景；不触发资源读取或GPU操作。
    [[nodiscard]] QJsonObject ModelDiagnosticSnapshot() const;
    // 廉价等待门；不构造全量列表、不访问资源。真实捕获另核完成的同一来源/修订。
    [[nodiscard]] bool ModelDiagnosticsReady() const;
    [[nodiscard]] Result<WorldModelDiagnosticsCapture> CaptureModelDiagnosticsView();
    // capture须来自成功的Viewport完成帧读回；只核同次层与当前typed计划，不制造GPU回执。
    // 不授予可见、点选或游戏签收。
    [[nodiscard]] Result<QJsonObject> ModelBugPlaceholderCaptureSnapshot(const Render::CapturedFrameRgba& capture) const;
    [[nodiscard]] Result<QJsonObject> ModelPremCaptureSnapshot(const Render::CapturedFrameRgba& capture) const;
    void ApplyEffectQualityFromSettings();
    void Refresh();

signals:
    void RenderPluginFailed(const War3Map::Modules::RenderPluginFailure& failure);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] Result<std::vector<std::byte>> ReadTerrain() const;
    // 地形显示后在后台读取摆放模型；换代时丢弃过期结果。
    void StartModels(std::uint64_t generation);
    void StartAnimatedModels(std::uint64_t generation);
    void StartPlacementPreview();
    void StartStaticModels(std::uint64_t generation, QString animationReason = {});
    void ReportModelPreparationFailure(const QString& stage, const std::vector<Diagnostic>& diagnostics);
    void SetupAnimationControls();
    void BindInstanceDiagnostics();
    void RefreshInstanceDiagnostics();
    void OpenModelDiagnostics();
    [[nodiscard]] std::vector<WorldModelDiagnosticRow> ModelDiagnosticRows() const;
    void RefreshEffectBudgetStatus();
    [[nodiscard]] bool InstanceDiagnosticsCurrent() const;
    [[nodiscard]] QString InstanceDiagnosticText(const Services::WorldSelectionVersion& expected,
        const MapData::PlacementId& id) const;
    // 当前地图文档：有编辑会话时取其快照（含未保存的摆放/区域/镜头修改）。
    [[nodiscard]] std::optional<MapDocument> CurrentDocument() const;
    // 文档编辑/撤销/重做：静态路径原位重排；动画场景重建一次新身份与静态包。
    // 临时拖动和播放只传动态矩阵。区域/镜头叠加与选择一并更新。
    void RefreshPlacements();
    bool StagePlacementInstances(const MapDocument& document, const WorldPlacementRefreshTimings& refresh = {});
    void QueuePlacementInstances();
    [[nodiscard]] std::optional<Services::WorldInstanceBinding> CurrentPlacementBinding() const;
    void RememberCreatedPlacements(std::span<const MapData::PlacementId> rows,
        std::span<const Render::ModelInstanceRequest> requests);
    void UpdateSelectionCircles();
    void RefreshBuildingSplats(bool rebindParent = false, const MapDocument* capturedDocument = nullptr);
    [[nodiscard]] Services::WorldSelectionVersion PlacementPresentationVersion() const;
    void RefreshMapEdits();
    [[nodiscard]] bool RefreshEnvironment(); // 已改为完整重新读取时返回 true
    void CaptureAuxiliary(const MapDocument& document);
    // 界面框架（WorldPanels.cpp）：顶栏工具、左侧工具面板（场景目录 / 摆放 / 地形笔刷）、状态栏。
    enum class Tool { Select, Place, Terrain };
    void BuildToolbar(QVBoxLayout* layout);
    void UpdateToolbarLayout();
    [[nodiscard]] QWidget* BuildToolPanel();
    [[nodiscard]] QWidget* BuildSelectPage();
    [[nodiscard]] QWidget* BuildPlacePage();
    [[nodiscard]] QWidget* BuildTerrainPage();
    void BuildStatusBar(QVBoxLayout* layout);
    void ConnectEditing();
    // 当前工具：地形笔刷为等待重建暂停时仍算地形工具，面板和选中底块不跳动。
    [[nodiscard]] Tool CurrentTool() const;
    void UpdateToolState();
    void UpdateToolHint();
    void UpdateStatusIndicators();
    void RefreshOutliner(std::shared_ptr<const MapDocument> owned = {});
    void SyncOutlinerSelection();
    void FilterOutliner(const QString& text);
    void ActivateOutlinerItem(const QModelIndex& index, bool center = false);
    [[nodiscard]] Services::WorldSelectionContext SelectionContext() const;
    bool RefreshSelectionContext(std::shared_ptr<const MapDocument> owned = {});
    void ApplySelectionSnapshot(const Services::WorldSelectionSnapshot& snapshot);
    bool SelectObjects(std::span<const Services::WorldObjectId> ids, Services::SelectionMode mode);
    [[nodiscard]] bool SelectionEditable() const;
    [[nodiscard]] std::uint64_t SelectionRevision() const;
    void SelectAllWorldObjects();
    void NudgeSelection(double dx, double dy);
    void DuplicateSelection();
    void RefreshAssetBrowser();
    void FlyTo(double x, double y, std::optional<double> distance);
    void UpdateBrushCursor(std::optional<std::array<double, 2>> point);
    void RefreshTerrainPalette();
    void RefreshTerrainSwatches();
    void UpdateTerrainBrushControls();
    void EditTerrainTextureSlot(bool cliff, bool append);
    [[nodiscard]] MapData::TerrainBrushKind CurrentBrushKind() const;
    [[nodiscard]] QString TerrainName(const std::string& raw, const std::string& rawcode) const;
    // 右侧属性（WorldInspector.cpp）：选中对象的可编辑属性与地图概况。
    [[nodiscard]] QWidget* BuildInspector();
    void UpdateInspector(bool animate = false);
    void CommitPlacementFields();
    void CommitUnitAttributeField(QWidget* field);
    void CommitRegionFields();
    void CommitCameraFields();
    void OpenSelectedDefinition();
    void ResetSceneFacts();
    void ShowSceneFacts(const WorldTerrainLoaded& loaded, const Render::TerrainGroundMesh* ground, const QString& missingGround);
    // 编辑交互（WorldModuleEditing.cpp）：选择、拖动、删除、摆放、叠加线与状态文本。
    void StartTerrainStroke(double x, double y);
    void ExtendTerrainStroke(double x, double y);
    void FinishTerrainStroke();
    void CancelTerrainStroke();
    void QueueTerrainRender(std::shared_ptr<const MapData::TerrainGridSnapshot> source, bool preview);
    void QueueWaterFrame(std::uint32_t frame);
    void UpdateWaterFrameControl();
    void StartTerrainRender();
    [[nodiscard]] bool PresentTerrainRender(WorldTerrainLoaded& loaded, bool preview, bool waterFrameOnly,
        std::shared_ptr<const MapData::TerrainGridSnapshot> source);
    void ClearSelection();
    void SelectInstance(int instance, int modifiers);
    void SelectBox(const QVector<int>& instances, int modifiers);
    void PresentSelection();
    void SelectGround(double x, double y, int modifiers);
    [[nodiscard]] bool HandleAt(double x, double y) const;
    void StartDrag(double x, double y);
    void DragSelection(double x, double y);
    void StartRotation(double x, double y);
    void RotateSelection(double x, double y);
    void FinishDrag();
    void CancelDrag();
    void DeleteSelection();
    void DuplicateSelectedAuxiliary();
    void ChoosePlacement(const QtApp::ObjectViewEntry& entry);
    void ActivatePlacementTool();
    void PlaceAt(double x, double y, bool snap);
    void StopPlacing();
    void CopySelection();
    void StartPaste();
    void PasteAt(double x, double y);
    void PreviewAt(double x, double y, double z, bool snap);
    void HidePreview();
    void UpdatePlacementPreview();
    void RefreshPalette();
    void ValidateSelection();
    void UpdateOverlay();
    void UpdateSelectionText();
    // 选中摆放在 requests_ 中的序号、在当前场景中的实例序号（未显示为空）。
    [[nodiscard]] std::optional<std::uint32_t> SelectedRequest() const;
    [[nodiscard]] std::optional<std::uint32_t> InstanceOfRequest(std::uint32_t request) const;
    [[nodiscard]] const MapData::RegionRecord* SelectedRegion() const;
    [[nodiscard]] const MapData::CameraRecord* SelectedCamera() const;
    QtProject::EditorProject& project_;
    const War3RenderPluginApiV2* backend_ = nullptr;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    WorldViewport* viewport_ = nullptr;
    WorldAnimationPlayer* animation_ = nullptr;
    WorldPlacementPreview* placementPreview_ = nullptr;
    QPushButton* animationPlay_ = nullptr;
    QPushButton* animationPause_ = nullptr;
    QPushButton* animationStep_ = nullptr;
    // 顶栏：工具分段（选择 / 摆放 / 地形）与叠加显示开关。
    QPushButton* selectTool_ = nullptr;
    QPushButton* placeToggle_ = nullptr;
    QPushButton* terrainToggle_ = nullptr;
    UiKit::SlidingHighlight* toolIndicator_ = nullptr;
    QPushButton* showRegions_ = nullptr;
    QPushButton* showCameras_ = nullptr;
    QComboBox* rotationStep_ = nullptr;
    QMainWindow* worldDockHost_ = nullptr;
    QDockWidget* toolDock_ = nullptr;
    QDockWidget* assetDock_ = nullptr;
    QDockWidget* inspectorDock_ = nullptr;
    QJsonObject retainedViewState_;
    std::optional<WorldPlacementRotationOrigin> rotationOrigin_;
    std::optional<Services::WorldSelectionVersion> dragSelectionVersion_;
    QString dragMapKey_;
    std::uint64_t dragGeneration_ = 0;
    // 左侧工具面板
    QLabel* toolTitle_ = nullptr;
    QLabel* toolSubtitle_ = nullptr;
    QStackedWidget* toolStack_ = nullptr;
    QLineEdit* outlinerFilter_ = nullptr;
    QTreeView* outliner_ = nullptr;
    WorldOutlinerModel* outlinerModel_ = nullptr;
    QComboBox* outlinerCategory_ = nullptr;
    QLabel* outlinerStatus_ = nullptr;
    bool outlinerDirty_ = true;
    Services::WorldSelection worldSelection_;
    std::shared_ptr<const MapDocument> selectionDocument_;
    std::optional<Services::MapDataQuerySnapshot> selectionMapData_;
    std::array<Services::ProjectSearchObjectSource, 7> selectionObjectSources_{};
    std::string selectionDocumentId_;
    std::uint64_t selectionCatalogRevision_ = 1;
    std::uint64_t selectionBoundCatalogRevision_ = 0;
    QString selectionMapKey_;
    WorldAssetBrowser* assetBrowser_ = nullptr;
    QWidget* terrainPalette_ = nullptr;
    QButtonGroup* terrainKinds_ = nullptr;
    QWidget* terrainTextureSection_ = nullptr;
    QWidget* swatchGrid_ = nullptr;
    QButtonGroup* terrainSwatches_ = nullptr;
    QLabel* swatchName_ = nullptr;
    QComboBox* terrainVariation_ = nullptr;
    std::vector<std::string> swatchIds_; // 与按钮 id 同序的地面贴图 rawcode
    const void* swatchSource_ = nullptr; // 生成缩略图所用的材质缓存（换代时重建）
    QWidget* terrainCliffSection_ = nullptr;
    QComboBox* terrainCliffTexture_ = nullptr;
    QButtonGroup* terrainCliffDeltas_ = nullptr;
    QWidget* terrainWaterSection_ = nullptr;
    QDoubleSpinBox* terrainWaterHeight_ = nullptr;
    QWidget* terrainBlightSection_ = nullptr;
    QButtonGroup* terrainBlightModes_ = nullptr;
    QButtonGroup* terrainShapes_ = nullptr;
    QSpinBox* terrainRadius_ = nullptr;
    QSlider* terrainRadiusSlider_ = nullptr;
    QWidget* terrainStrengthSection_ = nullptr;
    QDoubleSpinBox* terrainStrength_ = nullptr;
    QSlider* terrainStrengthSlider_ = nullptr;
    // 右侧属性
    QWidget* inspectorBody_ = nullptr;
    QWidget* selectionCard_ = nullptr;
    QLabel* selectionEmpty_ = nullptr;
    QLabel* selectionIcon_ = nullptr;
    QLabel* selectionTitle_ = nullptr;
    QLabel* selectionMeta_ = nullptr;
    QWidget* placementFields_ = nullptr;
    QDoubleSpinBox* fieldX_ = nullptr;
    QDoubleSpinBox* fieldY_ = nullptr;
    QDoubleSpinBox* fieldFacing_ = nullptr;
    QLabel* fieldZ_ = nullptr;
    QWidget* regionFields_ = nullptr;
    std::array<QDoubleSpinBox*, 4> regionBounds_{}; // 左、下、右、上
    QWidget* cameraFields_ = nullptr;
    QDoubleSpinBox* cameraX_ = nullptr;
    QDoubleSpinBox* cameraY_ = nullptr;
    QLabel* cameraDetails_ = nullptr;
    QPushButton* openDefinition_ = nullptr;
    QPushButton* deleteSelected_ = nullptr;
    QLabel* selectionNote_ = nullptr;
    QLabel* instanceDiagnosticNote_ = nullptr;
    QLabel* sceneTerrain_ = nullptr;
    QLabel* sceneGround_ = nullptr;
    QLabel* sceneWater_ = nullptr;
    QSpinBox* waterFrame_ = nullptr; // 显式预览帧，不进入地图数据事务或保存成员
    QLabel* sceneCliffs_ = nullptr;
    QLabel* sceneLighting_ = nullptr;
    QLabel* modelStatus_ = nullptr;
    QLabel* sceneNotes_ = nullptr;
    QLabel* groundPoint_ = nullptr;
    // 状态栏
    QLabel* rendererDot_ = nullptr;
    QLabel* rendererStatus_ = nullptr;
    UiKit::ElidedLabel* terrainStatus_ = nullptr;
    QLabel* terrainPathingWarning_ = nullptr;
    UiKit::ElidedLabel* selectionStatus_ = nullptr; // 当前工具的操作提示与最近一次操作结果
    QLabel* hoverStatus_ = nullptr;
    QVariantAnimation* cameraFlight_ = nullptr;
    std::optional<std::array<double, 2>> brushCursor_; // 笔刷落点（格点吸附前的地面坐标）
    Tool shownTool_ = Tool::Select;
    QPointer<QThread> worker_;
    QPointer<QThread> modelWorker_;
    WorldSelectionPresenter* selectionPresenter_ = nullptr;
    WorldBuildingSplatPresenter* buildingSplatPresenter_ = nullptr;
    WorldModelBugPlaceholderPresenter* bugPlaceholderPresenter_ = nullptr;
    WorldPremPresenter* premPresenter_ = nullptr;
    std::shared_ptr<const WorldPathingHelperCache> pathingHelperCache_;
    std::shared_ptr<const WorldPathingHelperPrepared> pathingHelperPresentation_; // accepted来源/占地原因保活，未验项不丢。
    std::vector<std::uint8_t> pathingHelperRequests_; // 与accepted requests平行，服务实际确认的辅助分类。
    std::vector<WorldPlacementPresentationCandidate> stagedPresentations_, clipboardPresentations_;
    QPushButton* showPathingHelpers_ = nullptr;
    std::uint64_t buildingSplatGeneration_ = 0; // 独立fallback registry代次，不能冒称主模型身份。
    std::shared_ptr<const Services::WorldInstanceRegistry> instanceRegistry_, stagedInstanceRegistry_;
    WorldInstanceTiming stagedInstanceTiming_;
    std::uint64_t placementInputStartedNs_ = 0;
    std::uint64_t placementTransactionCompletedNs_ = 0;
    WorldPlacementPending pendingPlacements_;
    std::shared_ptr<const Render::AnimatedModelScene> placementCompletedScene_;
    std::shared_ptr<const Render::AnimationFrame> placementCompletedFrame_;
    std::vector<Render::ModelInstanceRequest> stagedRequests_;
    std::vector<MapData::PlacementId> stagedPlacementIds_;
    std::uint64_t allowedInstanceEditRevision_ = 0;
    std::shared_ptr<std::uint64_t> instanceRevisionGate_;
    std::uint64_t modelGeneration_ = 0; // 模型线程运行期间又换代时，结束后按此补一次加载
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> rawTerrain_; // 对应编辑会话的不可变原始快照
    std::shared_ptr<const Render::TerrainCliffs> cliffs_; // 当前地形已烘焙的悬崖，随模型场景提交
    // 当前显示的模型场景（GUI 线程原位更新实例后交给视口）及构建它的请求、来源标识。
    std::shared_ptr<Render::WorldModelScene> scene_;
    std::vector<Render::ModelInstanceRequest> requests_;
    std::vector<MapData::PlacementId> placementIds_;
    std::vector<WorldUnresolvedPlacement> unresolvedPlacements_;
    QPointer<WorldModelDiagnosticsDialog> modelDiagnosticsDialog_;
    QPushButton* effectBudgetStatus_ = nullptr;
    // 仅绑定成功接收的场景；typed 摆放映射加载一次，逐实例诊断由 Core 公共查询提供。
    std::shared_ptr<const Render::AnimatedModelScene> instanceDiagnosticScene_;
    std::shared_ptr<const Services::WorldInstanceRegistry> instanceDiagnosticRegistry_;
    std::shared_ptr<Render::AnimationDiagnosticQuery> instanceDiagnosticQuery_;
    std::map<std::pair<MapData::PlacementFile, std::uint32_t>, std::uint32_t> instanceDiagnosticRequests_;
    std::optional<Services::WorldSelectionVersion> instanceDiagnosticVersion_;
    std::optional<Services::WorldSelectionVersion> emptyModelDiagnosticVersion_;
    QString emptyModelDiagnosticMapKey_;
    std::uint64_t emptyModelDiagnosticGeneration_ = 0;
    QString instanceDiagnosticMapKey_;
    std::uint64_t instanceDiagnosticGeneration_ = 0;
    bool placementsDirty_ = false; // 模型读取期间摆放又有变化
    // 选择：摆放、区域、镜头三者至多一个。
    std::optional<MapData::PlacementId> selection_;
    std::vector<MapData::PlacementId> selectedPlacements_;
    std::optional<MapData::RegionId> selectedRegion_;
    std::optional<MapData::CameraId> selectedCamera_;
    QString selectionName_; // 选中时解析的显示名；拖动期间不重复读取文档
    QString selectionIconPath_; // 选中摆放的图标路径与类型、所属玩家（选中时解析一次）
    std::optional<ObjectData::ObjectType> selectionType_;
    std::optional<std::int32_t> selectionOwner_;
    QString inspectorKey_; // 属性栏当前显示的对象；换对象时淡入
    // 拖动草稿：按下处地面点与各类对象的待提交值；松开时一次提交。
    std::optional<std::array<double, 2>> dragStart_;
    int regionCorner_ = -1; // -1 平移区域；0..3 拖动西南/东南/东北/西北角
    int regionEdge_ = -1; // 角优先；0..3 单独调整左/下/右/上边，均为 -1 时平移
    std::optional<std::array<float, 3>> dragPosition_;
    std::vector<MapData::PlacementMove> dragGroup_;
    std::optional<float> dragRotation_;
    std::optional<std::array<float, 4>> regionDraft_;
    std::optional<std::array<float, 2>> cameraDraft_;
    // 摆放面板
    QWidget* palette_ = nullptr;
    QtApp::ObjectCatalogWidget* catalog_ = nullptr;
    QComboBox* owner_ = nullptr;
    std::optional<std::pair<ObjectData::ObjectType, std::string>> placing_;
    bool pasting_ = false;
    std::vector<MapData::PlacementId> clipboard_;
    std::vector<Render::ModelInstanceRequest> clipboardRequests_; // Copy时只查询这些行，Paste直接平移自有值。
    QString clipboardMapKey_;
    std::uint64_t clipboardRevision_ = 0;
    std::uint64_t clipboardSession_ = 0;
    std::uint64_t clipboardPresentationCatalogRevision_ = 0;
    std::array<float, 3> clipboardAnchor_{};
    std::optional<std::array<float, 3>> previewHover_;
    std::vector<Render::ModelInstanceRequest> previewRequests_;
    std::size_t previewStart_ = 0;
    QString placingName_;
    std::vector<MapData::RegionRecord> regions_;
    std::vector<MapData::CameraRecord> cameras_;
    std::uint64_t generation_ = 0;
    bool reloadQueued_ = false;
    bool needsRefresh_ = true;
    struct TerrainRenderRequest {
        std::shared_ptr<const MapData::TerrainGridSnapshot> source;
        MapData::TerrainBrush brush;
        std::vector<MapData::TerrainStrokePoint> points;
        std::uint64_t serial = 0;
        bool preview = false;
        std::optional<std::uint32_t> selectedWaterFrame;
    };
    std::optional<TerrainRenderRequest> pendingTerrainRender_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> strokeBase_;
    MapData::TerrainBrush strokeBrush_;
    std::vector<MapData::TerrainStrokePoint> strokePoints_;
    std::uint64_t terrainRenderSerial_ = 0;
    std::uint64_t viewportRevision_ = 0;
    bool waterFrameVisible_ = false;
    bool terrainPathingPending_ = false;
    bool resumeBrushAfterRender_ = false;
    bool pushingTerrainStrokeUndo_ = false; // 本笔首个 QUndoStack::indexChanged 不得清除等待最终地形的恢复意图。
    // 镜头只在同一张地图上恢复：启动时来自上次会话，重新读取（如保存后）时来自读取前的视角。
    [[nodiscard]] QString MapKey() const;
    std::shared_ptr<WorldTerrainCache> terrainCache_; // 完整读取建立，地形编辑重建复用；只在 worker_ 中使用
    std::optional<WorldEnvironmentInputs> environmentInputs_; // 当前显示环境对应的地图设置输入
    QJsonObject pendingCamera_;
    QString pendingCameraMap_;
    QString shownMap_; // 视口当前显示的地图
};

} // namespace War3Map::Modules

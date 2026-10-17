#pragma once

#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/RenderPluginTerrainBlight.h"
#include "War3MapCore/Render/ViewRay.h"
#include "War3MapCore/Render/FrameCapture.h"
#include "War3MapCore/Render/SceneEnvironment.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/ModelPicking.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "War3MapCore/Render/ModelSceneLayerRenderer.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include "War3MapCore/Services/WorldModelBugPlaceholderService.h"
#include "RenderPluginFailure.h"

#include <QByteArray>
#include <QJsonObject>
#include <QLibrary>
#include <QPointF>
#include <QVector>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <map>
#include <deque>

#include <vector>

namespace War3Map::Modules {

// QWidget 只拥有 HWND 和输入；V2 插件独占 GPU 对象及渲染线程。
class WorldViewport final : public QWidget {
    Q_OBJECT
public:
    // 可注入同 ABI 的后端供隔离测试；函数表必须比视口长寿，不由视口卸载。
    explicit WorldViewport(QWidget* parent = nullptr, const War3RenderPluginApiV2* backend = nullptr);
    ~WorldViewport() override;
    bool SetMesh(std::vector<War3RenderVertexV2> vertices, std::vector<std::uint32_t> indices,
        std::uint64_t documentRevision);
    void ClearMesh(std::uint64_t documentRevision);
    bool SetPickingTerrain(std::shared_ptr<const MapData::TerrainGridSnapshot> terrain,
        std::uint32_t stride, std::uint64_t documentRevision);
    bool SetGroundMesh(std::shared_ptr<const Render::TerrainGroundMesh> ground);
    [[nodiscard]] const Status& LastGroundFailure() const noexcept { return groundFailure_; }
    // 笔刷预览/提交只替换地形与拾取快照；镜头、模型、环境及叠加线保持原位。
    bool ReplaceTerrainPresentation(std::vector<War3RenderVertexV2> vertices, std::vector<std::uint32_t> indices,
        std::shared_ptr<const MapData::TerrainGridSnapshot> terrain, std::uint32_t stride,
        std::shared_ptr<const Render::TerrainGroundMesh> ground, std::uint64_t documentRevision,
        std::span<const Render::BakedModelMesh> cliffs = {}, bool layeredCliffs = false);
    [[nodiscard]] bool HasGroundMesh() const noexcept { return ground_ != nullptr; }
    [[nodiscard]] bool HasGroundSupport() const noexcept;
    [[nodiscard]] bool HasTerrainMips() const noexcept;
    // 可在renderer建立前只读查询编译能力；不访问GPU，不因缺少可选尾部改变旧路径。
    [[nodiscard]] bool HasTerrainBlightSupport();
    [[nodiscard]] bool HasTerrainGridSupport();
    // 模型场景与地形并存、独立提交；插件不支持或拒绝时保留旧 CPU 场景和选择。空指针清除模型。
    bool SetModelScene(std::shared_ptr<const Render::WorldModelScene> models);
    [[nodiscard]] bool HasModelSupport() const noexcept;
    [[nodiscard]] Status CheckCliffSamplerSupport(std::span<const Render::BakedModelMesh> baked, bool layered) const;
    [[nodiscard]] bool HasModelScene() const noexcept { return models_ != nullptr; }
    [[nodiscard]] bool HasModelInstances() const noexcept { return models_ && !models_->instances.empty(); }
    [[nodiscard]] bool HasAnimationSupport() const noexcept;
    [[nodiscard]] bool HasParticleSupport() const noexcept;
    [[nodiscard]] bool HasModelLayerSupport() const noexcept;
    [[nodiscard]] Status SetModelLayer(std::uint64_t id, std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Render::AnimationFrame> frame, std::uint32_t role = WAR3_RENDER_MODEL_LAYER_MODEL);
    [[nodiscard]] Status UpdateModelLayer(std::uint64_t id, std::shared_ptr<const Render::AnimationFrame> frame);
    [[nodiscard]] Status RemoveModelLayer(std::uint64_t id);
    // PREM 清理失败时共用 renderer 销毁路径；保留 CPU/地图，拒绝继续显示无法撤销的旧子层。
    [[nodiscard]] Status RemovePremModelLayer(std::uint64_t id);
    using AcceptedAnimationObserver = std::function<void(std::shared_ptr<const Render::AnimatedModelScene>,
        std::shared_ptr<const Render::AnimationFrame>)>;
    void SetAcceptedAnimationObserver(AcceptedAnimationObserver observer) { acceptedAnimationObserver_ = std::move(observer); }
    // CPU占位准备可读取同源Draw/Upload完成但Present被遮挡的帧；不授予可见/点选资格。
    [[nodiscard]] Result<std::shared_ptr<const Services::WorldModelBugPlaceholderFrameSnapshot>> ReadCompletedBugPlaceholderSnapshot(
        const std::shared_ptr<const Render::AnimatedModelScene>& expected) const;
    void SetBugPlaceholderPicking(std::uint64_t layer,
        std::shared_ptr<const Services::WorldModelBugPlaceholderPlan> plan,
        std::shared_ptr<const Render::AnimatedModelScene> scene, std::shared_ptr<const Render::AnimationFrame> frame,
        std::vector<MapData::PlacementId> owners, std::function<bool()> live, Services::WorldModelBugFlagsReader flags);
    void ClearBugPlaceholderPicking();
    [[nodiscard]] bool HasBugPlaceholderPicking(const std::shared_ptr<const Services::WorldModelBugPlaceholderPlan>& plan) const;
    bool SetAnimationScene(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Render::AnimationFrame> frame, std::shared_ptr<Render::WorldModelScene> picking);
    bool UpdateAnimationFrame(std::shared_ptr<const Render::AnimationFrame> frame,
        const std::shared_ptr<Render::WorldModelScene>& picking);
    [[nodiscard]] bool HasInstanceRosterSupport() const noexcept;
    [[nodiscard]] Result<std::uint64_t> QueueAnimationRoster(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Render::AnimationFrame> frame, std::shared_ptr<Render::WorldModelScene> picking,
        bool assetsChanged, std::function<bool(std::uint32_t)> visible);
    [[nodiscard]] Result<War3RenderInstanceRosterResultV2> PollAnimationRoster(std::uint64_t ticket);
    [[nodiscard]] Result<War3RenderInstanceRosterResultV2> CancelPendingAnimationRoster(std::uint64_t ticket);
    [[nodiscard]] const Status& LastAnimationFailure() const noexcept { return animationFailure_; }
    [[nodiscard]] bool HasAnimationScene() const noexcept { return animationScene_ != nullptr; }
    [[nodiscard]] std::shared_ptr<const Render::AnimationFrame> AnimationTarget() const noexcept { return animationFrame_; }
    struct FrameMetricsPage {
        War3RenderFrameMetricsPageV2 page{};
        std::vector<War3RenderFrameMetricsV2> records;
    };
    // 只读分页，不触发绘制或像素读回；旧插件缺少可选尾部时明确拒绝。
    [[nodiscard]] Result<FrameMetricsPage> ReadFrameMetrics(std::uint64_t afterFrame) const;
    [[nodiscard]] std::uint64_t MeasurementRendererEpoch() const noexcept { return measurementRendererEpoch_; }
    struct AnimationReceiveTiming {
        std::uint64_t sceneVersion = 0, contentVersion = 0;
        std::uint64_t pickingNs = 0, visibilityNs = 0, submitNs = 0, installNs = 0, observerNs = 0;
    };
    [[nodiscard]] std::optional<AnimationReceiveTiming> ReceiveTiming(std::uint64_t scene, std::uint64_t content) const;
    void SetMeasurementEnabled(bool enabled) noexcept { measurementEnabled_ = enabled; }
    [[nodiscard]] std::uint64_t MeasurementPresentationRevision() const noexcept { return editorPresentationRevision_; }
    [[nodiscard]] std::uint64_t MeasurementModelEpoch() const noexcept {
        return editorPresentationTarget_ ? editorPresentationTarget_->modelEpoch : 0;
    }
    [[nodiscard]] bool MeasurementRequestVisible(std::uint32_t request) const {
        return !instanceVisible_ || instanceVisible_(request);
    }
    [[nodiscard]] QSize MeasurementPhysicalSize() const { return PhysicalSize(); }
    // 与渲染器 Resize 使用同一 DPR/取整规则；后台首帧和播放器队列共享此快照。
    [[nodiscard]] Render::AnimationView AnimationViewSnapshot() const;
    // 保留最后接收的几何/姿势，仅使失效来源的旧画面只读；新场景成功接收后才恢复。
    void InvalidateAnimationPicking();
    [[nodiscard]] bool ModelPresentationCurrent() const noexcept { return !animationScene_ || animationPickingCurrent_; }

    // 场景环境（模型光照、雾、静态阴影）；渲染器重建时自动重交。插件不支持时返回 false。
    bool SetEnvironment(std::shared_ptr<const Render::SceneEnvironment> environment);
    [[nodiscard]] bool HasEnvironmentSupport() const noexcept;
    // 调用方原位改动了模型场景的实例变换/颜色后调用：插件支持时只重交实例，否则重交整个场景。
    bool UpdateModelInstances();
    // 编辑叠加线（选择框、区域、镜头）；渲染器重建时自动重交。插件不支持时返回 false。
    bool SetOverlayLines(std::vector<War3RenderOverlayLineV2> lines);
    [[nodiscard]] bool HasOverlaySupport() const noexcept;
    [[nodiscard]] std::size_t OverlayLineCount() const noexcept { return overlay_.size(); } // 当前请求的叠加线段数
    // 当前选中的实例（场景 instances 下标）：左键在其上按下并拖动即移动，而不是旋转镜头。
    void SetSelectedInstance(std::optional<std::uint32_t> instance);
    [[nodiscard]] std::optional<std::uint32_t> SelectedInstance() const noexcept { return selectedInstance_; }
    [[nodiscard]] bool Moving() const noexcept { return moving_ && moved_; }
    // 地面拖动手柄（区域、镜头目标等）：按下处地面点满足该判断时左键拖动改为移动，而不是旋转镜头。
    void SetHandleTest(std::function<bool(double x, double y)> test) { handleTest_ = std::move(test); }
    // 参数是 instanceRequests 中的来源请求编号，不是渲染器实例下标；隐藏/锁定由宿主共同选择服务判断。
    void SetInstanceSelectable(std::function<bool(std::uint32_t request)> predicate) {
        instanceSelectable_ = std::move(predicate); ++pickPolicyRevision_;
        // 可选策略不改变已绘制姿势或 GPU 显隐；保留实际完成画面的拥有者，查询时
        // 仍重新冻结当前策略。清空历史会让播放中的单击等待下一帧而发生饥饿。
        for (auto& owned : animationPickHistory_) owned.policyRevision = pickPolicyRevision_;
        bugPreparationHistory_.clear(); bugPreparationFailure_ = {};
        RetainBugPreparationFrame(); // 可选性变化不等新动画帧；独立观察立即重建。
        if (animationPickHistory_.empty()) RetainAnimationPickFrame();
    }
    // 独立呈现掩码，参数为来源请求编号；烘焙网格/无placement保持可见。
    // 设置只记录回调；Refresh成功仅表示CPU已接收，同次GPU身份仍以截图回执为准。
    void SetInstanceVisible(std::function<bool(std::uint32_t request)> predicate);
    [[nodiscard]] bool RefreshEditorVisibility();
    [[nodiscard]] bool HasEditorVisibilitySupport() const noexcept;
    // 摆放模式：左键单击地面发出 PlaceRequested，不做实例点选；Esc 退出并发出 PlacingCancelled。
    void SetPlacing(bool placing);
    [[nodiscard]] bool Placing() const noexcept { return placing_; }
    void SetBrushMode(bool enabled);
    [[nodiscard]] bool BrushMode() const noexcept { return brushMode_; }
    [[nodiscard]] bool StrokeActive() const noexcept { return strokeActive_; }
    void ResetCamera();
    // Camera() 是当前请求状态；完成帧实际使用的相机只以 CaptureCompletedFrame 回执为准。
    [[nodiscard]] War3RenderCameraV2 Camera() const noexcept { return camera_; }
    [[nodiscard]] Result<War3RenderCameraV2> ReadCamera(std::uint64_t expectedRevision) const;
    [[nodiscard]] Status CheckCameraChange(const War3RenderCameraV2& camera, std::uint64_t expectedRevision) const;
    // 供服务层使用的修订绑定相机命令；成功仅表示已提交给渲染器，仍须等待完成帧。
    [[nodiscard]] Status SetCamera(const War3RenderCameraV2& camera, std::uint64_t expectedRevision);
    // 显式读回真实完成帧。单次调用沿用插件的 10 秒等待上限，绝不拼接旧版截图与后查视图。
    [[nodiscard]] Result<Render::CapturedFrameRgba> CaptureCompletedFrame(std::uint64_t expectedRevision,
        std::size_t maxBytes = Render::kDefaultFrameCaptureBytes) const;
    [[nodiscard]] QJsonObject SaveCamera() const;
    bool RestoreCamera(const QJsonObject& state);
    [[nodiscard]] QString StatusText() const { return status_; }
    // 仅读取现有后端和已观测状态；不查询/启动插件、不请求绘制，也不签收完成帧。
    [[nodiscard]] QJsonObject BackendDiagnosticSnapshot() const;
    // 状态点用：渲染器已创建且未失败 / 已失败（其余为启动中或未就绪）。
    [[nodiscard]] bool RendererReady() const noexcept { return renderer_ != nullptr && !failed_; }
    [[nodiscard]] bool RendererFailed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t VertexCount() const noexcept { return vertices_.size(); }
    [[nodiscard]] std::size_t TriangleCount() const noexcept { return indices_.size() / 3; }
    [[nodiscard]] bool FrameTimerActive() const noexcept { return frameTimer_.isActive(); }
    [[nodiscard]] std::uint64_t DocumentRevision() const noexcept { return revision_; }
    [[nodiscard]] QString HoverText() const { return hoverText_; }
    [[nodiscard]] QString SelectionText() const { return selectionText_; }
    [[nodiscard]] const std::optional<Render::TerrainPickHit>& HoverCoordinate() const noexcept { return hoverCoordinate_; }
    [[nodiscard]] const std::optional<Render::TerrainPickHit>& SelectedCoordinate() const noexcept { return selectedCoordinate_; }
    // 诊断/Agent 查询：当前完成帧下可选实例中心的逻辑像素位置，按实例序返回至多 limit 项。
    [[nodiscard]] std::vector<std::pair<std::uint32_t, QPointF>> ProjectedSelectableInstances(std::size_t limit) const;
    // 诊断/Agent 查询：可选实例的世界位置（摆放中心）；不可选或越界返回空。
    [[nodiscard]] std::optional<std::array<float, 3>> SelectableInstancePosition(std::uint32_t instance) const;
    [[nodiscard]] std::size_t ModelInstanceCount() const noexcept { return models_ ? models_->instances.size() : 0; }

signals:
    void PluginFailed(const War3Map::Modules::RenderPluginFailure& failure);
    void MeasurementFrameAccepted(); // 只在显式测量期间发送；观察者只留紧凑身份，不触发编辑。
    void AnimationTransformsChanged(); // UI 变换编辑转交同一动画服务，不退回静态槽。
    void AnimationCameraChanged(); // 世界空间粒子按新相机重放，仍由同一服务提交整帧。
    void StatusChanged(const QString& text);
    void CoordinatesChanged(const QString& hover, const QString& selection);
    // 左键单击：命中的实例下标，未命中为 -1。同一位置重复单击在重叠实例间轮换。
    void InstanceClicked(int instance, int modifiers);
    // 独立原创方块命中直接交真实摆放身份，绝不伪造主场景renderedInstance。
    void BugPlaceholderClicked(const War3Map::MapData::PlacementId& owner, int modifiers);
    void InstancesBoxSelected(const QVector<int>& instances, int modifiers);
    // 未点中实例时单击处的地面点（供区域、镜头点选）。
    void GroundClicked(double x, double y, int modifiers);
    void PlaceRequested(double x, double y, bool snap);
    void PlacementHovered(double x, double y, double z, bool snap);
    void PlacementHoverLost();
    void PlacingCancelled();
    void TerrainStrokeStarted(double x, double y);
    void TerrainStrokeMoved(double x, double y);
    void TerrainStrokeFinished();
    void TerrainStrokeCancelled();
    void TerrainBrushExitRequested();
    // 拖动选中对象：开始时给出按下处地面点，之后给出光标下地面点（调用方按差值移动）；结束或取消各发一次。
    void MoveStarted(double x, double y);
    void MoveDragged(double x, double y);
    void MoveFinished();
    void MoveCancelled();
    void RotateStarted(double x, double y);
    void RotateDragged(double x, double y);
    void RotateFinished();
    void RotateCancelled();
    void DeleteRequested();
    void CopyRequested();
    void PasteRequested();
    void DuplicateRequested();
    void SelectAllRequested();
    void NudgeRequested(double dx, double dy);
    void FocusSelectionRequested();
    // 仅转交有界资源身份与真实完成帧地形落点；宿主复核地图身份并执行已有摆放事务。
    void AssetDropped(const QByteArray& asset, double x, double y, bool snap);

protected:
    bool event(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    bool measurementEnabled_ = false;
    std::uint64_t measurementRendererEpoch_ = 1;
    std::deque<AnimationReceiveTiming> animationReceiveTimings_;
    void EnsureRenderer();
    bool LoadPluginApi();
    void ReportPluginFailure(const char* operation, std::int32_t result,
        const War3RenderStatusV2* observed = nullptr, std::optional<std::int32_t> statusCall = std::nullopt) const;
    void DestroyRenderer();
    void Tick();
    bool SubmitMesh();
    bool SubmitModels();
    bool SubmitAnimation();
    bool SubmitAnimation(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Render::AnimationFrame> frame, std::unique_ptr<Render::AnimationRenderer>& bridge);
    bool SubmitModels(const Render::WorldModelScene* models);
    struct EditorVisibilityBuffers {
        std::vector<std::uint8_t> instances, particleDraws;
        [[nodiscard]] Render::EditorVisibilityMasks Describe(std::uint64_t revision) const noexcept {
            return {instances, particleDraws, revision};
        }
    };
    struct AnimationVisibilityCache {
        std::uint64_t sceneVersion = 0, documentRevision = 0;
        std::vector<std::uint32_t> poseRendered, poseRequests, renderedRequests;
        std::vector<std::uint8_t> editorRequests, requestVisibility, instances;
        bool valid = false;
    };
    mutable AnimationVisibilityCache animationVisibilityCache_;
    [[nodiscard]] bool BuildEditorVisibility(const Render::WorldModelScene* models,
        const Render::AnimationFrame* frame, EditorVisibilityBuffers& output) const;
    [[nodiscard]] bool ReadEditorPresentationTarget(War3RenderEditorPresentationTargetV2& output) const;
    bool SubmitStaticVisibility(const Render::WorldModelScene* models, const War3RenderModelSceneV2& scene);
    bool UpdateStaticVisibility(bool transforms);
    bool SubmitEnvironment();
    void SubmitCamera();
    void ResizeRenderer();
    void SetStatus(QString text);
    [[nodiscard]] QSize PhysicalSize() const;
    [[nodiscard]] bool CanRender() const;
    [[nodiscard]] bool HasViewInfo() const;
    [[nodiscard]] Result<Render::ViewProjectionFrame> ReadPickingFrame() const;
    // cameraOnly：只核对镜头与画布未变（地面拾取用 CPU 地形，不依赖模型姿势）。
    [[nodiscard]] Result<Render::TerrainPickResult> PickAt(QPointF point, const Render::ViewProjectionFrame& frame,
        const MapData::TerrainGridSnapshot* fixedTerrain = nullptr, bool cameraOnly = false) const;
    void ScheduleHover();
    void UpdateHover();
    void ClearCoordinates(QString reason, bool clearSelection = false, bool preservePlacementPreview = false);
    void SetCoordinateTexts(QString hover, QString selection);
    void FinishClick(QPointF point, Qt::KeyboardModifiers modifiers);
    void FinishPlacement(QPointF point, bool snap, std::uint64_t serial, std::uint64_t cameraSerial, unsigned attempt);
    void FinishSelectionBox(QPointF point);
    bool DeferSelection(QPointF point, Qt::KeyboardModifiers modifiers, bool box);
    bool SubmitOverlay();
    void UpdateSelectionOutline(QPointF point);
    void ClearSelectionOutline();
    void RetainAnimationPickFrame();
    void RetainBugPreparationFrame();
    friend class WorldViewportPickHistoryTestAccess;
    struct ModelPickPolicy {
        std::uint64_t generation = 0, sceneVersion = 0;
        std::vector<std::uint8_t> visible, selectable;
        bool operator==(const ModelPickPolicy&) const noexcept = default;
    };
    struct CompletedModelPickFrame {
        Render::ViewProjectionFrame view;
        War3RenderFrameMetricsV2 metric{};
        Render::ModelPickVisibilitySnapshot visibility;
        std::shared_ptr<const Render::AnimationFrame> frame;
        std::shared_ptr<const Render::WorldModelScene> picking;
        std::shared_ptr<const Services::WorldModelBugPlaceholderFrameSnapshot> preparation;
    };
    struct AnimationCompletionIdentity {
        War3RenderEditorPresentationTargetV2 presentation{};
        std::uint64_t presentationRevision = 0, rendererEpoch = 0, policyRevision = 0;
        std::uint64_t sceneVersion = 0, contentVersion = 0, documentRevision = 0;
        std::size_t instanceCount = 0;
        std::uint32_t particleVertexCount = 0, particleDrawCount = 0;
    };
    struct RetainedAnimationPickFrame : AnimationCompletionIdentity {
        // 对外借用仍为 const；仅移出历史且没有借用者的帧可复用存储。
        std::shared_ptr<Render::AnimationFrame> frame;
        std::size_t ownedBytes = 0;
    };
    struct RetainedBugPreparationFrame : AnimationCompletionIdentity {
        std::shared_ptr<const Services::WorldModelBugPlaceholderFrameSnapshot> snapshot;
        std::size_t ownedBytes = 0;
    };
    [[nodiscard]] Result<ModelPickPolicy> ReadModelPickPolicy() const;
    [[nodiscard]] Result<CompletedModelPickFrame> ReadCompletedModelPickFrame(bool forGeometryPick = true,
        bool requireSuccessfulPresent = true) const;
    [[nodiscard]] Result<CompletedModelPickFrame> ReadCompletedAnimationSource(bool preparation,
        bool requireSuccessfulPresent, bool freezeGeometryPolicy = false) const;
    [[nodiscard]] Result<std::vector<Render::ModelPickHit>> PickInstancesAt(QPointF point,
        const Render::ViewProjectionFrame& frame) const;
    struct BugPlaceholderPick {
        MapData::PlacementId owner;
        double distance = 0;
    };
    [[nodiscard]] Result<std::optional<BugPlaceholderPick>> PickBugPlaceholderAt(QPointF point,
        const Render::ViewProjectionFrame& view) const;
    struct BugPlaceholderBinding {
        std::uint64_t layer = 0, rendererEpoch = 0;
        std::shared_ptr<const Services::WorldModelBugPlaceholderPlan> plan;
        std::shared_ptr<const Render::AnimatedModelScene> scene;
        std::shared_ptr<const Render::AnimationFrame> frame;
        std::vector<MapData::PlacementId> owners;
        std::function<bool()> live;
        Services::WorldModelBugFlagsReader flags;
    };
    std::shared_ptr<const BugPlaceholderBinding> bugPlaceholderPicking_;
    [[nodiscard]] bool IsInstanceSelectable(std::uint32_t instance) const;
    [[nodiscard]] std::optional<std::array<double, 3>> ProjectInstanceCenter(std::uint32_t instance,
        const Render::ViewProjectionFrame& frame) const;
    void BeginMove(QPointF point, const Render::ViewProjectionFrame& frame, bool rotate);
    void CancelMove();
    void CancelTerrainStroke();

    QLibrary library_;
    const War3RenderPluginApiV2* api_ = nullptr;
    War3RendererHandle renderer_ = nullptr;
    bool injected_ = false;
    bool initializing_ = false;
    bool failed_ = false;
    bool destroying_ = false;
    std::optional<int> attachResult_, statusCallResult_;
    std::optional<War3RenderStatusV2> observedStatus_;
    std::uint64_t observedStatusRendererEpoch_ = 0;
    bool pickingSuspended_ = false;
    QTimer frameTimer_;
    QTimer hoverTimer_;
    std::vector<War3RenderVertexV2> vertices_;
    std::vector<std::uint32_t> indices_;
    std::uint64_t revision_ = 0;
    War3RenderCameraV2 camera_{sizeof(War3RenderCameraV2), 0, 0, 0, 2048, -0.7853982f, 0.7853982f};
    War3RenderCameraV2 homeCamera_ = camera_;
    QPointF mousePosition_;
    QPointF pressPosition_, hoverPosition_, sampledPosition_;
    Qt::MouseButton dragButton_ = Qt::NoButton;
    bool dragging_ = false;
    bool pointerInside_ = false;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> strokeTerrain_; // 一笔始终按按下时地形求交
    std::shared_ptr<const Render::TerrainGroundMesh> ground_;
    std::shared_ptr<const Render::WorldModelScene> models_;
    std::shared_ptr<const Render::AnimatedModelScene> animationScene_;
    std::shared_ptr<const Render::AnimationFrame> animationFrame_;
    std::deque<RetainedAnimationPickFrame> animationPickHistory_;
    std::deque<RetainedBugPreparationFrame> bugPreparationHistory_;
    Status bugPreparationFailure_;
    std::uint64_t pickPolicyRevision_ = 1;
    Status animationFailure_;
    Status groundFailure_;
    bool animationPickingCurrent_ = true;
    std::unique_ptr<Render::AnimationRenderer> animationRenderer_;
    struct PendingInstanceRoster {
        std::uint64_t ticket = 0;
        std::shared_ptr<const Render::AnimatedModelScene> scene;
        std::shared_ptr<const Render::AnimationFrame> frame;
        std::shared_ptr<Render::WorldModelScene> picking;
    };
    std::optional<PendingInstanceRoster> pendingInstanceRoster_;
    Render::WorldModelScene animationPickingScratch_; // 只保留轻量实例，不复制静态模型/包围盒。
    War3RendererHandle modelLayerBridgeHandle_ = nullptr;
    struct ModelLayerBinding {
        std::unique_ptr<Render::ModelSceneLayerRenderer> renderer;
        std::shared_ptr<const Render::AnimatedModelScene> scene;
        Render::AnimationSession source; // 成功提交时冻结；与视口地形呈现序号是不同空间。
    };
    std::map<std::uint64_t, ModelLayerBinding> modelLayerBridges_;
    AcceptedAnimationObserver acceptedAnimationObserver_;
    void NotifyAcceptedAnimationFrame();
    std::shared_ptr<const Render::SceneEnvironment> environment_;
    std::vector<War3RenderOverlayLineV2> overlay_;
    std::vector<War3RenderOverlayLineV2> selectionOutline_;
    std::optional<std::uint32_t> selectedInstance_;
    bool moving_ = false, moved_ = false; // 按在选中对象上 / 已开始拖动
    bool rotating_ = false; // 旋转由编辑命令驱动；Ctrl保留给减选。
    bool selectionBox_ = false;
    Qt::KeyboardModifiers selectionModifiers_ = Qt::NoModifier;
    Qt::KeyboardModifiers hoverModifiers_ = Qt::NoModifier;
    bool placing_ = false;
    bool placementHoverActive_ = false;
    bool brushMode_ = false;
    bool strokeActive_ = false;
    QPointF strokeLastPickPixel_;
    std::array<double, 2> pressGround_{};
    std::function<bool(double, double)> handleTest_;
    std::function<bool(std::uint32_t)> instanceSelectable_;
    std::function<bool(std::uint32_t)> instanceVisible_;
    std::optional<War3RenderEditorPresentationTargetV2> editorPresentationTarget_;
    std::uint64_t editorPresentationRevision_ = 0;
    bool editorVisibilityDirty_ = false;
    std::optional<Render::ViewFrameIdentity> dropView_;
    std::uint64_t dropRevision_ = 0;
    QPointF lastClickPoint_{-1e9, -1e9};
    std::uint32_t terrainStride_ = 0;
    std::optional<Render::TerrainPickHit> hoverCoordinate_, selectedCoordinate_;
    std::optional<Render::ViewFrameIdentity> hoverView_, pressView_;
    std::optional<ModelPickPolicy> pressModelPolicy_; // 仅 CPU 编辑器标志快照；不是完成帧回执。
    std::uint64_t pressRevision_ = 0;
    std::uint64_t selectionGestureSerial_ = 0;
    // 每次提交镜头加一。摆放只要求按下后镜头未变：预览模型与动作每帧推进场景编号，不能因此丢弃单击。
    std::uint64_t cameraSerial_ = 0, pressCameraSerial_ = 0;
    unsigned selectionRetries_ = 0; // 一个待完成手势，禁止不断排队或无限等待GPU。
    QString status_;
    QString hoverText_, selectionText_;
};

} // namespace War3Map::Modules

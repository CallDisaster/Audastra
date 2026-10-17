#include "WorldModule.h"
#include "WorldDiagnosticJson.h"
#include "WorldSelectionPresenter.h"
#include "WorldBuildingSplatPresenter.h"
#include "WorldPremPresenter.h"
#include "WorldPathingHelperPresenter.h"
#include "WorldViewport.h"
#include "WorldAnimationPlayer.h"
#include "WorldPlacementPreview.h"
#include "WorldTerrainAssets.h"
#include "WorldTerrainBuild.h"
#include "WorldCliffLayer.h"
#include "WorldPlacementModels.h"
#include "WorldAssetBrowser.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/ModelPicking.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Render/SceneEnvironment.h"

#include "Widgets/ElidedLabel.h"

#include <QFile>
#include <QFileInfo>
#include <QEvent>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QDockWidget>
#include <QMainWindow>
#include <QTabWidget>
#include <QThread>
#include <QTreeView>
#include <QTimer>
#include <QUndoStack>
#include <QVariant>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <tuple>

namespace War3Map::Modules {
namespace {

QString Count(std::size_t value) { return QLocale().toString(static_cast<qulonglong>(value)); }

// 属性栏的悬崖一行：短句给数量，悬停给逐项原因。
QString CliffSummary(const Render::TerrainCliffs* cliffs) {
    if (!cliffs) return QObject::tr("未显示");
    if (cliffs->cliffTiles == 0) return QObject::tr("无");
    QString text = QObject::tr("%1 格").arg(Count(cliffs->placedTiles));
    if (cliffs->rampPieces != 0) text += QObject::tr(" · 坡道 %1 处").arg(Count(cliffs->rampPieces));
    if (const auto skipped = cliffs->cliffTiles - cliffs->placedTiles; skipped != 0)
        text += QObject::tr(" · %1 格按高度色").arg(Count(skipped));
    return text;
}

QString CliffDetails(const Render::TerrainCliffs* cliffs) {
    if (!cliffs || cliffs->cliffTiles == 0) return {};
    QString text = QObject::tr("原版悬崖模型 %1 格；坡道边 %2 处、坡道面 %3 格")
        .arg(Count(cliffs->placedTiles), Count(cliffs->rampPieces), Count(cliffs->rampSurfaceTiles));
    if (const auto skipped = cliffs->cliffTiles - cliffs->placedTiles; skipped != 0)
        text += QObject::tr("\n%1 格按高度色：缺模型 %2、类型无效 %3、层差过大 %4").arg(Count(skipped),
            Count(cliffs->missingModelTiles), Count(cliffs->unknownTypeTiles), Count(cliffs->tooHighTiles));
    return text;
}

// 荒芜：图集材质、近似色调与高度色回退三类格数（blightTiles 含全部带荒芜角的格）。
std::size_t BlightTiles(const Render::TerrainGroundMesh& ground) {
    const auto represented = ground.blightTexturedTiles + ground.blightApproximationTiles;
    return std::max(represented, ground.fallback.blightTiles);
}

QString BlightDetails(const Render::TerrainGroundMesh& ground) {
    QStringList segments;
    if (ground.blightTexturedTiles != 0)
        segments << QObject::tr("荒芜图集材质 %1 格").arg(Count(ground.blightTexturedTiles));
    if (ground.blightApproximationTiles != 0)
        segments << QObject::tr("荒芜 %1 格近似色调（非原版材质）").arg(Count(ground.blightApproximationTiles));
    const auto represented = ground.blightTexturedTiles + ground.blightApproximationTiles;
    if (ground.fallback.blightTiles > represented)
        segments << QObject::tr("荒芜 %1 格保留高度色回退（非原版材质）").arg(Count(ground.fallback.blightTiles - represented));
    return segments.join(QLatin1Char('\n'));
}

QJsonObject GroundBudgetJson(const Render::TerrainGroundMesh& ground) {
    struct Omission {
        Render::TerrainGroundOmissionReason reason;
        const char* code;
    };
    static constexpr Omission omissions[]{
        {Render::TerrainGroundOmissionReason::MissingWaterStyle, "missing-water-style"},
        {Render::TerrainGroundOmissionReason::MissingBlightAtlas, "missing-blight-atlas"},
        {Render::TerrainGroundOmissionReason::InvalidBlightAtlas, "invalid-blight-atlas"},
        {Render::TerrainGroundOmissionReason::TextureSlots, "texture-slots"},
        {Render::TerrainGroundOmissionReason::TextureDimension, "texture-dimension"},
        {Render::TerrainGroundOmissionReason::TextureBytes, "texture-bytes"},
        {Render::TerrainGroundOmissionReason::TextureChainBytes, "texture-chain-bytes"},
        {Render::TerrainGroundOmissionReason::RgbaBytes, "rgba-bytes"},
        {Render::TerrainGroundOmissionReason::Vertices, "vertex-budget"},
        {Render::TerrainGroundOmissionReason::Indices, "index-budget"},
        {Render::TerrainGroundOmissionReason::Batches, "batch-budget"},
        {Render::TerrainGroundOmissionReason::WorkingBytes, "working-bytes"}};
    const auto counts = [](const Render::TerrainGroundLayerCounts& value) {
        QJsonArray reasons;
        for (const auto& omission : omissions)
            if (value.omissionReasons & static_cast<std::uint32_t>(omission.reason)) reasons.append(QString::fromLatin1(omission.code));
        return QJsonObject{{"requested", static_cast<qint64>(value.requested)},
            {"displayed", static_cast<qint64>(value.displayed)}, {"omitted", static_cast<qint64>(value.omitted)},
            {"no_contribution", static_cast<qint64>(value.noContribution)},
            {"omission_reasons", reasons}, {"resource_admission_checked", value.resourceAdmissionChecked},
            {"texture_slots_before_admission", static_cast<qint64>(value.textureSlotsBeforeAdmission)}};
    };
    const auto& budget = ground.budget;
    return {{"policy", budget.policy == Render::TerrainGroundBudgetPolicy::PreserveBasePreview ? "preserve-base-preview" : "strict"},
        {"reduced", budget.reduced}, {"reduced_scope", "geometry-batches-working-bytes"},
        {"base", counts(budget.base)}, {"transitions", counts(budget.transitions)},
        {"blight", counts(budget.blight)}, {"water", counts(budget.water)},
        {"resource_state", QJsonObject{{"requested_ground_texture_slots", static_cast<qint64>(budget.requestedGroundTextureSlots)},
            {"ground_texture_slots", static_cast<qint64>(budget.groundTextureSlots)},
            {"final_texture_slots", static_cast<qint64>(budget.finalTextureSlots)},
            {"texture_slot_limit", static_cast<qint64>(budget.textureSlotLimit)},
            {"water_style_available", budget.waterStyleAvailable},
            {"water_source_texture_available", budget.waterSourceTextureAvailable},
            {"blight_atlas_available", budget.blightAtlasAvailable},
            {"blight_atlas_validation_performed", budget.blightAtlasValidationPerformed},
            {"blight_atlas_valid", budget.blightAtlasValid}}},
        {"hidden_base_tiles", static_cast<qint64>(ground.hiddenTileCount)}};
}

QString GroundOmissionDetails(const Render::TerrainGroundLayerCounts& value) {
    QStringList reasons;
    const auto add = [&](Render::TerrainGroundOmissionReason reason, const QString& text) {
        if (value.omissionReasons & static_cast<std::uint32_t>(reason)) reasons << text;
    };
    add(Render::TerrainGroundOmissionReason::MissingWaterStyle, QObject::tr("缺少水面样式"));
    add(Render::TerrainGroundOmissionReason::MissingBlightAtlas, QObject::tr("缺少荒芜图集"));
    add(Render::TerrainGroundOmissionReason::InvalidBlightAtlas, QObject::tr("荒芜图集无效"));
    add(Render::TerrainGroundOmissionReason::TextureSlots, QObject::tr("纹理槽已满"));
    add(Render::TerrainGroundOmissionReason::TextureDimension, QObject::tr("纹理尺寸超过预算"));
    add(Render::TerrainGroundOmissionReason::TextureBytes, QObject::tr("纹理 mip0 字节超过预算"));
    add(Render::TerrainGroundOmissionReason::TextureChainBytes, QObject::tr("纹理 mip 链字节超过预算"));
    add(Render::TerrainGroundOmissionReason::RgbaBytes, QObject::tr("RGBA 总量超过预算"));
    add(Render::TerrainGroundOmissionReason::Vertices, QObject::tr("顶点超过预算"));
    add(Render::TerrainGroundOmissionReason::Indices, QObject::tr("索引超过预算"));
    add(Render::TerrainGroundOmissionReason::Batches, QObject::tr("批次超过预算"));
    add(Render::TerrainGroundOmissionReason::WorkingBytes, QObject::tr("整体工作载荷超过预算"));
    if (reasons.isEmpty()) return {};
    QString result = reasons.join(QObject::tr("、"));
    if (value.resourceAdmissionChecked)
        result += QObject::tr("（资源准入前已用 %1 个纹理槽）").arg(Count(value.textureSlotsBeforeAdmission));
    return result;
}

QString GroundBudgetDetails(const Render::TerrainGroundMesh& ground) {
    const auto counts = [](const QString& name, const Render::TerrainGroundLayerCounts& value) {
        QString text = QObject::tr("%1：请求 %2、显示 %3、质量省略 %4、零贡献 %5")
            .arg(name, Count(value.requested), Count(value.displayed), Count(value.omitted), Count(value.noContribution));
        if (const auto reason = GroundOmissionDetails(value); !reason.isEmpty()) text += QObject::tr("；原因：%1").arg(reason);
        return text;
    };
    const auto& budget = ground.budget;
    QStringList lines{QObject::tr("基础层：请求 %1、保留 %2、省略 %3（悬崖模型另覆盖 %4 格）")
        .arg(Count(budget.base.requested), Count(budget.base.displayed), Count(budget.base.omitted), Count(ground.hiddenTileCount)),
        counts(QObject::tr("普通过渡遮罩"), budget.transitions), counts(QObject::tr("精确荒芜遮罩"), budget.blight),
        counts(QObject::tr("水面"), budget.water)};
    if (budget.reduced)
        lines << QObject::tr("基础贴图预算预览：完整基础层 → 全图水面 → 全图普通过渡 → 精确荒芜遮罩；每类全留或全省略，不代表完整原版保真。");
    else if (budget.water.omitted || budget.blight.omitted || budget.transitions.omitted)
        lines << QObject::tr("未触发几何预览降级；资源省略仍见各层原因，不代表完整原版保真。");
    lines << QObject::tr("纹理槽：请求地表预检 %1，保留地表 %2，最终全部 %3，上限 %4")
        .arg(Count(budget.requestedGroundTextureSlots), Count(budget.groundTextureSlots),
            Count(budget.finalTextureSlots), Count(budget.textureSlotLimit));
    return lines.join(QLatin1Char('\n'));
}

QString EnvironmentSummary(const Render::SceneEnvironment* environment) {
    if (!environment) return QObject::tr("默认光照");
    const auto& v = environment->values;
    QString text = v.fogMode == WAR3_RENDER_FOG_NONE ? QObject::tr("无雾")
        : QObject::tr("雾 %1–%2").arg(static_cast<double>(v.fogStart), 0, 'f', 0).arg(static_cast<double>(v.fogEnd), 0, 'f', 0);
    text += environment->shadow.empty() ? QObject::tr(" · 无静态阴影") : QObject::tr(" · 静态阴影");
    return text;
}

QString SourceLine(const std::string& path, const std::string& sha) {
    return QString::fromUtf8(path) + QStringLiteral(" · ") + QString::fromStdString(sha);
}

QString DiagnosticText(const std::vector<Diagnostic>& diagnostics) { return WorldDiagnosticText(diagnostics); }
} // namespace

QString WorldDiagnosticText(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error)
        return QString::fromUtf8(diagnostic.message);
    return QObject::tr("地形数据不可用");
}

Result<WorldTerrainMesh> BuildWorldTerrainMesh(const MapData::TerrainGridSnapshot& terrain) {
    return Render::BuildTerrainPreviewMesh(terrain);
}

WorldModule::WorldModule(QtProject::EditorProject& project, const War3RenderPluginApiV2* backend)
    : project_(project), backend_(backend) {
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &WorldModule::Refresh);
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, &WorldModule::Refresh);
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, &WorldModule::Refresh);
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsReplaced, this, &WorldModule::Refresh);
    for (auto* stack : project_.objectUndoStacks_)
        if (stack) connect(stack, &QUndoStack::indexChanged, this, &WorldModule::Refresh);
    if (project_.mapDataUndoStack_)
        connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, &WorldModule::RefreshMapEdits);
}

WorldModule::~WorldModule() {
    ++generation_;
    // QWidget 会先删除子控件，再由 QObject 断开连接。视口清场景会同步发出坐标/
    // 状态信号，此时工具栏或属性 QLabel 可能已释放；先关闭模块级 UI 回调。
    if (page_) {
        page_->removeEventFilter(this);
        QObject::disconnect(page_, nullptr, this, nullptr);
        for (auto* child : page_->findChildren<QObject*>()) {
            child->removeEventFilter(this);
            QObject::disconnect(child, nullptr, this, nullptr);
        }
    }
    // 播放调度器引用 viewport，必须先于 page/viewport 析构，迟到 worker 无 UI 回调。
    delete placementPreview_;
    placementPreview_ = nullptr;
    delete selectionPresenter_; selectionPresenter_ = nullptr;
    delete buildingSplatPresenter_; buildingSplatPresenter_ = nullptr;
    delete premPresenter_; premPresenter_ = nullptr;
    delete cliffLayer_; cliffLayer_ = nullptr;
    delete animation_;
    animation_ = nullptr;
    // CPU 解码线程只持有自有字节及结果；QObject 断开回调，绝不借用地图/视口指针。
    delete page_.data();
}

QUndoStack* WorldModule::UndoStack() { return project_.mapDataUndoStack_; }

Shell::ModuleInfo WorldModule::Info() const {
    return {"world", tr("世界"), "lucide/map", QKeySequence(Qt::ALT | Qt::Key_9)};
}

// 布局：顶栏工具 · [左侧工具面板 | 视口 | 右侧属性] · 单行状态栏。视口占满中间，
// 文字说明收进属性栏的逐项概况与状态栏，不再在视口下方堆叠段落。
QWidget* WorldModule::Widget() {
    if (page_) return page_;
    page_ = new QWidget;
    page_->setObjectName(QStringLiteral("worldModule"));
    page_->installEventFilter(this);
    auto* layout = new QVBoxLayout(page_);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    viewport_ = new WorldViewport(page_, backend_);
    connect(viewport_, &WorldViewport::PluginFailed, this, &WorldModule::RenderPluginFailed);
    BuildToolbar(layout);
    worldDockHost_ = new QMainWindow(page_);
    // QMainWindow 即使传入父控件也默认是顶层窗口；此处必须随世界页一起显示。
    worldDockHost_->setWindowFlags(Qt::Widget);
    worldDockHost_->setObjectName(QStringLiteral("worldDockHost"));
    worldDockHost_->setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    worldDockHost_->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    worldDockHost_->setCentralWidget(viewport_);
    const auto dock = [this](const char* name, const QString& title, QWidget* widget, Qt::DockWidgetArea area) {
        auto* result = new QDockWidget(title, worldDockHost_); result->setObjectName(QLatin1String(name));
        result->setAllowedAreas(Qt::AllDockWidgetAreas); result->setWidget(widget);
        worldDockHost_->addDockWidget(area, result); return result;
    };
    toolDock_ = dock("worldToolDock", tr("场景目录"), BuildToolPanel(), Qt::LeftDockWidgetArea);
    assetBrowser_ = new WorldAssetBrowser(project_.iconProvider_.get(), backend_, worldDockHost_, backend_ == nullptr);
    assetDock_ = dock("worldAssetDock", tr("资源浏览器"), assetBrowser_, Qt::LeftDockWidgetArea);
    inspectorDock_ = dock("worldInspectorDock", tr("属性"), BuildInspector(), Qt::RightDockWidgetArea);
    worldDockHost_->tabifyDockWidget(toolDock_, assetDock_); toolDock_->raise();
    worldDockHost_->resizeDocks({toolDock_, inspectorDock_}, {264, 292}, Qt::Horizontal);
    assetBrowser_->SetVerticalDock(true);
    connect(assetDock_, &QDockWidget::dockLocationChanged, assetBrowser_, [this](Qt::DockWidgetArea area) {
        assetBrowser_->SetVerticalDock(area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea);
    });
    layout->addWidget(worldDockHost_, 1);
    BuildStatusBar(layout);
    ConnectEditing();
    UpdateToolbarLayout();
    SetupAnimationControls();
    if (!retainedViewState_.isEmpty()) RestoreViewState(retainedViewState_);
    Refresh();
    return page_;
}

// 属性栏“地图”概况：每项一行短句，来源与原因放在悬停说明里。
void WorldModule::ShowSceneFacts(const WorldTerrainLoaded& loaded, const Render::TerrainGroundMesh* ground,
    const QString& missingGround) {
    if (!sceneGround_) return;
    if (ground) {
        QString text = tr("纹理 %1 格").arg(Count(ground->texturedTiles));
        const auto layers = ground->overlayTiles.size() - std::min(ground->overlayTiles.size(), ground->blightTexturedTiles);
        if (layers != 0) text += tr(" · 过渡 %1 层").arg(Count(layers));
        if (ground->fallback.totalTiles != 0) text += tr(" · 高度色 %1 格").arg(Count(ground->fallback.totalTiles));
        if (const auto blight = BlightTiles(*ground); blight != 0) text += tr(" · 荒芜 %1 格").arg(Count(blight));
        if (ground->budget.reduced) text += tr(" · 基础贴图预览");
        sceneGround_->setText(text);
        sceneGround_->setProperty("groundBudget", QVariant::fromValue(GroundBudgetJson(*ground)));
        QStringList tip{viewport_->HasTerrainMips() ? tr("原始 mip 链") : tr("仅 mip0")};
        tip << GroundBudgetDetails(*ground);
        if (const auto blight = BlightDetails(*ground); !blight.isEmpty()) tip << blight;
        for (const auto& texture : ground->textures) tip << SourceLine(texture->source.canonicalPath, texture->sourceSha256);
        sceneGround_->setToolTip(tip.join(QLatin1Char('\n')));
        QString water = ground->waterFlaggedTiles == 0 ? tr("无") : tr("%1 格").arg(Count(ground->waterTiles.size()));
        if (ground->budget.water.omitted != 0) water += tr(" · 省略 %1 格").arg(Count(ground->budget.water.omitted));
        if (ground->budget.water.noContribution != 0) water += tr(" · 零贡献 %1 格").arg(Count(ground->budget.water.noContribution));
        sceneWater_->setText(water);
        QString waterDetails = tr("水面：请求 %1、显示 %2、质量省略 %3、零贡献 %4")
            .arg(Count(ground->budget.water.requested), Count(ground->budget.water.displayed),
                Count(ground->budget.water.omitted), Count(ground->budget.water.noContribution));
        if (const auto reason = GroundOmissionDetails(ground->budget.water); !reason.isEmpty())
            waterDetails += tr("\n原因：%1").arg(reason);
        if (!ground->waterFrameSourcePath.empty()) waterDetails += QLatin1Char('\n')
            + SourceLine(ground->waterFrameSourcePath, ground->waterFrameSourceSha256);
        sceneWater_->setToolTip(waterDetails);
    } else {
        sceneGround_->setText(tr("高度预览"));
        sceneGround_->setProperty("groundBudget", QVariant{});
        sceneGround_->setToolTip(missingGround.isEmpty() ? loaded.materialNote : missingGround);
        sceneWater_->setText(tr("未显示")); sceneWater_->setToolTip({});
    }
    sceneCliffs_->setText(CliffSummary(ground ? cliffs_.get() : nullptr));
    sceneCliffs_->setToolTip(CliffDetails(ground ? cliffs_.get() : nullptr));
    sceneLighting_->setText(EnvironmentSummary(loaded.environment.get()));
    sceneNotes_->setVisible(!loaded.materialNote.isEmpty());
    sceneNotes_->setText(ground && ground->budget.reduced ? tr("已保留基础贴图，部分地形细节未显示")
        : tr("部分资源不可用，已按近似方式显示"));
    sceneNotes_->setToolTip(loaded.materialNote);
    terrainProblems_.clear();
    QStringList lines = loaded.materialNote.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (!missingGround.isEmpty()) lines.prepend(missingGround);
    for (const auto& line : lines)
        terrainProblems_.push_back({DiagnosticSeverity::Warning, {}, "WORLD_TERRAIN_RESOURCE", tr("地形：%1").arg(line).toStdString(), std::nullopt});
    PublishResourceProblems();
}

// 属性栏“地图”概况回到未读取状态（换图、重新读取时）。
void WorldModule::ResetSceneFacts() {
    terrainProblems_.clear(); modelVariants_.clear();
    project_.ReplaceDiagnostics(QStringLiteral("World.Resources"), {});
    for (auto* label : {sceneTerrain_, sceneGround_, sceneWater_, sceneCliffs_, sceneLighting_}) {
        if (!label) continue;
        label->setText(QStringLiteral("—")); label->setToolTip({});
    }
    if (modelStatus_) {
        modelStatus_->setText(tr("尚未读取")); modelStatus_->setToolTip({});
        modelStatus_->setProperty("fullDiagnostics", QString());
    }
    if (sceneNotes_) { sceneNotes_->clear(); sceneNotes_->setToolTip({}); sceneNotes_->hide(); }
    if (sceneGround_) sceneGround_->setProperty("groundBudget", QVariant{});
}

// 有地图数据编辑会话时取其当前地形（含未保存的笔刷修改），否则读来源成员。
Result<std::vector<std::byte>> WorldModule::ReadTerrain() const {
    if (project_.mapEdits_ && project_.mapEdits_->Terrain()) return MapData::SerializeMapTerrain(*project_.mapEdits_->Terrain());
    return project_.ReadMapMember("war3map.w3e");
}

void WorldModule::RecordTerrainLoad(const char* stage, std::vector<Diagnostic> diagnostics, bool failed) {
    terrainLoadStage_ = QString::fromLatin1(stage);
    terrainLoadDiagnostics_ = std::move(diagnostics);
    terrainLoadFailed_ = failed;
}

void WorldModule::RecordModelLoad(const char* stage, std::vector<Diagnostic> diagnostics, bool failed) {
    modelLoadStage_ = QString::fromLatin1(stage);
    modelLoadDiagnostics_ = std::move(diagnostics);
    modelLoadGeneration_ = generation_;
    modelLoadFailed_ = failed;
}

QString WorldModule::MapKey() const {
    const auto& path = project_.sourceArchivePath_.empty() ? project_.projectRoot_ : project_.sourceArchivePath_;
    return path.empty() ? QString{} : QString::fromStdWString(path.lexically_normal().wstring());
}

WorldViewport* WorldModule::ViewportForTools() const {
    return viewport_ && !shownMap_.isEmpty() && shownMap_ == MapKey() && !reloadQueued_
        && !worker_ && !pendingTerrainRender_ ? viewport_ : nullptr;
}

void WorldModule::Refresh() {
    ++selectionCatalogRevision_;
    if (premPresenter_) premPresenter_->Clear();
    if (animation_) animation_->Clear();
    if (placementPreview_) placementPreview_->Clear();
    if (selectionPresenter_) selectionPresenter_->Clear();
    if (buildingSplatPresenter_) buildingSplatPresenter_->Clear();
    buildingSplatGeneration_ = Render::NextAnimationSerial();
    if (modelStatus_) modelStatus_->setProperty("buildingSplatPreview", QVariant{});
    instanceRegistry_.reset(); stagedInstanceRegistry_.reset(); instanceRevisionGate_.reset();
    pendingPlacements_.Clear(); placementCompletedScene_.reset(); placementCompletedFrame_.reset();
    const auto generation = ++generation_;
    RecordTerrainLoad("awaiting-page");
    RecordModelLoad("awaiting-terrain");
    terrainPathingPending_ = project_.mapEdits_ && !project_.mapEdits_->VerifyTerrainPathingForSave().Succeeded();
    if (terrainPathingWarning_) terrainPathingWarning_->setVisible(terrainPathingPending_);
    ++terrainRenderSerial_;
    pendingTerrainRender_.reset();
    resumeBrushAfterRender_ = false;
    strokeBase_.reset(); strokePoints_.clear();
    if (terrainToggle_ && terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
    // 同一张地图重新读取时保留当前视角；换图时下面的比较不成立，回到默认游戏视角。
    if (viewport_ && viewport_->VertexCount() != 0 && !shownMap_.isEmpty()) {
        pendingCamera_ = viewport_->SaveCamera(); pendingCameraMap_ = shownMap_;
    }
    shownMap_.clear();
    terrain_.reset();
    rawTerrain_.reset();
    terrainCache_.reset();
    waterFrameVisible_ = false;
    UpdateWaterFrameControl();
    cliffs_.reset();
    cliffsInLayer_ = false; if (cliffLayer_) cliffLayer_->Clear();
    scene_.reset(); requests_.clear(); placementIds_.clear(); unresolvedPlacements_.clear();
    emptyModelDiagnosticVersion_.reset(); instanceDiagnosticVersion_.reset();
    pathingHelperCache_.reset(); pathingHelperPresentation_.reset(); pathingHelperRequests_.clear(); stagedPresentations_.clear();
    if (modelStatus_) {
        modelStatus_->setProperty("pathingHelperCount", QVariant::fromValue(qulonglong{0}));
        modelStatus_->setProperty("pathingHelperUnknownFootprints", QVariant::fromValue(qulonglong{0}));
    }
    if (selectionMapKey_ != MapKey()) ClearSelection();
    regions_.clear(); cameras_.clear();
    if (const auto document = CurrentDocument()) CaptureAuxiliary(*document);
    if (viewport_) (void)viewport_->SetOverlayLines({});
    RefreshAssetBrowser();
    if (!page_) return;
    viewport_->ClearMesh(generation);
    RefreshTerrainPalette();
    RefreshOutliner();
    ResetSceneFacts();
    UpdateInspector();
    if (!page_->isVisible()) { needsRefresh_ = true; RecordTerrainLoad("deferred-hidden"); return; }
    needsRefresh_ = false;
    if (worker_) {
        reloadQueued_ = true; RecordTerrainLoad("waiting-for-worker");
        terrainStatus_->setText(tr("正在切换地形…")); return;
    }
    reloadQueued_ = false;
    const auto sourceRaw = project_.mapEdits_ ? project_.mapEdits_->Terrain() : nullptr;
    RecordTerrainLoad("reading-source");
    auto bytes = ReadTerrain();
    if (!bytes.Succeeded()) {
        RecordTerrainLoad("source-read-failed", bytes.diagnostics, true);
        terrainStatus_->setText(DiagnosticText(bytes.diagnostics)); return;
    }
    RecordTerrainLoad("loading-terrain");
    terrainStatus_->setText(tr("正在读取地形…"));
    sceneGround_->setText(tr("正在读取地表资源…"));
    auto sources = CaptureWorldTerrainSources(project_);
    // 雾与光照环境取当前文档（含未保存的地图设置修改）。
    std::optional<WorldEnvironmentInputs> settings;
    if (const auto document = CurrentDocument(); document && document->Settings())
        settings = CaptureWorldEnvironment(&*document->Settings());
    environmentInputs_ = settings;
    // 静态阴影可缺省；读取失败视为没有阴影。
    std::optional<std::vector<std::byte>> shadow;
    if (auto shd = project_.ReadMapMember("war3map.shd"); shd.Succeeded()) shadow = std::move(*shd.value);
    auto result = std::make_shared<Result<WorldTerrainLoaded>>();
    auto cache = std::make_shared<std::shared_ptr<WorldTerrainCache>>();
    auto* worker = QThread::create([bytes = std::move(*bytes.value), sources = std::move(sources), settings, shadow = std::move(shadow), result, cache] {
        *result = LoadWorldTerrain(bytes, sources, settings, shadow, cache.get());
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, generation, result, cache, sourceRaw] {
        worker_ = nullptr;
        if (reloadQueued_) { Refresh(); return; }
        if (generation != generation_ || !page_) return;
        if (!result->Succeeded()) {
            RecordTerrainLoad("terrain-load-failed", result->diagnostics, true);
            terrainStatus_->setText(DiagnosticText(result->diagnostics)); return;
        }
        if (*cache && (*cache)->reader) {
            // worker 完成后到此 queued 回调之间，源路径仍可能被替换。
            const auto current = (*cache)->reader->VerifyCurrentSource();
            if (!current.Succeeded()) {
                RecordTerrainLoad("terrain-source-changed", current.diagnostics, true);
                terrainStatus_->setText(DiagnosticText(current.diagnostics)); return;
            }
        }
        terrainCache_ = std::move(*cache);
        auto& mesh = result->value->mesh;
        const auto vertexCount = mesh.vertices.size(), triangleCount = mesh.indices.size() / 3;
        const auto displayRevision = ++viewportRevision_;
        if (!viewport_->SetMesh(std::move(mesh.vertices), std::move(mesh.indices), displayRevision)) {
            RecordTerrainLoad("mesh-submit-failed", Status::Failure("World", "WORLD_TERRAIN_MESH_SUBMIT",
                "地形网格提交失败；插件原因见同次 backend 诊断。").diagnostics, true);
            terrainStatus_->setText(tr("地形网格提交失败")); return;
        }
        terrain_ = result->value->terrain;
        rawTerrain_ = sourceRaw ? sourceRaw : result->value->rawTerrain;
        if (!viewport_->SetPickingTerrain(terrain_, mesh.stride, displayRevision)) {
            RecordTerrainLoad("picking-bind-failed", Status::Failure("World", "WORLD_TERRAIN_PICKING_BIND",
                "地形拾取数据未绑定当前显示修订。").diagnostics, true);
            terrainStatus_->setText(tr("地形已显示，但坐标拾取数据不匹配")); return;
        }
        const auto& ground = result->value->ground;
        const bool textured = ground && viewport_->SetGroundMesh(ground);
        waterFrameVisible_ = textured && !ground->waterOmitted && !ground->waterTiles.empty();
        UpdateWaterFrameControl();
        if (result->value->environment) (void)viewport_->SetEnvironment(result->value->environment); // 失败时状态行说明
        cliffs_ = textured ? result->value->cliffs : nullptr;
        ShowSceneFacts(*result->value, textured ? ground.get() : nullptr,
            ground && !textured ? WorldDiagnosticText(viewport_->LastGroundFailure().diagnostics) : QString{});
        sceneTerrain_->setText(tr("%1 × %2 顶点 · %3 三角形%4")
            .arg(mesh.sourceWidth).arg(mesh.sourceHeight)
            .arg(QLocale().toString(static_cast<qulonglong>(textured ? ground->indices.size() / 3 : triangleCount)))
            .arg(mesh.stride > 1 ? tr(" · 每 %1 格取样").arg(mesh.stride) : QString{}));
        sceneTerrain_->setToolTip(tr("显示 %1 顶点").arg(QLocale().toString(
            static_cast<qulonglong>(textured ? ground->vertices.size() : vertexCount))));
        terrainStatus_->setText(tr("地形已就绪%1").arg(mesh.stride > 1 ? tr(" · 大地图按每 %1 格取样显示，不可笔刷").arg(mesh.stride) : QString{}));
        RecordTerrainLoad("prepared", result->diagnostics);
        shownMap_ = MapKey();
        RefreshTerrainPalette();
        if (!pendingCamera_.isEmpty() && pendingCameraMap_ == shownMap_) (void)viewport_->RestoreCamera(pendingCamera_);
        pendingCamera_ = {}; pendingCameraMap_.clear();
        StartModels(generation);
        PresentSelection(); // typed 属性独立于模型加载；地形就绪后重新启用现有选择。
        if (project_.mapEdits_ && project_.mapEdits_->Terrain() != rawTerrain_)
            RefreshMapEdits();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

void WorldModule::StartModels(std::uint64_t generation) {
    if (animation_ && viewport_->HasAnimationSupport()) { StartAnimatedModels(generation); return; }
    const auto reason = tr("当前渲染插件不具备世界动画能力；仅显示静态姿态，播放控件不可用。");
    if (animation_) emit animation_->Message(reason);
    StartStaticModels(generation, reason);
}

void WorldModule::ReportModelPreparationFailure(const QString& stage, const std::vector<Diagnostic>& diagnostics) {
    RecordModelLoad("failed", diagnostics, true);
    project_.AddDiagnostics(diagnostics);
    const auto reason = tr("%1：%2").arg(stage, WorldDiagnosticText(diagnostics));
    QStringList details{reason};
    for (const auto& diagnostic : diagnostics)
        details << QString::fromUtf8(diagnostic.code + "：" + diagnostic.message);
    if (modelStatus_) {
        modelStatus_->setText(reason);
        modelStatus_->setToolTip(details.join(QLatin1Char('\n')));
    }
    if (animation_) emit animation_->Message(reason);
}

QJsonObject WorldModule::PlaybackDiagnosticSnapshot() const {
    const auto number = [](auto value) { return QString::number(static_cast<qulonglong>(value)); };
    const auto widgetState = [](const QWidget* widget) {
        QJsonObject result{{"exists", widget != nullptr}};
        if (widget) {
            result.insert("effective_enabled", widget->isEnabled());
            result.insert("effective_disabled", !widget->isEnabled());
            result.insert("locally_disabled", widget->testAttribute(Qt::WA_ForceDisabled));
            result.insert("visible", widget->isVisible());
            result.insert("hidden", widget->isHidden());
            WorldDiagnosticJson::Text(result, "tooltip", widget->toolTip());
        }
        return result;
    };
    QJsonObject player{{"exists", animation_ != nullptr}};
    QJsonObject serviceScene{{"present", false}}, acceptedFrame{{"present", false}};
    if (animation_) {
        player.insert("ready", animation_->Ready());
        player.insert("playing", animation_->Playing());
        player.insert("busy", animation_->Busy());
        player.insert("instances_busy", animation_->InstancesBusy());
        WorldDiagnosticJson::Text(player, "last_diagnostic", animation_->LastDiagnostic());
        if (const auto scene = animation_->Scene()) {
            serviceScene = {{"present", true}, {"scene_version", number(scene->sceneVersion)},
                {"generation", number(scene->generation)}, {"document_revision", number(scene->documentRevision)},
                {"source_identity", QString::fromUtf8(scene->sourceIdentity)},
                {"animated_instances", number(scene->animatedInstances)},
                {"particle_instances", number(scene->particleInstanceCount)},
                {"dynamic_material_alphas", scene->dynamicMaterialAlphas}, {"dynamic_colors", scene->dynamicColors}};
        }
        if (const auto frame = animation_->Frame()) {
            acceptedFrame = {{"present", true}, {"scene_version", number(frame->sceneVersion)},
                {"content_version", number(frame->contentVersion)}, {"document_revision", number(frame->documentRevision)}};
        }
    }
    // 所有 enabled 值来自控件当前有效状态，不能重新推导一份启用公式。
    QJsonObject buttons{{"play", widgetState(animationPlay_)}, {"pause", widgetState(animationPause_)},
        {"step", widgetState(animationStep_)}};
    QJsonObject model{{"exists", modelStatus_ != nullptr}};
    if (modelStatus_) {
        WorldDiagnosticJson::Text(model, "text", modelStatus_->text());
        WorldDiagnosticJson::Text(model, "details", modelStatus_->toolTip());
    }
    QJsonObject loading{{"terrain_worker", worker_ != nullptr}, {"model_worker", modelWorker_ != nullptr},
        {"terrain_reload_queued", reloadQueued_}, {"needs_refresh", needsRefresh_},
        {"placements_dirty", placementsDirty_}, {"terrain_render_pending", pendingTerrainRender_.has_value()},
        {"generation", number(generation_)}, {"model_generation", number(modelGeneration_)},
        {"terrain_render_serial", number(terrainRenderSerial_)}, {"viewport_revision", number(viewportRevision_)}};
    QJsonObject result{{"schema_version", 1}, {"player", player}, {"service_scene", serviceScene},
        {"accepted_frame", acceptedFrame}, {"buttons", buttons}, {"page", widgetState(page_)},
        {"model_status", model}, {"loading", loading}};
    const auto terrainDiagnostics = WorldDiagnosticJson::Diagnostics(terrainLoadDiagnostics_);
    QJsonObject terrainLoad{{"scope", "source-load"}, {"stage", terrainLoadStage_},
        {"failed", terrainLoadFailed_}, {"generation", number(generation_)},
        {"worker_running", worker_ != nullptr}, {"reload_queued", reloadQueued_}, {"needs_refresh", needsRefresh_},
        {"has_terrain", terrain_ != nullptr}, {"has_raw_terrain", rawTerrain_ != nullptr},
        {"vertex_count", number(viewport_ ? viewport_->VertexCount() : 0)},
        {"completion_proven", false}, {"diagnostics", terrainDiagnostics},
        {"diagnostics_total", number(terrainLoadDiagnostics_.size())},
        {"diagnostics_omitted", number(terrainLoadDiagnostics_.size() - terrainDiagnostics.size())}};
    WorldDiagnosticJson::Text(terrainLoad, "status_text", terrainStatus_ ? terrainStatus_->text() : QString{});
    result.insert("terrain_load", terrainLoad);
    const auto modelDiagnostics = WorldDiagnosticJson::Diagnostics(modelLoadDiagnostics_);
    result.insert("model_load", QJsonObject{{"stage", modelLoadStage_}, {"failed", ModelLoadFailed()},
        {"generation", number(modelLoadGeneration_)}, {"worker_running", modelWorker_ != nullptr},
        {"diagnostics", modelDiagnostics}, {"diagnostics_total", number(modelLoadDiagnostics_.size())},
        {"diagnostics_omitted", number(modelLoadDiagnostics_.size() - modelDiagnostics.size())}});
    if (viewport_) result.insert("backend", viewport_->BackendDiagnosticSnapshot());
    // 属性栏“地图”概况的原文与悬停原因，供探针批量查看地形资源问题。
    QJsonObject terrain;
    const auto fact = [&](const char* key, const QLabel* label) {
        if (label) {
            QJsonObject value;
            WorldDiagnosticJson::Text(value, "text", label->text());
            WorldDiagnosticJson::Text(value, "details", label->toolTip());
            terrain.insert(key, value);
        }
    };
    fact("terrain", sceneTerrain_); fact("ground", sceneGround_); fact("water", sceneWater_);
    fact("cliffs", sceneCliffs_); fact("lighting", sceneLighting_);
    if (sceneGround_) {
        const auto budget = sceneGround_->property("groundBudget").value<QJsonObject>();
        if (!budget.isEmpty()) terrain.insert("ground_budget", budget);
    }
    if (sceneNotes_ && !sceneNotes_->isHidden()) WorldDiagnosticJson::Text(terrain, "notes", sceneNotes_->toolTip());
    result.insert("scene_facts", terrain);
    return result;
}

void WorldModule::StartStaticModels(std::uint64_t generation, QString animationReason) {
    RecordModelLoad("capturing-static");
    emptyModelDiagnosticVersion_.reset();
    if (!viewport_->HasModelSupport()) {
        ReportModelPreparationFailure(tr("当前渲染插件不支持模型显示"), Status::Failure("World",
            "WORLD_MODEL_BACKEND_UNAVAILABLE", "渲染插件没有模型显示能力。").diagnostics); return;
    }
    if (modelWorker_) {
        RecordModelLoad("waiting-for-model-worker");
        modelGeneration_ = generation; modelStatus_->setText(tr("等待上一批模型读取结束…")); return;
    }
    const auto document = CurrentDocument();
    if (!document) {
        ReportModelPreparationFailure(tr("地图数据不可用，未显示摆放模型"), Status::Failure("World",
            "WORLD_MODEL_DOCUMENT_UNAVAILABLE", "当前地图文档不可用，不能捕获摆放模型。").diagnostics); return;
    }
    CaptureAuxiliary(*document);
    RefreshOutliner();
    RefreshBuildingSplats(false, &*document); // uubs资格独立于后续空模型/解析拒绝。
    auto capture = CaptureWorldPlacements(project_, *document);
    placementsDirty_ = false;
    QString unresolved;
    if (capture.unresolved != 0) unresolved = tr(" · %1 个无模型").arg(Count(capture.unresolved));
    QString unresolvedTip = capture.unresolved == 0 ? QString{}
        : tr("%1 个摆放没有可用模型（未定义对象、模型字段为空或随机单位）").arg(Count(capture.unresolved))
            + (capture.unresolvedCodes.isEmpty() ? QString{} : tr("：%1").arg(capture.unresolvedCodes.join(QStringLiteral("、"))));
    const auto cliffs = cliffs_;
    if (capture.requests.empty() && (!cliffs || cliffs->meshes.empty())) {
        modelStatus_->setText(tr("无%1").arg(unresolved));
        modelStatus_->setToolTip(unresolvedTip);
        (void)viewport_->SetModelScene(nullptr);
        scene_.reset(); if (animation_) animation_->Clear();
        requests_ = std::move(capture.requests); placementIds_ = std::move(capture.ids);
        unresolvedPlacements_ = std::move(capture.unresolvedRows);
        FinishEmptyModelCapture();
        UpdateOverlay();
        return;
    }
    auto sources = CaptureWorldTerrainSources(project_);
    if (!sources.Succeeded()) { ReportModelPreparationFailure(tr("模型资源来源准备失败"), sources.diagnostics); return; }
    const auto counts = tr("装饰物 %1 · 可破坏物 %2 · 单位 %3 · 物品 %4").arg(Count(capture.doodads),
        Count(capture.destructibles), Count(capture.units), Count(capture.items));
    modelStatus_->setText(tr("正在读取…"));
    modelStatus_->setToolTip(counts);
    const auto modelRevision = project_.mapEdits_ ? project_.mapEdits_->Revision() : 0;
    const auto presentationVersion = PlacementPresentationVersion();
    const auto helperIdentity = WorldPathingHelperResourceIdentity(*sources.value, presentationVersion.catalogRevision);
    if (!helperIdentity.Succeeded()) {
        ReportModelPreparationFailure(tr("静态模型的辅助物资源身份准备失败"), helperIdentity.diagnostics); return;
    }
    const auto helperCache = pathingHelperCache_;
    auto preparedHelpers = std::make_shared<std::optional<WorldPathingHelperPrepared>>();
    auto result = std::make_shared<Result<Render::WorldModelScene>>();
    (void)ApplyModelVariantFallbacks(capture, modelVariants_);
    auto placed = std::make_shared<WorldPlacementCapture>(std::move(capture));
    auto variants = std::make_shared<ModelVariantFallbacks>();
    auto* worker = QThread::create([placed, sources = std::move(*sources.value), result, cliffs,
        presentationVersion, helperIdentity = *helperIdentity.value, helperCache, preparedHelpers, variants] {
        try {
            auto reader = WorldResourceReader::Open(sources);
            if (!reader.Succeeded()) { result->diagnostics = reader.diagnostics; return; }
            auto source = std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            const auto valid = source->VerifyCurrentSource();
            if (!valid.Succeeded()) { result->diagnostics = valid.diagnostics; return; }
            Services::PlacementPresentationContext context{presentationVersion, helperIdentity, WorldPathingHelperResourceReader(source)};
            auto helpers = PrepareWorldPathingHelpers(placed->requests, placed->presentations, context, source, helperCache);
            if (!helpers.Succeeded()) { result->diagnostics = std::move(helpers.diagnostics); return; }
            *preparedHelpers = std::move(*helpers.value);
            auto& resolver = source->Resolver();
            ResolveModelVariantFallbacks(*placed, resolver, *variants);
            (void)ApplyModelVariantFallbacks(*placed, *variants);
            *result = Render::BuildWorldModelScene(placed->requests, [&resolver](std::string_view path) {
                auto read = resolver.Read(path);
                if (!read.Succeeded()) return Result<std::vector<std::byte>>{std::nullopt, std::move(read.diagnostics)};
                return Result<std::vector<std::byte>>::Success(std::move(read.value->bytes));
            }, {}, cliffs ? std::span<const Render::BakedModelMesh>(cliffs->meshes) : std::span<const Render::BakedModelMesh>{});
            const auto verified = source->VerifyCurrentSource();
            if (!verified.Succeeded()) *result = {std::nullopt, verified.diagnostics};
        } catch (const std::exception&) {
            *result = Result<Render::WorldModelScene>::Failure("World", "WORLD_MODELS_FAILED", "摆放模型读取失败。");
        }
    });
    modelWorker_ = worker;
    RecordModelLoad("preparing-static");
    connect(worker, &QThread::finished, this, [this, generation, result, placed, counts, unresolved, unresolvedTip, cliffs, animationReason,
        modelRevision, preparedHelpers, presentationVersion, variants] {
        modelWorker_ = nullptr;
        for (const auto& [variant, target] : *variants) modelVariants_.insert_or_assign(variant, target);
        if (!page_) return;
        if (generation != generation_) {
            // 读取期间地形已换代：新地形已就绪时补一次加载。
            if (modelGeneration_ == generation_ && viewport_->VertexCount() != 0) StartModels(generation_);
            return;
        }
        if ((project_.mapEdits_ ? project_.mapEdits_->Revision() : 0) != modelRevision) { StartModels(generation_); return; }
        const auto currentVersion = PlacementPresentationVersion();
        if (currentVersion.document != presentationVersion.document || currentVersion.catalogRevision != presentationVersion.catalogRevision) {
            StartModels(generation_); return;
        }
        if (cliffs_ != cliffs) { StartModels(generation_); return; } // 地形重烘焙已替换旧悬崖网格。
        if (!result->Succeeded()) { ReportModelPreparationFailure(tr("静态模型准备失败"), result->diagnostics); return; }
        auto scene = std::make_shared<Render::WorldModelScene>(std::move(*result->value));
        if (*preparedHelpers && (*preparedHelpers)->cache->sourceOwner) {
            const auto verified = (*preparedHelpers)->cache->sourceOwner->VerifyCurrentSource();
            if (!verified.Succeeded()) { ReportModelPreparationFailure(tr("静态模型来源已变化"), verified.diagnostics); return; }
        }
        if (!viewport_->SetModelScene(scene)) {
            ReportModelPreparationFailure(tr("模型提交失败"), Status::Failure("World", "WORLD_STATIC_MODEL_SUBMIT_FAILED",
                "静态模型没有被渲染插件接受；没有授予当前场景完成资格。").diagnostics); return;
        }
        scene_ = scene; requests_ = std::move(placed->requests); placementIds_ = std::move(placed->ids);
        unresolvedPlacements_ = std::move(placed->unresolvedRows);
        RecordModelLoad("prepared-static");
        pathingHelperRequests_.assign(requests_.size(), 0);
        pathingHelperPresentation_.reset();
        if (*preparedHelpers) {
            pathingHelperCache_ = (*preparedHelpers)->cache;
            pathingHelperRequests_ = (*preparedHelpers)->helperRequests;
            project_.AddDiagnostics((*preparedHelpers)->diagnostics);
            pathingHelperPresentation_ = std::make_shared<const WorldPathingHelperPrepared>(std::move(**preparedHelpers));
        }
        stagedPresentations_ = placed->presentations;
        (void)viewport_->RefreshEditorVisibility();
        RefreshBuildingSplats(true);
        ValidateSelection();
        UpdateOverlay(); UpdateSelectionText();
        if (placementsDirty_) RefreshPlacements();
        QString skipped;
        if (scene->skippedInstances != 0) skipped += tr(" · %1 个不可用").arg(Count(scene->skippedInstances));
        modelStatus_->setText((animationReason.isEmpty() ? QString{} : tr("动画未启用 · ")) +
            tr("%1 个实例 · %2 种模型").arg(Count(scene->instances.size()), Count(scene->models.size())) + unresolved + skipped);
        QStringList notes{counts, tr("%1 张贴图 · 静态姿态").arg(Count(scene->textures.size()))};
        if (!animationReason.isEmpty()) notes << animationReason;
        if (scene->untexturedLayers != 0) notes << tr("%1 个材质层缺贴图按白色显示").arg(Count(scene->untexturedLayers));
        for (const auto& warning : scene->warnings) notes << QString::fromUtf8(warning.message);
        if (!unresolvedTip.isEmpty()) notes << unresolvedTip;
        modelStatus_->setToolTip(notes.join(QLatin1Char('\n')));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

std::optional<MapDocument> WorldModule::CurrentDocument() const {
    if (project_.mapEdits_) return project_.mapEdits_->Snapshot();
    if (project_.mapDocument_) return *project_.mapDocument_;
    return std::nullopt;
}

void WorldModule::CaptureAuxiliary(const MapDocument& document) {
    regions_ = document.Regions() ? document.Regions()->entries : std::vector<MapData::RegionRecord>{};
    cameras_ = document.Cameras() ? document.Cameras()->entries : std::vector<MapData::CameraRecord>{};
}

void WorldModule::RefreshBuildingSplats(bool rebindParent, const MapDocument* capturedDocument) {
    if (!buildingSplatPresenter_ || !viewport_ || !terrain_ || !page_ || generation_ == 0) return;
    // 只由初次模型接收、正式文档编辑/显隐、地形接收和主roster完成调用。
    // 不在动画Changed、UpdateOverlay或鼠标镜头路径捕获整张表。
    auto ownedDocument = capturedDocument ? std::optional<MapDocument>{} : CurrentDocument();
    const auto* document = capturedDocument ? capturedDocument : ownedDocument ? &*ownedDocument : nullptr;
    if (!document || !RefreshSelectionContext() || !worldSelection_.Version()) return;
    const auto version = *worldSelection_.Version();
    auto capture = CaptureWorldBuildingSplats(project_, *document, version);
    if (!capture.Succeeded()) { project_.AddDiagnostics(capture.diagnostics); return; }
    auto sources = CaptureWorldTerrainSources(project_);
    if (!sources.Succeeded()) { project_.AddDiagnostics(sources.diagnostics); return; }
    const auto generation = generation_; const auto mapKey = MapKey(); const auto catalogRevision = selectionCatalogRevision_;
    const auto revision = version.document.revision; const auto sourceSha = version.document.sourceSha256;
    const auto terrain = terrain_;
    Render::AnimationSession session{buildingSplatGeneration_ ? buildingSplatGeneration_ : generation, revision, sources.value->archiveSha};
    buildingSplatPresenter_->SetSource(std::move(*sources.value), session, [this, generation, mapKey, catalogRevision] {
        return page_ && generation_ == generation && MapKey() == mapKey && selectionCatalogRevision_ == catalogRevision;
    });
    std::vector<std::uint8_t> visible; visible.reserve(capture.value->units.size());
    for (const auto& input : capture.value->units) visible.push_back(worldSelection_.Flags(input.owner).visible ? 1 : 0);
    auto registry = instanceRegistry_;
    if (registry && (registry->Binding().mapRevision != revision || registry->Binding().sourceIdentity != sourceSha)) registry.reset();
    Services::WorldInstanceBinding fallback{session.generation, revision,
        project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0, revision, sourceSha};
    std::shared_ptr<const MapDocument> fallbackDocument;
    if (!registry) fallbackDocument = ownedDocument ? std::make_shared<const MapDocument>(std::move(*ownedDocument))
        : std::make_shared<const MapDocument>(*document);
    const auto terrainIdentity = sourceSha + ":shown-terrain:" + std::to_string(generation) + ":" + std::to_string(terrainRenderSerial_);
    buildingSplatPresenter_->Show(std::move(*capture.value), std::move(visible), std::move(fallbackDocument),
        std::move(registry), std::move(fallback), terrain, terrainIdentity, viewport_->DocumentRevision(),
        [this, generation, mapKey, catalogRevision, revision, terrain] {
            if (!page_ || generation_ != generation || MapKey() != mapKey || selectionCatalogRevision_ != catalogRevision
                || terrain_ != terrain || modelWorker_ || (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0) != revision) return false;
            // 主roster有排队前缀时，最新文档贴画不能提前发布到较旧主世界身份上。
            return !animation_ || !animation_->Ready() || !instanceRegistry_ || instanceRegistry_->Binding().mapRevision == revision;
        });
    if (rebindParent) buildingSplatPresenter_->RebindParent();
}

void WorldModule::RefreshPlacements() {
    if (!page_ || !viewport_ || generation_ == 0) return;
    WorldPlacementRefreshTimings refresh;
    const auto snapshotStamp = [this] {
        // 仅用于当前同步调用的owned快照，不跨事务缓存或冒称外部文件长期未改。
        return std::tuple{MapKey(), generation_, selectionCatalogRevision_, project_.mapDocument_.get(),
            project_.mapEdits_.get(), project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0,
            project_.mapEdits_ ? project_.mapEdits_->Revision() : 0, project_.archiveSession_.get(),
            project_.archiveSession_ ? project_.archiveSession_->SourceSha256() : std::string{}};
    };
    auto started = WorldInstanceNowNs();
    const auto capturedStamp = snapshotStamp();
    auto capturedDocument = CurrentDocument();
    const auto auxiliaryDocument = capturedDocument
        ? std::make_shared<const MapDocument>(std::move(*capturedDocument)) : nullptr;
    refresh.snapshotNs = WorldInstanceNowNs() - started;
    started = WorldInstanceNowNs();
    if (auxiliaryDocument) CaptureAuxiliary(*auxiliaryDocument);
    refresh.auxiliaryNs = WorldInstanceNowNs() - started;
    // 新文档贴画在正式roster完成前本就不能发布；成功接收后还有一次带新parent的刷新。
    // 完整增量路径跳过这里的重复整表捕获，旧模型和旧贴画共同保留到完成票据。
    // 非增量/缺场景路径仍在本轮刷新，不能因模型不可用连带丢失建筑贴画。
    refresh.splatsDeferredToRoster = animation_ && instanceRegistry_ && stagedInstanceRegistry_
        && instanceRevisionGate_ && project_.mapEdits_ && viewport_->HasInstanceRosterSupport()
        && viewport_->HasAnimationScene() && !modelWorker_ && !worker_ && scene_ && viewport_->VertexCount() != 0;
    if (!refresh.splatsDeferredToRoster) {
        started = WorldInstanceNowNs();
        RefreshBuildingSplats(false, auxiliaryDocument ? &*auxiliaryDocument : nullptr);
        refresh.buildingSplatsNs = WorldInstanceNowNs() - started;
    }
    const auto refreshOutliner = [this, &snapshotStamp, &capturedStamp, &auxiliaryDocument] {
        const auto begin = WorldInstanceNowNs();
        // 当前修订的文档已自有；只有整份同步身份仍匹配才能供typed查询复用。
        // Qt同步通知若换过地图/会话/修订，就回原捕获路径，不能把旧内容标成新修订。
        RefreshOutliner(capturedStamp == snapshotStamp() ? auxiliaryDocument : nullptr);
        return WorldInstanceNowNs() - begin;
    };
    // 完整增量可先排资源worker，让目录查询/重建与它重叠。仍在本次UI调用返回前
    // 刷新typed选择和目录；缺模型、旧后端或重建路径保持原来的同步顺序。
    const bool outlinerAfterQueue = refresh.splatsDeferredToRoster;
    if (!outlinerAfterQueue) refresh.outlinerNs = refreshOutliner();
    if (modelWorker_ || worker_) { placementsDirty_ = true; return; }
    if (viewport_->VertexCount() == 0) { placementsDirty_ = true; return; }
    if (!scene_) {
        // 异步场景过期时可能尚无可用旧场景；地形完成后仍须消费重试。
        // 明确失败已清 dirty，不在每次刷新时无限重试相同错误。
        if (placementsDirty_) StartModels(generation_);
        return;
    }
    // 已提交的文档编辑可能改变行身份、几何或来源：重建一次新代次，不在 pose tick 重建。
    // 拖动过程中的临时变换仍走 UpdateModelInstances -> 动画服务矩阵更新。
    if (viewport_->HasAnimationScene()) {
        started = WorldInstanceNowNs();
        // 非增量路径的Qt模型通知可能同步重入；只有来源、会话、修订和目录仍相同才复用。
        // 来源身份不完整时保留新捕获路径，Stage内部的receipt/SHA/谱系校验不变。
        refresh.reusedSnapshot = auxiliaryDocument && project_.archiveSession_
            && !project_.archiveSession_->SourceSha256().empty() && capturedStamp == snapshotStamp();
        std::optional<MapDocument> recaptured;
        const auto* document = auxiliaryDocument ? &*auxiliaryDocument : nullptr;
        if (!refresh.reusedSnapshot) {
            const auto currentStamp = snapshotStamp();
            recaptured = CurrentDocument();
            if (currentStamp != snapshotStamp()) {
                if (outlinerAfterQueue) (void)refreshOutliner();
                placementsDirty_ = true; return;
            }
            document = recaptured ? &*recaptured : nullptr;
        }
        refresh.stageSnapshotNs = WorldInstanceNowNs() - started;
        const bool staged = document && StagePlacementInstances(*document, refresh);
        if (outlinerAfterQueue) {
            const auto identity = stagedInstanceTiming_;
            const auto outlinerNs = refreshOutliner();
            if (staged) {
                // player只修改UI独占副本；后台读取的是排队时的独立值，完成回调再合并。
                (void)animation_->RecordInstanceOutlinerTiming(identity, outlinerNs);
                if (stagedInstanceTiming_.generation == identity.generation
                    && stagedInstanceTiming_.editSessionIdentity == identity.editSessionIdentity
                    && stagedInstanceTiming_.documentRevision == identity.documentRevision
                    && stagedInstanceTiming_.commandIdentity == identity.commandIdentity) {
                    stagedInstanceTiming_.refresh.outlinerNs += outlinerNs;
                    stagedInstanceTiming_.refresh.outlinerAfterQueue = true;
                }
            }
        }
        if (staged) return;
        if (refresh.splatsDeferredToRoster) RefreshBuildingSplats(false, document);
        StartModels(generation_); return;
    }
    const auto document = CurrentDocument();
    if (!document) return;
    CaptureAuxiliary(*document);
    auto capture = CaptureWorldPlacements(project_, *document);
    (void)ApplyModelVariantFallbacks(capture, modelVariants_);
    dragStart_.reset(); dragPosition_.reset(); dragRotation_.reset(); dragGroup_.clear();
    regionDraft_.reset(); cameraDraft_.reset();
    std::vector<std::uint32_t> models;
    models.reserve(scene_->instances.size());
    for (const auto& instance : scene_->instances) models.push_back(instance.model);
    if (!Render::ReplaceModelInstances(*scene_, requests_, capture.requests)) {
        StartModels(generation_); // 新的模型变体：重新读取
        return;
    }
    requests_ = std::move(capture.requests); placementIds_ = std::move(capture.ids);
    const bool sameLayout = models.size() == scene_->instances.size() && std::equal(models.begin(), models.end(),
        scene_->instances.begin(), [](std::uint32_t model, const auto& instance) { return model == instance.model; });
    // 数量与模型次序不变（移动、旋转）只更新实例；否则重交场景（几何不重新读取）。
    if (sameLayout) (void)viewport_->UpdateModelInstances();
    else { (void)viewport_->SetModelScene(nullptr); (void)viewport_->SetModelScene(scene_); }
    ValidateSelection();
    UpdateOverlay(); UpdateSelectionText();
}

void WorldModule::RefreshMapEdits() {
    if (!page_ || !viewport_) return;
    RefreshAssetBrowser();
    terrainPathingPending_ = project_.mapEdits_ && !project_.mapEdits_->VerifyTerrainPathingForSave().Succeeded();
    if (terrainPathingWarning_) terrainPathingWarning_->setVisible(terrainPathingPending_);
    // push() 的首次 redo 不改动地图，只同步发出 indexChanged；最终地形已经在 FinishTerrainStroke
    // 排队。此时按钮为防止对旧完成帧下笔而暂时取消勾选，不能把它解释为用户退出笔刷。
    if (pushingTerrainStrokeUndo_) { RefreshPlacements(); return; }
    if (RefreshEnvironment()) return;
    const auto source = project_.mapEdits_ ? project_.mapEdits_->Terrain() : nullptr;
    if (source && source != rawTerrain_) {
        strokeBase_.reset(); strokePoints_.clear();
        resumeBrushAfterRender_ = terrainToggle_ && terrainToggle_->isChecked();
        if (resumeBrushAfterRender_) terrainToggle_->setChecked(false);
        else if (viewport_->StrokeActive()) viewport_->SetBrushMode(false);
        if (terrainToggle_) terrainToggle_->setEnabled(false);
        if (page_->isVisible()) QueueTerrainRender(source, false);
        else needsRefresh_ = true;
    }
    RefreshPlacements();
    // WPM 草稿和撤销不换 W3E 快照；仍须按会话修订刷新当前通行格标记。
    if (CurrentTool() == Tool::Terrain && CurrentBrushKind() == MapData::TerrainBrushKind::PathingRestriction)
        UpdateOverlay();
}

bool WorldModule::RefreshEnvironment() {
    // 地图设置的应用/撤销/重做只经共用撤销栈到达这里；雾变化只重算环境，光照环境变化重新读取。
    if (!environmentInputs_) return false;
    const auto document = CurrentDocument();
    const auto current = document ? CaptureWorldEnvironment(document->Settings() ? &*document->Settings() : nullptr)
                                  : std::nullopt;
    if (!current) return false;
    if (!SameWorldLighting(*current, *environmentInputs_)) { Refresh(); return true; }
    if (SameWorldFog(*current, *environmentInputs_)) return false;
    // 缓存同时被后台地形构建读取；构建中改为完整重新读取。
    if (worker_ || !terrainCache_ || !terrainCache_->environment) { Refresh(); return true; }
    auto environment = std::make_shared<Render::SceneEnvironment>(*terrainCache_->environment);
    Render::ApplySceneFog(*environment, &current->settings, current->terrainFog);
    std::shared_ptr<const Render::SceneEnvironment> shared = std::move(environment);
    terrainCache_->environment = shared;
    environmentInputs_ = current;
    (void)viewport_->SetEnvironment(shared); // 失败时状态行说明
    return false;
}

void WorldModule::QueueTerrainRender(std::shared_ptr<const MapData::TerrainGridSnapshot> source, bool preview) {
    if (!source || !page_ || !viewport_) return;
    if (!page_->isVisible()) { needsRefresh_ = true; return; }
    TerrainRenderRequest request;
    request.source = std::move(source);
    request.preview = preview;
    request.serial = ++terrainRenderSerial_;
    if (preview) { request.brush = strokeBrush_; request.points = strokePoints_; }
    pendingTerrainRender_ = std::move(request); // 拖动期间只保留最新完整前缀，旧完成结果绝不覆盖新笔迹。
    StartTerrainRender();
    UpdateWaterFrameControl();
}

void WorldModule::UpdateWaterFrameControl() {
    if (!waterFrame_) return;
    // 后台构建会按需刷新 cache 中的材质；运行期间只关控件，不读取其可变成员。
    if (worker_ || pendingTerrainRender_) { waterFrame_->setEnabled(false); return; }
    const bool available = terrainCache_ && terrainCache_->water && terrainCache_->waterDefinition
        && terrainCache_->water->texture && waterFrameVisible_;
    const QSignalBlocker blocker(waterFrame_);
    const auto count = available ? terrainCache_->waterDefinition->frameCount : 0u;
    waterFrame_->setRange(0, count ? static_cast<int>(count - 1) : 0);
    waterFrame_->setValue(available ? static_cast<int>(terrainCache_->water->frameIndex) : 0);
    waterFrame_->setSuffix(count ? tr(" / %1").arg(count - 1) : QString{});
    waterFrame_->setVisible(available && count > 1); // 只有一帧或没有水面时不占位置
    waterFrame_->setEnabled(available && count > 1 && !worker_ && !pendingTerrainRender_ && !strokeBase_
        && !(terrainToggle_ && terrainToggle_->isChecked()));
    waterFrame_->setToolTip(available
        ? tr("%1\n第 %2 / %3 帧；只改变当前画面，不修改地图或保存内容。")
            .arg(QString::fromUtf8(terrainCache_->water->frameSourcePath))
            .arg(terrainCache_->water->frameIndex).arg(count - 1)
        : tr("地图没有可用的原版水面材质或当前画面未显示水面。"));
}

void WorldModule::QueueWaterFrame(std::uint32_t frame) {
    if (worker_ || pendingTerrainRender_ || !terrainCache_ || !terrainCache_->water
        || !terrainCache_->waterDefinition || !rawTerrain_ || !waterFrameVisible_
        || !terrainCache_->water->texture
        || strokeBase_ || (terrainToggle_ && terrainToggle_->isChecked())) {
        UpdateWaterFrameControl();
        return;
    }
    if (frame == terrainCache_->water->frameIndex) return;
    TerrainRenderRequest request;
    request.source = rawTerrain_;
    request.serial = ++terrainRenderSerial_;
    request.selectedWaterFrame = frame;
    pendingTerrainRender_ = std::move(request);
    terrainStatus_->setText(tr("正在读取水面第 %1 帧…").arg(frame));
    StartTerrainRender();
    UpdateWaterFrameControl();
}

void WorldModule::StartTerrainRender() {
    if (worker_ || !pendingTerrainRender_ || !page_ || !viewport_) return;
    auto request = std::move(*pendingTerrainRender_);
    pendingTerrainRender_.reset();
    if (!terrainCache_) terrainCache_ = std::make_shared<WorldTerrainCache>(); // 没有原版材质时仍可更新高度预览。
    auto cache = terrainCache_;
    const auto selectedWaterFrame = request.selectedWaterFrame;
    const bool keepPreviewCliffs = request.preview && (request.brush.kind == MapData::TerrainBrushKind::Texture
        || request.brush.kind == MapData::TerrainBrushKind::Blight || request.brush.kind == MapData::TerrainBrushKind::WaterHeight);
    const auto keepCliffs = keepPreviewCliffs || selectedWaterFrame ? cliffs_ : nullptr;
    const auto generation = generation_;
    const auto serial = request.serial;
    const bool preview = request.preview;
    const auto source = request.source;
    auto result = std::make_shared<Result<WorldTerrainLoaded>>();
    // 悬崖在独立层时，新烘焙的悬崖在同一 worker 里准备好层帧，提交地形时一并显示，不重建整张动画场景。
    std::optional<Render::AnimationSession> cliffSession;
    if (cliffsInLayer_ && cliffLayer_ && animation_ && !selectedWaterFrame)
        if (const auto scene = animation_->Scene())
            cliffSession = Render::AnimationSession{scene->generation, scene->documentRevision, scene->sourceIdentity};
    auto* worker = QThread::create([request = std::move(request), cache, keepCliffs, result, cliffSession] {
        if (request.selectedWaterFrame)
            *result = RebuildWorldTerrain(*request.source, *cache, keepCliffs, request.selectedWaterFrame);
        else if (request.preview) {
            auto preview = std::make_shared<MapData::TerrainGridSnapshot>(*request.source);
            auto stroke = MapData::ApplyTerrainStroke(*preview, request.brush, request.points);
            if (!stroke.Succeeded()) { result->diagnostics = std::move(stroke.diagnostics); return; }
            *result = RebuildWorldTerrain(*preview, *cache, keepCliffs);
        } else *result = RebuildWorldTerrain(*request.source, *cache, nullptr);
        if (!cliffSession || !result->Succeeded() || !result->value->cliffs || result->value->cliffs == keepCliffs || !cache->reader) return;
        try {
            auto layer = PrepareWorldCliffLayer(result->value->cliffs, [&cache](std::string_view path, std::uint64_t maximum) {
                return cache->reader->ReadAnimationResource(path, maximum);
            }, *cliffSession);
            if (layer.Succeeded()) result->value->cliffLayer = std::move(*layer.value);
            else result->value->cliffLayerDiagnostics = std::move(layer.diagnostics);
        } catch (const std::exception& error) {
            result->value->cliffLayerDiagnostics.push_back({DiagnosticSeverity::Error, "World", "WORLD_CLIFF_LAYER_FAILED", error.what(), std::nullopt});
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, generation, serial, preview, source, result,
        selectedWaterFrame, cache] {
        worker_ = nullptr;
        if (reloadQueued_) { Refresh(); return; }
        if (generation != generation_ || !page_) return;
        if (serial == terrainRenderSerial_) {
            if (!result->Succeeded()) {
                terrainStatus_->setText(selectedWaterFrame
                    ? tr("水面帧未切换：%1").arg(DiagnosticText(result->diagnostics))
                    : tr("地形预览失败：%1").arg(DiagnosticText(result->diagnostics)));
                if (!preview) resumeBrushAfterRender_ = false;
            } else if (selectedWaterFrame && !result->value->selectedWater) {
                terrainStatus_->setText(tr("水面帧未切换：渲染结果缺少帧身份"));
            } else if (PresentTerrainRender(*result->value, preview, selectedWaterFrame.has_value(), source)
                && selectedWaterFrame) cache->water = std::move(result->value->selectedWater);
        }
        StartTerrainRender();
        UpdateWaterFrameControl();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

bool WorldModule::PresentTerrainRender(WorldTerrainLoaded& loaded, bool preview, bool waterFrameOnly,
    std::shared_ptr<const MapData::TerrainGridSnapshot> source) {
    auto& mesh = loaded.mesh;
    const auto ground = viewport_->HasGroundSupport() ? loaded.ground : nullptr;
    const bool layered = cliffsInLayer_ && cliffLayer_ && ground && loaded.cliffLayer;
    if (waterFrameOnly && (!ground || ground->waterTiles.empty())) {
        terrainStatus_->setText(tr("水面帧未切换：当前渲染器没有显示可替换的水面"));
        return false;
    }
    if (!viewport_->ReplaceTerrainPresentation(std::move(mesh.vertices), std::move(mesh.indices), loaded.terrain,
            mesh.stride, ground, ++viewportRevision_,
            loaded.cliffs ? std::span<const Render::BakedModelMesh>(loaded.cliffs->meshes) : std::span<const Render::BakedModelMesh>{}, layered)) {
        if (!viewport_->LastGroundFailure().Succeeded()) project_.AddDiagnostics(viewport_->LastGroundFailure().diagnostics);
        terrainStatus_->setText(tr("地形网格更新失败，保留此前视图"));
        if (!preview) resumeBrushAfterRender_ = false;
        return false;
    }
    terrain_ = loaded.terrain;
    // 拖动预览与正式提交都把新悬崖层和地面一起显示；地面隐藏的悬崖格不会出现空洞。
    if (layered) cliffLayer_->Show(*loaded.cliffLayer);
    else if (!loaded.cliffLayerDiagnostics.empty()) project_.AddDiagnostics(loaded.cliffLayerDiagnostics);
    if (!preview && !waterFrameOnly) RefreshBuildingSplats();
    if (!preview && !waterFrameOnly) {
        rawTerrain_ = std::move(source); // 渲染器接收并显示该快照后，才允许下一笔从它拾取。
        cliffs_ = ground ? loaded.cliffs : nullptr;
        if (scene_) {
            const auto baked = cliffs_ ? std::span<const Render::BakedModelMesh>(cliffs_->meshes)
                                       : std::span<const Render::BakedModelMesh>{};
            if (viewport_->HasAnimationScene()) {
                // 悬崖在独立层且新层已显示：单位、装饰物等主场景不变，不重建。
                if (!layered || !cliffLayer_->Showing(cliffs_)) StartModels(generation_);
            } else if (Render::ReplaceBakedMeshes(*scene_, baked)) {
                if (!viewport_->SetModelScene(scene_)) modelStatus_->setText(tr("悬崖模型提交失败"));
            } else StartModels(generation_); // 新贴图未在现有场景中时完整读取一次。
        } else if (cliffs_ && !cliffs_->meshes.empty() && !modelWorker_) StartModels(generation_);
    }
    terrainStatus_->setText(preview ? tr("笔刷预览中，松开后提交") : waterFrameOnly
        ? tr("水面帧已切换（只改变画面，不修改地图）") : tr("地形已更新")
        + (mesh.stride > 1 ? tr(" · 降采样显示不可笔刷") : QString{}));
    ShowSceneFacts(loaded, ground.get(), {});
    waterFrameVisible_ = ground && !ground->waterOmitted && !ground->waterTiles.empty();
    UpdateStatusIndicators();
    UpdateOverlay();
    RefreshTerrainPalette();
    if (!preview && resumeBrushAfterRender_) {
        const bool current = project_.mapEdits_ && project_.mapEdits_->Terrain() == rawTerrain_;
        resumeBrushAfterRender_ = false;
        if (current && terrainToggle_ && terrainToggle_->isEnabled()) terrainToggle_->setChecked(true);
    }
    if (placementsDirty_) RefreshPlacements();
    return true;
}

bool WorldModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "world" || (!target.kind.isEmpty() && target.kind != "terrain")) return false;
    (void)Widget();
    return true;
}

bool WorldModule::eventFilter(QObject* watched, QEvent* event) {
    if (watched->objectName() == QStringLiteral("worldToolbar") &&
        (event->type() == QEvent::Resize || event->type() == QEvent::FontChange))
        QTimer::singleShot(0, this, &WorldModule::UpdateToolbarLayout);
    if (watched == page_ && event->type() == QEvent::Show && needsRefresh_)
        QTimer::singleShot(0, this, [this] { if (page_ && page_->isVisible() && needsRefresh_) Refresh(); });
    if (watched == outliner_ && event->type() == QEvent::Show && outlinerDirty_)
        QTimer::singleShot(0, this, [this] {
            // 等 stacked page/dock 完成 Show 后再建目录；多次 Show 只消费最后状态一次。
            if (outliner_ && outliner_->isVisible() && outlinerDirty_) RefreshOutliner();
        });
    // 用户开始操作视口时停止正在进行的视角过渡，避免与拖动、缩放争夺相机。
    if (watched == viewport_ && cameraFlight_
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel)) cameraFlight_->stop();
    return QObject::eventFilter(watched, event);
}

// 视图状态版本 2 起记录所属地图；旧版本（不分地图的整图视角）不再恢复。
constexpr int kViewStateVersion = 2;

QJsonObject WorldModule::SaveViewState() const {
    if (!viewport_ || shownMap_.isEmpty()) return {};
    auto state = retainedViewState_; // 保留旧 panels 和本模块未知键，不覆盖其他模块设置。
    state["version"] = kViewStateVersion; state["map"] = shownMap_; state["camera"] = viewport_->SaveCamera();
    if (worldDockHost_) state["docks"] = QString::fromLatin1(worldDockHost_->saveState(1).toBase64());
    if (assetBrowser_) state["assetCompact"] = assetBrowser_->Compact();
    return state;
}

void WorldModule::RestoreViewState(const QJsonObject& state) {
    if (state["version"].toInt() != kViewStateVersion) return;
    retainedViewState_ = state;
    bool restored = false;
    if (worldDockHost_ && state["docks"].isString() && state["docks"].toString().size() <= 65536)
        restored = worldDockHost_->restoreState(QByteArray::fromBase64(state["docks"].toString().toLatin1()), 1);
    // 只读迁移旧三列宽度；原 panels 键原样保留，新的拖拽位置写入独立 docks 键。
    if (const auto panels = state["panels"].toArray(); !restored && worldDockHost_ && panels.size() == 3
        && panels[0].toInt() > 0 && panels[2].toInt() > 0)
        worldDockHost_->resizeDocks({toolDock_, inspectorDock_}, {panels[0].toInt(), panels[2].toInt()}, Qt::Horizontal);
    if (assetBrowser_ && state["assetCompact"].isBool()) assetBrowser_->SetCompact(state["assetCompact"].toBool());
    const auto camera = state["camera"].toObject();
    const auto map = state["map"].toString();
    if (camera.isEmpty() || map.isEmpty()) return;
    if (viewport_ && viewport_->VertexCount() != 0 && !shownMap_.isEmpty()) {
        if (map == shownMap_) (void)viewport_->RestoreCamera(camera);
        return;
    }
    pendingCamera_ = camera; pendingCameraMap_ = map;
}

} // namespace War3Map::Modules

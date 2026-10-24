// 世界视图的界面框架：顶栏工具分段、左侧工具面板（场景目录 / 摆放；地形笔刷页见 WorldTerrainPanel.cpp）、状态栏，
// 以及它们与视口、编辑交互的连接。只做呈现与输入转发；地图修改仍经 WorldModuleEditing.cpp 提交。
#include "WorldModule.h"
#include "WorldPanelWidgets.h"
#include "WorldViewport.h"
#include "WorldAnimationPlayer.h"
#include "WorldOutlinerModel.h"
#include "WorldAssetBrowser.h"
#include "Modules/ModuleHost.h"
#include "EditorProject.h"
#include "ObjectViewEntries.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"
#include "Widgets/ResponsiveToolBar.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/TerrainPicking.h"
#include "Widgets/SearchField.h"

#include <QButtonGroup>
#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTreeView>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace War3Map::Modules {
namespace {

using namespace WorldUi;

// Qt 默认 Ctrl 是切换选择。这里只规整输入，并在原生输入完成后转交共用选择服务，
// 避免服务回显在 selectionChanged 栈内重置 Qt 当前项与范围选择锚点。
class OutlinerInputView final : public QTreeView {
public:
    using InputCommit = std::function<void(const QItemSelection&, const QItemSelection&,
        Qt::KeyboardModifiers, const QPersistentModelIndex&)>;
    explicit OutlinerInputView(QWidget* parent) : QTreeView(parent) {}
    InputCommit commitInput;
    bool CollectChange(const QItemSelection& selected, const QItemSelection& deselected) {
        if (!inputActive_) return false;
        changed_ = true;
        added_.merge(selected, QItemSelectionModel::Select);
        added_.merge(deselected, QItemSelectionModel::Deselect);
        removed_.merge(deselected, QItemSelectionModel::Select);
        removed_.merge(selected, QItemSelectionModel::Deselect);
        return true;
    }
protected:
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& index, const QEvent* event) const override {
        auto flags = QTreeView::selectionCommand(index, event);
        if (!inputActive_ || flags == QItemSelectionModel::NoUpdate) return flags;
        if (modifiers_.testFlag(Qt::ControlModifier))
            return QItemSelectionModel::Deselect | (flags & QItemSelectionModel::Rows);
        if (modifiers_.testFlag(Qt::ShiftModifier)) {
            flags &= ~(QItemSelectionModel::Clear | QItemSelectionModel::Toggle | QItemSelectionModel::Deselect);
            flags |= QItemSelectionModel::Select;
        }
        return flags;
    }
    void mousePressEvent(QMouseEvent* event) override {
        plainPress_ = event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier
            ? QPersistentModelIndex(indexAt(event->position().toPoint())) : QPersistentModelIndex{};
        Input(event->modifiers(), [this, event] { QTreeView::mousePressEvent(event); });
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        Input(event->modifiers(), [this, event] { QTreeView::mouseMoveEvent(event); });
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        const auto index = indexAt(event->position().toPoint());
        const bool plainClick = !doubleClickRelease_ && event->button() == Qt::LeftButton
            && event->modifiers() == Qt::NoModifier && index.isValid() && index == plainPress_
            && index.column() == 0 && model()->flags(index).testFlag(Qt::ItemIsSelectable);
        Input(event->modifiers(), [this, event] { QTreeView::mouseReleaseEvent(event); }, plainClick);
        plainPress_ = QPersistentModelIndex{}; doubleClickRelease_ = false;
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        doubleClickRelease_ = true;
        Input(event->modifiers(), [this, event] { QTreeView::mouseDoubleClickEvent(event); });
    }
    void keyPressEvent(QKeyEvent* event) override {
        Input(event->modifiers(), [this, event] { QTreeView::keyPressEvent(event); });
    }
private:
    template<class Action> void Input(Qt::KeyboardModifiers modifiers, Action action, bool forceCommit = false) {
        if (inputActive_) { action(); return; }
        inputActive_ = true; modifiers_ = modifiers; changed_ = false;
        added_.clear(); removed_.clear();
        action();
        inputActive_ = false;
        if ((changed_ || forceCommit) && commitInput)
            commitInput(added_, removed_, modifiers_, QPersistentModelIndex(currentIndex()));
    }
    bool inputActive_ = false, changed_ = false;
    bool doubleClickRelease_ = false;
    QPersistentModelIndex plainPress_;
    Qt::KeyboardModifiers modifiers_;
    QItemSelection added_, removed_;
};

QPushButton* ToolButton(QWidget* parent, const char* name, const char* icon, const QString& text, const QString& tip) {
    auto* button = new QPushButton(UiKit::SvgIcon(QLatin1String(icon)), text, parent);
    button->setObjectName(QLatin1String(name));
    button->setProperty("worldTool", true);
    button->setCheckable(true);
    button->setIconSize(QSize(15, 15));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    return button;
}

QPushButton* ToggleButton(QWidget* parent, const char* name, const QString& text, const QString& tip) {
    auto* button = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/eye")), text, parent);
    button->setObjectName(QLatin1String(name));
    button->setProperty("worldToggle", true);
    button->setCheckable(true);
    button->setChecked(true);
    button->setIconSize(QSize(14, 14));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    QObject::connect(button, &QPushButton::toggled, button, [button](bool on) {
        button->setIcon(UiKit::SvgIcon(on ? QStringLiteral("lucide/eye") : QStringLiteral("lucide/eye-off")));
    });
    return button;
}

QString Integer(double value) { return QString::number(static_cast<long long>(std::llround(value))); }

} // namespace

// ── 顶栏 ──

void WorldModule::BuildToolbar(QVBoxLayout* layout) {
    auto* toolbar = new UiKit::ResponsiveToolBar(page_);
    toolbar->setObjectName(QStringLiteral("worldToolbar"));
    toolbar->installEventFilter(this);
    auto* row = toolbar->ContentLayout();
    row->setContentsMargins(10, 7, 10, 7); row->setSpacing(8);
    auto* segment = new QFrame(toolbar);
    segment->setObjectName(QStringLiteral("worldToolSegment"));
    segment->setProperty("toolbarPriority", 100);
    auto* tools = new QHBoxLayout(segment);
    tools->setContentsMargins(3, 3, 3, 3); tools->setSpacing(2);
    toolIndicator_ = new UiKit::SlidingHighlight(segment);
    toolIndicator_->setObjectName(QStringLiteral("worldToolIndicator"));
    selectTool_ = ToolButton(segment, "worldSelectTool", "lucide/mouse-pointer-2", tr("选择"),
        tr("选择、移动单位与装饰物，拖动区域和镜头"));
    placeToggle_ = ToolButton(segment, "worldPlaceToggle", "lucide/box", tr("摆放"),
        tr("从物体目录选择后单击地面放置；再次点击回到选择"));
    terrainToggle_ = ToolButton(segment, "worldTerrainToggle", "lucide/mountain", tr("地形"),
        tr("地形笔刷：贴图、荒芜、升降、高原、平滑、悬崖；再次点击回到选择"));
    for (auto* button : {selectTool_, placeToggle_, terrainToggle_}) tools->addWidget(button);
    row->addWidget(segment);
    auto* divider = new QFrame(toolbar);
    divider->setObjectName(QStringLiteral("worldToolbarDivider"));
    divider->setFixedSize(1, 18);
    row->addWidget(divider);
    showRegions_ = ToggleButton(toolbar, "worldShowRegions", tr("区域"), tr("在视口中显示区域轮廓"));
    showCameras_ = ToggleButton(toolbar, "worldShowCameras", tr("镜头"), tr("在视口中显示镜头位置与视锥"));
    row->addWidget(showRegions_); row->addWidget(showCameras_);
    showPathingHelpers_ = ToggleButton(toolbar, "worldShowPathingHelpers", tr("路径阻断器"),
        tr("显示物编明确标记的路径阻断器辅助方块；未知占地用小标记表示，不代表原版占地"));
    row->addWidget(showPathingHelpers_);
    connect(showPathingHelpers_, &QPushButton::toggled, this, [this] {
        if (viewport_) (void)viewport_->RefreshEditorVisibility();
        UpdateOverlay(); UpdateInspector();
    });
    rotationStep_ = new QComboBox(toolbar);
    rotationStep_->setObjectName(QStringLiteral("worldRotationStep"));
    rotationStep_->setToolTip(tr("Alt 拖动选中组旋转时的角度步长"));
    rotationStep_->addItem(tr("自由角度"), 0.0);
    for (const auto degrees : {15, 45, 90}) rotationStep_->addItem(tr("%1°").arg(degrees), degrees * 3.141592653589793 / 180.0);
    row->addWidget(rotationStep_);
    animationPlay_ = new QPushButton(tr("播放"), toolbar);
    animationPause_ = new QPushButton(tr("暂停"), toolbar);
    animationStep_ = new QPushButton(tr("单步"), toolbar);
    animationPlay_->setObjectName(QStringLiteral("worldAnimationPlay"));
    animationPause_->setObjectName(QStringLiteral("worldAnimationPause"));
    animationStep_->setObjectName(QStringLiteral("worldAnimationStep"));
    animationPlay_->setIcon(toolbar->style()->standardIcon(QStyle::SP_MediaPlay));
    animationPause_->setIcon(toolbar->style()->standardIcon(QStyle::SP_MediaPause));
    animationStep_->setIcon(toolbar->style()->standardIcon(QStyle::SP_MediaSkipForward));
    for (auto* button : {animationPlay_, animationPause_, animationStep_}) {
        button->setProperty("worldToggle", true); button->setEnabled(false); row->addWidget(button);
    }
    row->addStretch(1);
    auto* reset = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/rotate-ccw")), tr("重置视角"), toolbar);
    reset->setObjectName(QStringLiteral("worldResetCamera"));
    reset->setProperty("worldToggle", true);
    reset->setIconSize(QSize(14, 14));
    reset->setCursor(Qt::PointingHandCursor);
    reset->setToolTip(tr("回到游戏默认视角"));
    connect(reset, &QPushButton::clicked, this, [this] {
        if (cameraFlight_) cameraFlight_->stop();
        viewport_->ResetCamera();
    });
    row->addWidget(reset);
    for (const auto side : {0, 2}) {
        auto* collapse = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/panel-right")),
            side == 0 ? tr("大纲") : tr("属性"), toolbar);
        collapse->setObjectName(side == 0 ? QStringLiteral("worldToggleOutliner") : QStringLiteral("worldToggleInspector"));
        collapse->setProperty("worldToggle", true);
        collapse->setCheckable(true); collapse->setChecked(true);
        connect(collapse, &QPushButton::toggled, this, [this, side](bool shown) {
            auto* dock = side == 0 ? toolDock_ : inspectorDock_;
            if (dock) { dock->setVisible(shown); if (shown) dock->raise(); }
        });
        row->addWidget(collapse);
    }
    for (auto* button : toolbar->findChildren<QPushButton*>()) {
        button->setProperty("worldFullLabel", button->text());
        if (button->toolTip().isEmpty()) button->setToolTip(button->text());
        button->setMinimumWidth(0);
    }
    layout->addWidget(toolbar);
}

// ── 左侧工具面板 ──

QWidget* WorldModule::BuildToolPanel() {
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("worldToolPanel"));
    panel->setMinimumWidth(220);
    auto* column = new QVBoxLayout(panel);
    column->setContentsMargins(0, 0, 0, 0); column->setSpacing(0);
    auto* header = new QFrame(panel);
    header->setObjectName(QStringLiteral("worldPanelHeader"));
    auto* titles = new QVBoxLayout(header);
    titles->setContentsMargins(14, 10, 14, 10); titles->setSpacing(1);
    toolTitle_ = new QLabel(tr("场景目录"), header);
    toolTitle_->setObjectName(QStringLiteral("worldPanelTitle"));
    toolSubtitle_ = new QLabel(header);
    toolSubtitle_->setObjectName(QStringLiteral("worldPanelSubtitle"));
    titles->addWidget(toolTitle_); titles->addWidget(toolSubtitle_);
    column->addWidget(header);
    toolStack_ = new QStackedWidget(panel);
    toolStack_->setObjectName(QStringLiteral("worldToolStack"));
    toolStack_->setProperty("worldPanel", true);
    toolStack_->addWidget(BuildSelectPage());
    toolStack_->addWidget(BuildPlacePage());
    toolStack_->addWidget(BuildTerrainPage());
    column->addWidget(toolStack_, 1);
    return panel;
}

QWidget* WorldModule::BuildSelectPage() {
    auto* page = Surface();
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(10, 10, 10, 10); column->setSpacing(8);
    outlinerFilter_ = new QLineEdit(page);
    outlinerFilter_->setObjectName(QStringLiteral("worldOutlinerFilter"));
    outlinerFilter_->setPlaceholderText(tr("搜索名称或原始 ID"));
    outlinerFilter_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(outlinerFilter_);
    column->addWidget(outlinerFilter_);
    outlinerCategory_ = new QComboBox(page);
    outlinerCategory_->setObjectName(QStringLiteral("worldOutlinerCategory"));
    outlinerCategory_->addItem(tr("全部类别"), -1);
    for (const auto category : {Services::WorldCategory::Unit, Services::WorldCategory::Item,
        Services::WorldCategory::Destructable, Services::WorldCategory::Doodad,
        Services::WorldCategory::Region, Services::WorldCategory::Camera,
        Services::WorldCategory::StartLocation, Services::WorldCategory::Unknown})
        outlinerCategory_->addItem(QString::fromUtf8(Services::WorldCategoryName(category)), static_cast<int>(category));
    column->addWidget(outlinerCategory_);
    auto* inputView = new OutlinerInputView(page);
    outliner_ = inputView;
    outliner_->setObjectName(QStringLiteral("worldOutliner"));
    outliner_->installEventFilter(this);
    outlinerModel_ = new WorldOutlinerModel(outliner_);
    outliner_->setModel(outlinerModel_);
    outliner_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    outliner_->setSelectionBehavior(QAbstractItemView::SelectRows);
    outliner_->setIndentation(14);
    outliner_->setUniformRowHeights(true);
    outliner_->setAnimated(UiKit::AnimationsEnabled());
    outliner_->setFocusPolicy(Qt::StrongFocus);
    outliner_->header()->setStretchLastSection(false);
    outliner_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    outliner_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    outliner_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    column->addWidget(outliner_, 1);
    auto* more = new QPushButton(tr("继续读取场景"), page);
    more->setObjectName(QStringLiteral("worldOutlinerMore"));
    column->addWidget(more);
    outlinerStatus_ = new QLabel(page);
    outlinerStatus_->setObjectName(QStringLiteral("worldOutlinerStatus"));
    outlinerStatus_->setWordWrap(true);
    column->addWidget(outlinerStatus_);
    auto* commands = new UiKit::ResponsiveToolBar(page);
    commands->setObjectName(QStringLiteral("worldAuxiliaryCommands"));
    auto* create = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/plus")), tr("新建"), commands);
    create->setObjectName(QStringLiteral("worldNewAuxiliary"));
    auto* menu = new QMenu(create); create->setMenu(menu);
    auto* newRegion = menu->addAction(tr("新建区域")); newRegion->setObjectName(QStringLiteral("worldNewRegion"));
    auto* newCamera = menu->addAction(tr("新建镜头")); newCamera->setObjectName(QStringLiteral("worldNewCamera"));
    connect(newRegion, &QAction::triggered, this, [this] { CreateAuxiliary(MapDataKind::Regions); });
    connect(newCamera, &QAction::triggered, this, [this] { CreateAuxiliary(MapDataKind::Cameras); });
    commands->AddWidget(create, 100);
    auto* properties = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/settings-2")), tr("属性"), commands);
    properties->setObjectName(QStringLiteral("worldAuxiliaryProperties")); commands->AddWidget(properties, 80);
    connect(properties, &QPushButton::clicked, this, &WorldModule::EditSelectedAuxiliary);
    auto* duplicate = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/copy")), tr("副本"), commands);
    duplicate->setObjectName(QStringLiteral("worldDuplicateAuxiliary"));
    duplicate->setProperty("worldToggle", true);
    duplicate->setToolTip(tr("当前所选对象一次复制；未支持的混合类别保持只读"));
    commands->AddWidget(duplicate, 50);
    auto* remove = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/trash-2")), tr("删除"), commands);
    remove->setObjectName(QStringLiteral("worldDeleteAuxiliary")); commands->AddWidget(remove, 30);
    connect(remove, &QPushButton::clicked, this, &WorldModule::DeleteSelectedAuxiliary);
    column->addWidget(commands);
    connect(outlinerFilter_, &QLineEdit::textChanged, this, &WorldModule::FilterOutliner);
    connect(outlinerCategory_, &QComboBox::currentIndexChanged, this, [this] { FilterOutliner(outlinerFilter_->text()); });
    connect(outliner_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto* row = index.column() == 0 ? outlinerModel_->RowAt(index) : nullptr;
        if (row && row->id && row->positionWorld && worldSelection_.Version()
            && outlinerModel_->Version() == worldSelection_.Version())
            FlyTo((*row->positionWorld)[0], (*row->positionWorld)[1], std::nullopt);
    });
    inputView->commitInput = [this](const QItemSelection& added, const QItemSelection& removed,
        Qt::KeyboardModifiers modifiers, const QPersistentModelIndex& current) {
        if (!worldSelection_.Version() || outlinerModel_->Version() != worldSelection_.Version()) return;
        const auto mode = modifiers.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
            : modifiers.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace;
        std::vector<Services::WorldObjectId> ids;
        const auto indexes = mode == Services::SelectionMode::Replace ? outliner_->selectionModel()->selectedRows()
            : (mode == Services::SelectionMode::Add ? added : removed).indexes();
        for (const auto& index : indexes)
            if (index.column() == 0)
                if (const auto* row = outlinerModel_->RowAt(index); row && row->id) ids.push_back(*row->id);
        (void)SelectObjects(ids, mode);
        // 服务保有 primary；Qt 导航当前项独立，不被已选范围的最后一个身份覆盖。
        if (current.isValid() && current.model() == outlinerModel_) {
            const QSignalBlocker blocker(outliner_->selectionModel());
            outliner_->selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
        }
    };
    connect(outliner_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        [this, inputView](const QItemSelection& selected, const QItemSelection& deselected) {
            if (!inputView->CollectChange(selected, deselected)) SyncOutlinerSelection();
    });
    // QTreeView 也会自动分页；核心选择可以先于对应目录行存在。
    // 每页结束后再读取当前同文档快照，不在 begin/endInsertRows 调用栈里同步。
    connect(outlinerModel_, &WorldOutlinerModel::PageLoaded, this, [this] {
        outliner_->expandAll();
        outlinerStatus_->setText(outlinerModel_->StatusText());
        SyncOutlinerSelection();
    }, Qt::QueuedConnection);
    connect(more, &QPushButton::clicked, this, [this] {
        if (outlinerModel_->canFetchMore({})) outlinerModel_->fetchMore({});
        outliner_->expandAll();
        outlinerStatus_->setText(outlinerModel_->StatusText());
        SyncOutlinerSelection();
    });
    connect(duplicate, &QPushButton::clicked, this, &WorldModule::DuplicateSelection);
    return page;
}

QWidget* WorldModule::BuildPlacePage() {
    palette_ = Surface();
    palette_->setObjectName(QStringLiteral("worldPlacePalette"));
    auto* column = new QVBoxLayout(palette_);
    column->setContentsMargins(10, 10, 10, 10); column->setSpacing(8);
    auto* ownerRow = new QHBoxLayout;
    ownerRow->setSpacing(8);
    auto* ownerLabel = new QLabel(tr("所属玩家"), palette_);
    ownerLabel->setObjectName(QStringLiteral("worldFieldLabel"));
    ownerRow->addWidget(ownerLabel);
    owner_ = new QComboBox(palette_);
    owner_->setObjectName(QStringLiteral("worldPlaceOwner"));
    for (int player = 0; player < 12; ++player) owner_->addItem(tr("玩家 %1").arg(player + 1), player);
    owner_->addItem(tr("中立敌对"), 12); owner_->addItem(tr("中立受害"), 13);
    owner_->addItem(tr("中立特殊"), 14); owner_->addItem(tr("中立被动"), 15);
    ownerRow->addWidget(owner_, 1);
    column->addLayout(ownerRow);
    column->addStretch(1);
    return palette_;
}

// ── 状态栏 ──

void WorldModule::BuildStatusBar(QVBoxLayout* layout) {
    auto* bar = new QFrame(page_);
    bar->setObjectName(QStringLiteral("worldStatusBar"));
    bar->setFixedHeight(28);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 0, 12, 0); row->setSpacing(10);
    rendererDot_ = new QLabel(bar);
    rendererDot_->setObjectName(QStringLiteral("worldRendererDot"));
    rendererDot_->setFixedSize(7, 7);
    rendererStatus_ = new QLabel(viewport_->StatusText(), bar);
    rendererStatus_->setObjectName(QStringLiteral("worldRendererStatus"));
    terrainStatus_ = new UiKit::ElidedLabel(tr("打开地图以查看地形"), bar);
    terrainStatus_->setObjectName(QStringLiteral("worldTerrainStatus"));
    terrainPathingWarning_ = new QLabel(tr("通行性待核对"), bar);
    terrainPathingWarning_->setObjectName(QStringLiteral("worldTerrainPathingWarning"));
    terrainPathingWarning_->setToolTip(tr("保存时会核对地面贴图、水深和悬崖对通行的影响，必要时按实际资源重建；无法完整核对时会提示原因，修改仍可保存在工程中。"));
    terrainPathingWarning_->hide();
    selectionStatus_ = new UiKit::ElidedLabel(bar);
    selectionStatus_->setObjectName(QStringLiteral("worldToolHint"));
    selectionStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hoverStatus_ = new QLabel(QStringLiteral("—"), bar);
    hoverStatus_->setObjectName(QStringLiteral("worldHoverCoordinates"));
    hoverStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hoverStatus_->setFont(UiKit::MonoFont());
    hoverStatus_->setMinimumWidth(hoverStatus_->fontMetrics().horizontalAdvance(QStringLiteral("X -00000  Y -00000  Z -0000  ·  000,000")) + 24);
    row->addWidget(rendererDot_);
    row->addWidget(rendererStatus_);
    row->addWidget(terrainStatus_, 2);
    row->addWidget(terrainPathingWarning_);
    effectBudgetStatus_ = new QPushButton(bar);
    effectBudgetStatus_->setObjectName(QStringLiteral("worldEffectBudgetStatus"));
    effectBudgetStatus_->setFlat(true);
    effectBudgetStatus_->setCursor(Qt::PointingHandCursor);
    effectBudgetStatus_->hide();
    row->addWidget(effectBudgetStatus_);
    connect(effectBudgetStatus_, &QPushButton::clicked, this, [this] {
        if (animation_) animation_->SetTemporaryFullEffects(!animation_->TemporaryFullEffects());
        RefreshEffectBudgetStatus();
    });
    row->addWidget(selectionStatus_, 3);
    row->addWidget(hoverStatus_);
    layout->addWidget(bar);
}

// ── 连接 ──

void WorldModule::ConnectEditing() {
    connect(showRegions_, &QPushButton::toggled, this, &WorldModule::UpdateOverlay);
    connect(showCameras_, &QPushButton::toggled, this, &WorldModule::UpdateOverlay);
    connect(selectTool_, &QPushButton::clicked, this, [this] {
        StopPlacing();
        terrainToggle_->setChecked(false);
        // 松手核验期间按钮已暂时取消勾选，不能只依赖toggled(false)取消事务。
        CancelTerrainStroke();
        resumeBrushAfterRender_ = false;
        UpdateToolState();
    });
    connect(placeToggle_, &QPushButton::toggled, this, [this](bool shown) {
        if (shown) {
            if (terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
            CancelTerrainStroke();
            resumeBrushAfterRender_ = false;
        } else StopPlacing();
        UpdateToolState(); // 先切到摆放页：目录只在可见时填充
        if (shown) { RefreshAssetBrowser(); if (assetDock_) { assetDock_->show(); assetDock_->raise(); } assetBrowser_->setFocus(); }
    });
    connect(terrainToggle_, &QPushButton::toggled, this, [this](bool shown) {
        if (shown && placeToggle_->isChecked()) placeToggle_->setChecked(false);
        viewport_->SetBrushMode(shown && (!NeedsStructuralBrushRules() || WaterBrushCaptureCurrent()));
        if (shown) { StopPlacing(); ClearSelection(); RefreshTerrainPalette(); UpdateOverlay(); }
        else CancelTerrainStroke();
        UpdateToolState();
        UpdateSelectionText();
        UpdateWaterFrameControl();
        if (shown && NeedsStructuralBrushRules()) QueueWaterBrushCapture();
    });
    connect(terrainKinds_, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (!on) return;
        CancelTerrainStroke();
        UpdateTerrainBrushControls();
        UpdateToolHint();
        if (NeedsStructuralBrushRules()) QueueWaterBrushCapture();
        viewport_->SetBrushMode(terrainToggle_->isChecked() && (!NeedsStructuralBrushRules() || WaterBrushCaptureCurrent()));
    });
    connect(assetBrowser_, &WorldAssetBrowser::PlacementRequested, this, [this](const QtApp::ObjectViewEntry& entry, const WorldAssetBinding& binding) {
        if (binding == WorldAssetBinding{MapKey(), generation_, project_.mapEdits_ ? project_.mapEdits_->Revision() : 0}) ChoosePlacement(entry);
    });
    connect(assetBrowser_, &WorldAssetBrowser::DefinitionRequested, this, [this](ObjectData::ObjectType type, const QString& rawcode, const WorldAssetBinding& binding) {
        if (host_ && binding == WorldAssetBinding{MapKey(), generation_, project_.mapEdits_ ? project_.mapEdits_->Revision() : 0})
            host_->Navigate({"objects", "object", QString::fromUtf8(ObjectData::ToString(type)) + QLatin1Char(':') + rawcode});
    });
    connect(assetBrowser_, &WorldAssetBrowser::FileRequested, this, [this](const QString& path, const WorldAssetBinding& binding) {
        if (host_ && binding == WorldAssetBinding{MapKey(), generation_, project_.mapEdits_ ? project_.mapEdits_->Revision() : 0})
            host_->Navigate({"assets", "asset", path});
    });
    connect(assetBrowser_, &WorldAssetBrowser::CollapsedChanged, this, [this](bool collapsed) {
        if (worldDockHost_ && assetDock_ && worldDockHost_->dockWidgetArea(assetDock_) == Qt::BottomDockWidgetArea)
            worldDockHost_->resizeDocks({assetDock_}, {collapsed ? 72 : 240}, Qt::Vertical);
    });
    connect(viewport_, &WorldViewport::InstanceClicked, this, &WorldModule::SelectInstance);
    connect(viewport_, &WorldViewport::BugPlaceholderClicked, this,
        [this](const MapData::PlacementId& owner, int modifiers) {
            const std::array<Services::WorldObjectId, 1> ids{owner};
            const auto keys = Qt::KeyboardModifiers(modifiers);
            (void)SelectObjects(ids, keys.testFlag(Qt::ControlModifier) ? Services::SelectionMode::Subtract
                : keys.testFlag(Qt::ShiftModifier) ? Services::SelectionMode::Add : Services::SelectionMode::Replace);
        });
    connect(viewport_, &WorldViewport::InstancesBoxSelected, this, &WorldModule::SelectBox);
    connect(viewport_, &WorldViewport::GroundClicked, this, &WorldModule::SelectGround);
    connect(viewport_, &WorldViewport::PlaceRequested, this, &WorldModule::PlaceAt);
    connect(viewport_, &WorldViewport::PlacementHovered, this, &WorldModule::PreviewAt);
    connect(viewport_, &WorldViewport::PlacementHoverLost, this, &WorldModule::HidePreview);
    connect(viewport_, &WorldViewport::PlacingCancelled, this, &WorldModule::StopPlacing);
    connect(viewport_, &WorldViewport::MoveStarted, this, &WorldModule::StartDrag);
    connect(viewport_, &WorldViewport::MoveDragged, this, &WorldModule::DragSelection);
    connect(viewport_, &WorldViewport::MoveFinished, this, &WorldModule::FinishDrag);
    connect(viewport_, &WorldViewport::MoveCancelled, this, &WorldModule::CancelDrag);
    connect(viewport_, &WorldViewport::RotateStarted, this, &WorldModule::StartRotation);
    connect(viewport_, &WorldViewport::RotateDragged, this, &WorldModule::RotateSelection);
    connect(viewport_, &WorldViewport::RotateFinished, this, &WorldModule::FinishDrag);
    connect(viewport_, &WorldViewport::RotateCancelled, this, &WorldModule::CancelDrag);
    connect(viewport_, &WorldViewport::DeleteRequested, this, &WorldModule::DeleteSelection);
    connect(viewport_, &WorldViewport::CopyRequested, this, &WorldModule::CopySelection);
    connect(viewport_, &WorldViewport::PasteRequested, this, &WorldModule::StartPaste);
    connect(viewport_, &WorldViewport::DuplicateRequested, this, &WorldModule::DuplicateSelection);
    connect(viewport_, &WorldViewport::SelectAllRequested, this, &WorldModule::SelectAllWorldObjects);
    connect(viewport_, &WorldViewport::NudgeRequested, this, &WorldModule::NudgeSelection);
    connect(viewport_, &WorldViewport::FocusSelectionRequested, this, [this] {
        if (selection_ && selectionDocument_) {
            const auto found = MapData::FindPlacement(*selectionDocument_, *selection_);
            if (found.Succeeded()) FlyTo(found.value->position[0], found.value->position[1], std::nullopt);
        } else if (const auto* region = SelectedRegion()) FlyTo((region->left + region->right) / 2, (region->bottom + region->top) / 2, std::nullopt);
        else if (const auto* camera = SelectedCamera()) FlyTo(camera->targetX, camera->targetY, std::nullopt);
    });
    connect(viewport_, &WorldViewport::AssetDropped, this, [this](const QByteArray& data, double x, double y, bool snap) {
        const auto parsed = WorldAssetBrowser::ParseDrag(data);
        if (!parsed.Succeeded() || parsed.value->binding != WorldAssetBinding{MapKey(), generation_, project_.mapEdits_ ? project_.mapEdits_->Revision() : 0}) return;
        QtApp::ObjectViewEntry entry;
        entry.kind = QString::fromUtf8(ObjectData::ToString(parsed.value->object.type));
        entry.id = QString::fromUtf8(parsed.value->object.rawcode);
        entry.name = entry.id;
        ChoosePlacement(entry); PlaceAt(x, y, snap);
    });
    viewport_->SetInstanceSelectable([this](std::uint32_t request) {
        if (request >= placementIds_.size()) return false;
        const auto flags = worldSelection_.Flags(Services::WorldObjectId{placementIds_[request]});
        return flags.visible && !flags.locked && (!showPathingHelpers_ || showPathingHelpers_->isChecked()
            || request >= pathingHelperRequests_.size() || !pathingHelperRequests_[request]);
    });
    viewport_->SetInstanceVisible([this](std::uint32_t request) {
        const bool helperHidden = showPathingHelpers_ && !showPathingHelpers_->isChecked()
            && request < pathingHelperRequests_.size() && pathingHelperRequests_[request];
        return !helperHidden && (request >= placementIds_.size() || worldSelection_.Flags(Services::WorldObjectId{placementIds_[request]}).visible);
    });
    connect(viewport_, &WorldViewport::TerrainStrokeStarted, this, [this](double x, double y) {
        brushCursor_ = std::array{x, y};
        StartTerrainStroke(x, y);
    });
    connect(viewport_, &WorldViewport::TerrainStrokeMoved, this, [this](double x, double y) {
        brushCursor_ = std::array{x, y};
        ExtendTerrainStroke(x, y);
        UpdateOverlay();
    });
    connect(viewport_, &WorldViewport::TerrainStrokeFinished, this, &WorldModule::FinishTerrainStroke);
    connect(viewport_, &WorldViewport::TerrainStrokeCancelled, this, &WorldModule::CancelTerrainStroke);
    connect(viewport_, &WorldViewport::TerrainBrushExitRequested, this, [this] {
        resumeBrushAfterRender_ = false;
        terrainToggle_->setChecked(false);
        UpdateToolState();
    });
    connect(viewport_, &WorldViewport::StatusChanged, this, [this](const QString& text) {
        rendererStatus_->setText(text);
        UpdateStatusIndicators();
    });
    connect(viewport_, &WorldViewport::CoordinatesChanged, this, [this](const QString& hover, const QString& selected) {
        groundPoint_->setText(selected);
        const auto& hit = viewport_->HoverCoordinate();
        hoverStatus_->setText(hit ? QStringLiteral("X %1  Y %2  Z %3  ·  %4,%5").arg(Integer(hit->position[0]),
            Integer(hit->position[1]), Integer(hit->position[2])).arg(hit->tileX).arg(hit->tileY) : QStringLiteral("—"));
        hoverStatus_->setToolTip(hover);
        if (CurrentTool() == Tool::Terrain && !strokeBase_) UpdateBrushCursor(hit
            ? std::optional(std::array<double, 2>{hit->position[0], hit->position[1]}) : std::nullopt);
    });
    viewport_->SetHandleTest([this](double x, double y) { return HandleAt(x, y); });
    viewport_->installEventFilter(this);
    // [ 与 ] 调整笔刷半径（原版编辑器同键位习惯）。
    for (const auto& [key, step] : {std::pair{Qt::Key_BracketLeft, -1}, std::pair{Qt::Key_BracketRight, 1}}) {
        auto* shortcut = new QShortcut(QKeySequence(key), page_);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, step] {
            if (CurrentTool() == Tool::Terrain) terrainRadius_->setValue(terrainRadius_->value() + step);
        });
    }
    UpdateTerrainBrushControls();
    UpdateToolState();
    UpdateStatusIndicators();
}

// ── 工具状态 ──

void WorldModule::ActivatePlacementTool() {
    resumeBrushAfterRender_ = false;
    if (terrainToggle_ && terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
    if (viewport_) {
        viewport_->SetBrushMode(false);
        viewport_->SetPlacing(true);
    }
    if (placeToggle_) {
        // 目录点击/拖入和Ctrl+V已经确定目标；只同步工具，不重载目录或抢回焦点。
        const QSignalBlocker blocker(placeToggle_);
        placeToggle_->setChecked(true);
    }
    UpdateToolState();
}

WorldModule::Tool WorldModule::CurrentTool() const {
    if (placeToggle_ && placeToggle_->isChecked()) return Tool::Place;
    if (terrainToggle_ && (terrainToggle_->isChecked() || resumeBrushAfterRender_)) return Tool::Terrain;
    return Tool::Select;
}

void WorldModule::UpdateToolState() {
    if (!selectTool_) return;
    const auto tool = CurrentTool();
    {
        const QSignalBlocker blocker(selectTool_);
        selectTool_->setChecked(tool == Tool::Select);
    }
    const std::array buttons{std::pair{selectTool_, Tool::Select}, std::pair{placeToggle_, Tool::Place},
        std::pair{terrainToggle_, Tool::Terrain}};
    QPushButton* current = selectTool_;
    for (const auto& [button, which] : buttons) {
        const bool on = which == tool;
        if (on) current = button;
        if (button->property("current").toBool() != on) { button->setProperty("current", on); Repolish(button); }
    }
    toolIndicator_->Track(current);
    const int page = static_cast<int>(tool);
    if (toolStack_->currentIndex() != page) {
        toolStack_->setCurrentIndex(page);
        UiKit::FadeIn(toolStack_->currentWidget(), 170, 0.15);
    }
    toolTitle_->setText(tool == Tool::Terrain ? tr("地形笔刷") : tool == Tool::Place ? tr("摆放") : tr("场景目录"));
    if (tool != Tool::Terrain && brushCursor_) { brushCursor_.reset(); UpdateOverlay(); }
    shownTool_ = tool;
    UpdateToolHint();
}

void WorldModule::UpdateToolHint() {
    if (!selectionStatus_ || !toolSubtitle_) return;
    if (pasting_) {
        toolSubtitle_->setText(tr("整组粘贴 · %1 个摆放").arg(clipboard_.size()));
        selectionStatus_->clear();
        return;
    }
    const auto tool = CurrentTool();
    if (tool == Tool::Terrain) {
        const auto* kind = terrainKinds_->checkedButton();
        const bool ramp = CurrentBrushKind() == MapData::TerrainBrushKind::Ramp;
        const bool corner = CurrentBrushKind() == MapData::TerrainBrushKind::CornerRamp;
        const bool straight = CurrentBrushKind() == MapData::TerrainBrushKind::StraightRamp;
        toolSubtitle_->setText(straight ? tr("一级含水直坡 · 单点三顶点范围")
            : corner ? tr("一级拐角坡道 · 单点九顶点范围") : ramp ? tr("一级干地直坡 · 三点范围")
            : tr("%1 · 尺寸 %2").arg(kind ? kind->text() : QString{}).arg(terrainRadius_->value()));
        selectionStatus_->setText(strokeBase_ ? tr("涂刷中") : QString{});
        return;
    }
    if (tool == Tool::Place) {
        toolSubtitle_->setText(placing_ ? placingName_ : tr("选择要摆放的物体"));
        selectionStatus_->clear();
        return;
    }
    toolSubtitle_->setText(tr("区域 %1 · 镜头 %2").arg(regions_.size()).arg(cameras_.size()));
    if (selectionMapData_ && worldSelection_.Version()) {
        const auto snapshot = worldSelection_.Snapshot(SelectionContext(), *worldSelection_.Version());
        if (snapshot.Succeeded() && snapshot.value->selected.size() > 1 &&
            std::any_of(snapshot.value->selected.begin(), snapshot.value->selected.end(), [](const auto& id) {
                return !std::holds_alternative<MapData::PlacementId>(id);
            })) {
            selectionStatus_->setText(tr("已选 %1 个对象").arg(snapshot.value->selected.size()));
            return;
        }
    }
    if (dragPosition_ || dragRotation_ || regionDraft_ || cameraDraft_)
        selectionStatus_->setText(tr("拖动中"));
    else if (SelectedRequest()) selectionStatus_->setText(tr("已选 %1 个").arg(selectedPlacements_.size()));
    else selectionStatus_->clear();
}

void WorldModule::UpdateStatusIndicators() {
    if (terrainPathingWarning_) terrainPathingWarning_->setVisible(terrainPathingPending_);
    if (!rendererDot_ || !viewport_) return;
    const auto state = viewport_->RendererFailed() ? QStringLiteral("error")
        : viewport_->RendererReady() ? QStringLiteral("ready") : QStringLiteral("busy");
    if (rendererDot_->property("state").toString() != state) { rendererDot_->setProperty("state", state); Repolish(rendererDot_); }
}

void WorldModule::ApplyEffectQualityFromSettings() {
    if (!animation_) return;
    const auto name = QSettings{}.value(QStringLiteral("world/effectQuality"), QStringLiteral("balanced")).toString();
    animation_->SetEffectQuality(Render::AnimationPreviewQualityFromName(name.toStdString()).value_or(Render::AnimationPreviewQuality::Balanced));
    RefreshEffectBudgetStatus();
}

void WorldModule::RefreshEffectBudgetStatus() {
    if (!effectBudgetStatus_) return;
    if (!animation_ || !animation_->Ready()) { effectBudgetStatus_->hide(); return; }
    const auto status = animation_->EffectBudgetStatus();
    const bool temporary = animation_->TemporaryFullEffects();
    const auto omitted = status.budgetOmittedEmitters;
    effectBudgetStatus_->setVisible(temporary || omitted != 0);
    effectBudgetStatus_->setText(temporary ? tr("临时显示全部 · 恢复预算")
        : tr("有 %1 个特效因预算未显示").arg(omitted));
    effectBudgetStatus_->setToolTip(temporary
        ? tr("点击恢复所选特效设置。资源、内存限制和未支持功能仍会在模型诊断中说明。")
        : tr("点击临时显示全部支持的特效。切换地图后恢复所选设置；资源和内存限制仍然有效。"));
}


// 视角平滑移到目标点（保持朝向与俯仰）；用户按下鼠标或滚轮时立即停止。
void WorldModule::FlyTo(double x, double y, std::optional<double> distance) {
    if (!viewport_ || viewport_->VertexCount() == 0) return;
    if (cameraFlight_) cameraFlight_->stop();
    const auto from = viewport_->SaveCamera();
    auto to = from;
    to["x"] = x; to["y"] = y;
    if (terrain_) if (const auto ground = Render::SampleTerrainHeight(*terrain_, x, y)) to["z"] = static_cast<double>(*ground);
    if (distance) to["distance"] = *distance;
    if (!UiKit::AnimationsEnabled()) { (void)viewport_->RestoreCamera(to); return; }
    if (!cameraFlight_) {
        cameraFlight_ = new QVariantAnimation(this);
        cameraFlight_->setDuration(420);
        cameraFlight_->setEasingCurve(QEasingCurve::InOutCubic);
        cameraFlight_->setStartValue(0.0); cameraFlight_->setEndValue(1.0);
    }
    cameraFlight_->disconnect(this);
    connect(cameraFlight_, &QVariantAnimation::valueChanged, this, [this, from, to](const QVariant& value) {
        const double t = value.toDouble();
        auto frame = from;
        for (const auto* key : {"x", "y", "z", "distance"}) {
            const auto name = QLatin1String(key);
            frame[name] = from[name].toDouble() + (to[name].toDouble() - from[name].toDouble()) * t;
        }
        (void)viewport_->RestoreCamera(frame);
    });
    cameraFlight_->start();
}

} // namespace War3Map::Modules

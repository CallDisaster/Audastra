#include "GameUiCanvas.h"
#include "GameUiModule.h"
#include "GameUiNodeTree.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "Widgets/ProblemList.h"
#include "Widgets/ResponsiveToolBar.h"
#include "Widgets/SearchField.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QAction>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <utility>

namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;
using namespace War3Map::Modules::GameUiNodeTree;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString Number(float value) { return QString::number(static_cast<double>(value), 'f', 4); }

QString RaceLabel(const QString& race) {
    if (race.compare(QLatin1String("Human"), Qt::CaseInsensitive) == 0) return QObject::tr("人族");
    if (race.compare(QLatin1String("Orc"), Qt::CaseInsensitive) == 0) return QObject::tr("兽族");
    if (race.compare(QLatin1String("NightElf"), Qt::CaseInsensitive) == 0) return QObject::tr("暗夜精灵");
    if (race.compare(QLatin1String("Undead"), Qt::CaseInsensitive) == 0) return QObject::tr("不死族");
    return race;
}

QString SkinSourceText(const std::string& source) {
    if (source == "literal") return QObject::tr("直接路径");
    if (source == "script") return QObject::tr("地图脚本");
    if (source == "description") return QObject::tr("自己的界面");
    if (source == "map:customskin") return QObject::tr("地图皮肤 CustomSkin");
    if (source.rfind("game:", 0) == 0) return QObject::tr("游戏皮肤 [%1]").arg(Text(source.substr(5)));
    return Text(source);
}

QString TextSourceText(const std::string& source) {
    if (source == "map:framedef") return QObject::tr("地图 FrameDef");
    if (source == "fdf") return QObject::tr("界面文字表");
    if (source == "literal") return QObject::tr("原文");
    if (source == "script") return QObject::tr("地图脚本");
    if (source == "description") return QObject::tr("自己的界面");
    return Text(source);
}

QString UiTypeText(UiFrameType type) {
    switch (type) {
    case UiFrameType::Backdrop: return QObject::tr("背景");
    case UiFrameType::Text: return QObject::tr("文字");
    case UiFrameType::Bar: return QObject::tr("进度条");
    case UiFrameType::Button: return QObject::tr("按钮");
    case UiFrameType::Template: return QObject::tr("模板");
    case UiFrameType::Model: return QObject::tr("模型");
    default: return QObject::tr("容器");
    }
}

QString BindingText(const UiBinding& binding) {
    QString text = Text(binding.value);
    if (binding.perInstance) text += QStringLiteral("[]");
    else if (binding.index) text += QStringLiteral("[%1]").arg(*binding.index);
    if (binding.scale != 1.0) text += QStringLiteral(" ×%1").arg(binding.scale);
    if (binding.offset != 0.0) text += QStringLiteral(" %1%2").arg(binding.offset > 0.0 ? QStringLiteral("+") : QString()).arg(binding.offset);
    if (binding.invert) text += QObject::tr("（取反）");
    return text;
}

QString ColorText(const std::array<float, 4>& color) {
    const auto channel = [](float value) { return std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255); };
    return QStringLiteral("#%1%2%3%4")
        .arg(channel(color[3]), 2, 16, QLatin1Char('0'))
        .arg(channel(color[0]), 2, 16, QLatin1Char('0'))
        .arg(channel(color[1]), 2, 16, QLatin1Char('0'))
        .arg(channel(color[2]), 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString PropertyValues(const FdfProperty& property) {
    QStringList values;
    for (const auto& value : property.values) values.push_back(value.quoted ? QStringLiteral("\"%1\"").arg(Text(value.text)) : Text(value.text));
    return values.join(QStringLiteral(", "));
}

QString SeverityText(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error: return QObject::tr("错误");
    case DiagnosticSeverity::Warning: return QObject::tr("警告");
    default: return QObject::tr("信息");
    }
}

QPushButton* Toggle(QWidget* parent, const char* name, const QString& text, bool checked, const char* icon = nullptr) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(name));
    if (icon) button->setIcon(UiKit::SvgIcon(QLatin1String(icon)));
    button->setCheckable(true);
    button->setChecked(checked);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

void GameUiModule::BuildToolbar(QWidget* page, QVBoxLayout* layout) {
    auto* toolbar = new UiKit::ResponsiveToolBar(page);
    toolbar->setObjectName(QStringLiteral("gameUiToolbar"));
    auto* row = toolbar->ContentLayout();
    row->setContentsMargins(10, 7, 10, 7);
    row->setSpacing(6);

    raceBox_ = new QComboBox(toolbar);
    raceBox_->setObjectName(QStringLiteral("gameUiRace"));
    for (const QString race : {QStringLiteral("Human"), QStringLiteral("Orc"), QStringLiteral("NightElf"), QStringLiteral("Undead")})
        raceBox_->addItem(RaceLabel(race), race);
    raceBox_->setCurrentIndex(std::max(0, raceBox_->findData(race_)));
    versionBox_ = new QComboBox(toolbar);
    versionBox_->setObjectName(QStringLiteral("gameUiVersion"));
    versionBox_->addItems({tr("冰封王座"), tr("混乱之治")});
    versionBox_->setCurrentIndex(expansion_ ? 0 : 1);
    aspectBox_ = new QComboBox(toolbar);
    aspectBox_->setObjectName(QStringLiteral("gameUiAspect"));
    aspectBox_->addItem(QStringLiteral("16:9"), 16.0 / 9.0);
    aspectBox_->addItem(QStringLiteral("16:10"), 16.0 / 10.0);
    aspectBox_->addItem(QStringLiteral("4:3"), 4.0 / 3.0);
    showContent_ = Toggle(toolbar, "gameUiShowContent", tr("渲染"), true, "lucide/eye");
    showBounds_ = Toggle(toolbar, "gameUiShowBounds", tr("边框"), false, "lucide/square-dashed");
    showNames_ = Toggle(toolbar, "gameUiShowNames", tr("名称"), false, "lucide/info");
    showGrid_ = Toggle(toolbar, "gameUiShowGrid", tr("网格"), false, "lucide/layout-grid");
    auto* fit = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/maximize-2")), tr("适应窗口"), toolbar);
    fit->setObjectName(QStringLiteral("gameUiFitView"));
    fit->setToolTip(tr("适应窗口 (Ctrl+0)"));
    referenceButton_ = new QToolButton(toolbar);
    referenceButton_->setObjectName(QStringLiteral("gameUiReference"));
    referenceButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/image")));
    referenceButton_->setText(tr("参考图"));
    referenceButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    referenceButton_->setPopupMode(QToolButton::InstantPopup);
    referenceButton_->setCheckable(true);
    auto* referenceMenu = new QMenu(referenceButton_);
    referenceButton_->setMenu(referenceMenu);
    connect(referenceMenu, &QMenu::aboutToShow, this, [this, referenceMenu] {
        referenceMenu->clear();
        referenceMenu->addAction(tr("选择图片…"), this, [this] {
            const QString path = QFileDialog::getOpenFileName(page_, tr("参考图"), QFileInfo(referencePath_).absolutePath(),
                tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
            if (!path.isEmpty()) SetReferenceImage(path, referenceOpacity_);
        });
        auto* opacity = referenceMenu->addMenu(tr("透明度"));
        for (const int percent : {25, 50, 75, 100}) {
            QAction* action = opacity->addAction(QStringLiteral("%1%").arg(percent), this,
                [this, percent] { SetReferenceImage(referencePath_, percent / 100.0); });
            action->setCheckable(true);
            action->setChecked(std::abs(referenceOpacity_ * 100.0 - percent) < 1.0);
        }
        QAction* clear = referenceMenu->addAction(tr("不显示参考图"), this, [this] { SetReferenceImage({}, referenceOpacity_); });
        clear->setEnabled(!referencePath_.isEmpty());
    });
    showHiddenButton_ = Toggle(toolbar, "gameUiShowHidden", tr("显示隐藏"), showHidden_, "lucide/eye-off");
    originSelectableButton_ = Toggle(toolbar, "gameUiOriginSelectable", tr("原版可选"), originSelectable_, "lucide/mouse-pointer-2");
    addRootBox_ = new QComboBox(toolbar);
    addRootBox_->setObjectName(QStringLiteral("gameUiAddRoot"));
    addRootBox_->setEditable(true);
    addRootBox_->setInsertPolicy(QComboBox::NoInsert);
    addRootBox_->setMinimumContentsLength(18);
    addRootBox_->lineEdit()->setPlaceholderText(tr("添加界面…"));
    addRootBox_->completer()->setFilterMode(Qt::MatchContains);
    addRootBox_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    addRootBox_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    auto* reload = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/rotate-ccw")), tr("重新读取"), toolbar);
    reload->setObjectName(QStringLiteral("gameUiReload"));

    for (QWidget* widget : std::initializer_list<QWidget*>{raceBox_, versionBox_, aspectBox_, showContent_, showBounds_, showNames_, showGrid_,
             referenceButton_, fit, showHiddenButton_, originSelectableButton_})
        row->addWidget(widget);
    row->addStretch(1);
    for (QWidget* widget : BuildAuthoringControls(toolbar)) row->addWidget(widget);
    row->addWidget(addRootBox_);
    row->addWidget(reload);
    layout->addWidget(toolbar);

    connect(raceBox_, &QComboBox::currentIndexChanged, this, [this] { SetRace(raceBox_->currentData().toString()); });
    connect(versionBox_, &QComboBox::currentIndexChanged, this, [this](int index) { SetExpansion(index == 0); });
    connect(aspectBox_, &QComboBox::currentIndexChanged, this, [this] { canvas_->SetAspect(aspectBox_->currentData().toDouble()); });
    connect(showContent_, &QPushButton::toggled, this, [this](bool on) { canvas_->SetShowContent(on); });
    connect(showBounds_, &QPushButton::toggled, this, [this](bool on) { canvas_->SetShowBounds(on); });
    connect(showNames_, &QPushButton::toggled, this, [this](bool on) { canvas_->SetShowNames(on); });
    connect(showGrid_, &QPushButton::toggled, this, [this](bool on) { canvas_->SetShowGrid(on); });
    connect(fit, &QPushButton::clicked, this, [this] { canvas_->FitView(); });
    connect(showHiddenButton_, &QPushButton::toggled, this, [this](bool on) { SetShowHidden(on); });
    connect(originSelectableButton_, &QPushButton::toggled, this, [this](bool on) { SetOriginSelectable(on); });
    const auto add = [this] {
        if (AddRoot(addRootBox_->currentText())) addRootBox_->setEditText({});
    };
    connect(addRootBox_, &QComboBox::activated, this, add);
    connect(addRootBox_->lineEdit(), &QLineEdit::returnPressed, this, add);
    connect(reload, &QPushButton::clicked, this, &GameUiModule::Reload);
}

void GameUiModule::BuildPage() {
    page_ = new QWidget;
    page_->setObjectName(QStringLiteral("gameUiWorkspace"));
    auto* layout = new QVBoxLayout(page_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    BuildToolbar(page_, layout);

    mainSplit_ = new QSplitter(Qt::Horizontal, page_);
    mainSplit_->setObjectName(QStringLiteral("gameUiMainSplit"));
    mainSplit_->setChildrenCollapsible(false);

    auto* left = new QWidget(mainSplit_);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(8, 8, 4, 8);
    leftLayout->setSpacing(6);
    search_ = new QLineEdit(left);
    search_->setObjectName(QStringLiteral("gameUiSearch"));
    search_->setPlaceholderText(tr("搜索节点"));
    search_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(search_);
    nodes_ = new QTreeWidget(left);
    nodes_->setObjectName(QStringLiteral("gameUiNodes"));
    nodes_->setColumnCount(kColumnCount);
    nodes_->setHeaderLabels({tr("名称"), tr("类型"), tr("状态"), QString(), QString()});
    nodes_->headerItem()->setIcon(kVisibleColumn, UiKit::SvgIcon(QStringLiteral("lucide/eye")));
    nodes_->headerItem()->setToolTip(kVisibleColumn, tr("可见"));
    nodes_->headerItem()->setIcon(kSelectColumn, UiKit::SvgIcon(QStringLiteral("lucide/mouse-pointer-2")));
    nodes_->headerItem()->setToolTip(kSelectColumn, tr("可选"));
    nodes_->header()->setSectionResizeMode(QHeaderView::Interactive);
    nodes_->header()->setStretchLastSection(false);
    // 可见、可选两列排在最前，窄列不随拖动；树的展开标记留在名称列。
    for (const int column : {kVisibleColumn, kSelectColumn}) {
        nodes_->header()->setSectionResizeMode(column, QHeaderView::Fixed);
        nodes_->header()->resizeSection(column, 26);
    }
    nodes_->header()->moveSection(kVisibleColumn, 0);
    nodes_->header()->moveSection(kSelectColumn, 1);
    nodes_->setTreePosition(kNameColumn);
    nodes_->header()->resizeSection(kNameColumn, 190);
    nodes_->header()->resizeSection(kTypeColumn, 90);
    nodes_->viewport()->installEventFilter(this);
    nodes_->setUniformRowHeights(true);
    nodes_->setAlternatingRowColors(true);
    nodes_->setContextMenuPolicy(Qt::CustomContextMenu);
    leftLayout->addWidget(search_);
    leftLayout->addWidget(nodes_, 1);

    centerSplit_ = new QSplitter(Qt::Vertical, mainSplit_);
    centerSplit_->setObjectName(QStringLiteral("gameUiCenterSplit"));
    centerSplit_->setChildrenCollapsible(false);
    canvas_ = new GameUiCanvas(centerSplit_);
    canvas_->SetTextures(&textures_);
    canvas_->SetFontFamilies(&fontFamilies_);
    canvas_->SetAspect(aspectBox_->currentData().toDouble());
    canvas_->SetMeasure(pixelUnits_, resolution_);
    problemModel_ = new QStandardItemModel(0, 3, page_);
    problemModel_->setObjectName(QStringLiteral("gameUiProblemModel"));
    problemModel_->setHorizontalHeaderLabels({tr("级别"), tr("说明"), tr("代码")});
    problems_ = new UiKit::ProblemList(centerSplit_);
    problems_->setObjectName(QStringLiteral("gameUiProblems"));
    problems_->SetSourceModel(problemModel_, [](const QAbstractItemModel& model, int row) {
        return UiKit::ProblemList::SeverityFromText(model.index(row, 0).data().toString());
    });
    problems_->View()->header()->setStretchLastSection(false);
    problems_->View()->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    problems_->View()->header()->resizeSection(0, 64);
    problems_->View()->header()->resizeSection(2, 260);
    centerSplit_->setStretchFactor(0, 4);
    centerSplit_->setStretchFactor(1, 1);

    auto* right = new QWidget(mainSplit_);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(4, 8, 8, 8);
    rightLayout->setSpacing(6);
    rawFields_ = new QCheckBox(tr("显示原始字段"), right);
    rawFields_->setObjectName(QStringLiteral("showRawFields"));
    properties_ = new QTreeWidget(right);
    properties_->setObjectName(QStringLiteral("gameUiProperties"));
    properties_->setColumnCount(2);
    properties_->setHeaderLabels({tr("属性"), tr("值")});
    properties_->header()->setSectionResizeMode(QHeaderView::Interactive);
    properties_->header()->resizeSection(0, 128);
    properties_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
        QAbstractItemView::SelectedClicked);
    properties_->setRootIsDecorated(true);
    properties_->setAlternatingRowColors(true);
    properties_->setWordWrap(true);
    auto* propsViewportLayout = new QVBoxLayout(properties_->viewport());
    propsViewportLayout->setContentsMargins(16, 24, 16, 24);
    auto* propsEmpty = new QLabel(tr("未选择"), properties_->viewport());
    propsEmpty->setObjectName(QStringLiteral("emptyState"));
    propsEmpty->setAlignment(Qt::AlignCenter);
    propsEmpty->setWordWrap(true);
    propsEmpty->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    propsViewportLayout->addWidget(propsEmpty, 1, Qt::AlignCenter);
    const QPointer<QLabel> safeEmpty(propsEmpty);
    const QPointer<QTreeWidget> safeProps(properties_);
    const auto syncPropsEmpty = [safeEmpty, safeProps] {
        if (safeEmpty && safeProps) safeEmpty->setVisible(safeProps->topLevelItemCount() == 0);
    };
    connect(properties_->model(), &QAbstractItemModel::rowsInserted, propsEmpty, syncPropsEmpty);
    connect(properties_->model(), &QAbstractItemModel::rowsRemoved, propsEmpty, syncPropsEmpty);
    connect(properties_->model(), &QAbstractItemModel::modelReset, propsEmpty, syncPropsEmpty);
    syncPropsEmpty();
    rightLayout->addWidget(rawFields_, 0, Qt::AlignRight);
    auto* rightSplit = new QSplitter(Qt::Vertical, right);
    rightSplit->setObjectName(QStringLiteral("gameUiRightSplit"));
    rightSplit->setChildrenCollapsible(false);
    rightSplit->addWidget(properties_);
    BuildDescriptionPanel(rightSplit);
    rightSplit->addWidget(descriptionPanel_);
    BuildSimulationPanel(rightSplit);
    rightSplit->addWidget(simulationPanel_);
    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);
    rightSplit->setStretchFactor(2, 2);
    rightLayout->addWidget(rightSplit, 1);

    mainSplit_->setStretchFactor(0, 0);
    mainSplit_->setStretchFactor(1, 1);
    mainSplit_->setStretchFactor(2, 0);
    mainSplit_->setSizes({300, 900, 340});
    layout->addWidget(mainSplit_, 1);

    connect(search_, &QLineEdit::textChanged, this, &GameUiModule::ApplyFilter);
    connect(nodes_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (populating_ || !item) return;
        SelectNode(item->data(0, kNodeRole).toUInt());
    });
    connect(nodes_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        QTreeWidgetItem* item = nodes_->itemAt(position);
        ShowContextMenu(item ? item->data(0, kNodeRole).toUInt() : kNoNode, std::nullopt, nodes_->viewport()->mapToGlobal(position), true);
    });
    connect(canvas_, &GameUiCanvas::ContextMenuRequested, this, [this](std::uint32_t node, QPointF point, QPoint global) {
        ShowContextMenu(node, point, global, false);
    });
    connect(canvas_, &GameUiCanvas::NodeClicked, this, [this](std::uint32_t node) { SelectNode(node); });
    connect(canvas_, &GameUiCanvas::NodeDragged, this, [this](std::uint32_t node, double dx, double dy) {
        QString error;
        const bool ok = OriginNodeName(node).isEmpty() ? MoveDescriptionFrame(node, dx, dy, &error) : MoveOriginNode(node, dx, dy, &error);
        if (!ok) ReportAuthoring(error);
    });
    connect(canvas_, &GameUiCanvas::NodeResized, this, [this](std::uint32_t node, GameUi::FrameRect rect) {
        QString error;
        if (!ResizeDescriptionFrame(node, rect, &error)) ReportAuthoring(error);
    });
    connect(rawFields_, &QCheckBox::toggled, this, &GameUiModule::UpdateProperties);
    InstallPropertyEditors(properties_);
    // Delete 删除所选的描述框架（节点树与画布上都可用）。
    for (QWidget* target : std::initializer_list<QWidget*>{nodes_, canvas_}) {
        for (const auto& entry : {std::pair{QKeySequence::Copy, true}, std::pair{QKeySequence::Paste, false}}) {
            auto* action = new QAction(target);
            action->setShortcut(QKeySequence(entry.first));
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            target->addAction(action);
            connect(action, &QAction::triggered, this, [this, copy = entry.second] {
                QString error;
                const bool ok = copy ? CopySelectedDescriptionFrame(&error) : PasteDescriptionFrame(&error);
                if (!ok && !error.isEmpty()) ReportAuthoring(error);
            });
        }
        auto* remove = new QAction(target);
        remove->setShortcut(QKeySequence::Delete);
        remove->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        target->addAction(remove);
        connect(remove, &QAction::triggered, this, [this] {
            QString error;
            if (!RemoveSelectedDescriptionFrame(&error) && !error.isEmpty()) ReportAuthoring(error);
        });
    }

    if (!pendingView_.isEmpty()) {
        const QJsonObject state = std::exchange(pendingView_, {});
        RestoreViewState(state);
    }
}

void GameUiModule::SelectNode(std::uint32_t node) {
    pendingSelection_.clear();
    if (!tree_ || node >= tree_->Nodes().size() || node == kScreenNode) node = kNoNode;
    selected_ = node;
    selectedKey_ = node == kNoNode ? QString() : NodeKey(node);
    if (canvas_) canvas_->SetSelected(node);
    if (nodes_) {
        populating_ = true;
        QTreeWidgetItemIterator it(nodes_);
        QTreeWidgetItem* match = nullptr;
        for (; *it; ++it) {
            if ((*it)->data(0, kNodeRole).toUInt() == node) {
                match = *it;
                break;
            }
        }
        nodes_->setCurrentItem(match);
        if (match) nodes_->scrollToItem(match);
        populating_ = false;
    }
    // 选中自己界面里的节点时，那份界面成为当前界面。
    if (node != kNoNode && tree_->Nodes()[node].uiFrame) SetCurrentDescription(Text(tree_->Nodes()[node].uiFrame->description));
    UpdateProperties();
}

void GameUiModule::UpdateProperties() {
    UpdateDescriptionPanel();
    UpdateAuthoringControls();
    RefreshFrameSettings();
    if (!properties_) return;
    const QSignalBlocker block(properties_);  // 填表不算编辑
    // 同一框架改完字段重填时停在原来的滚动位置，不跳回顶部。
    const bool sameNode = propertiesKey_ == selectedKey_;
    propertiesKey_ = selectedKey_;
    const auto keepScroll = qScopeGuard([this, sameNode, scroll = properties_->verticalScrollBar()->value()] {
        if (!sameNode) return;
        properties_->doItemsLayout();
        properties_->verticalScrollBar()->setValue(scroll);
    });
    properties_->clear();
    if (!tree_ || selected_ == kNoNode || selected_ >= tree_->Nodes().size()) return;
    const FrameNode& node = tree_->Nodes()[selected_];
    AddOriginProperties();
    // 自己的控件：可改字段分组在前，只读的节点信息收进“详细”（默认折叠）。
    QTreeWidgetItem* detail = nullptr;
    const UiFrame* ownFrame = nullptr;
    if (node.uiFrame && !node.uiFrame->frame.empty()) {
        if (const UiDescription* description = Description(Text(node.uiFrame->description))) ownFrame = FindUiFrame(*description, node.uiFrame->frame);
    }
    const auto bold = [](QTreeWidgetItem* item) {
        item->setFirstColumnSpanned(true);
        QFont font = item->font(0);
        font.setWeight(QFont::DemiBold);
        item->setFont(0, font);
    };
    if (ownFrame) {
        AddEditableRows(properties_->invisibleRootItem(), *ownFrame);
        detail = new QTreeWidgetItem(properties_, {tr("详细")});
        bold(detail);
        MarkPropertyGroup(detail, QStringLiteral("detail"), false);
    }
    const auto group = [this, detail, bold](const QString& title) {
        auto* item = detail ? new QTreeWidgetItem(detail, {title}) : new QTreeWidgetItem(properties_, {title});
        bold(item);
        item->setExpanded(true);
        return item;
    };
    const auto row = [](QTreeWidgetItem* parent, const QString& label, const QString& value) {
        auto* item = new QTreeWidgetItem(parent, {label, value});
        item->setToolTip(1, value);
    };
    const auto origin = [this](const std::string& path) {
        const QString key = GameUiAssetKey(Text(path));
        if (const auto error = assetErrors_.constFind(key); error != assetErrors_.constEnd()) return tr("读取失败：%1").arg(*error);
        return assetOrigins_.value(key, IsLoading() ? tr("读取中…") : QString());
    };

    auto* basic = group(tr("节点"));
    if (!node.codeLabel.empty()) {
        // 代码区域在游戏里没有界面名，只能经游戏代码取得。
        row(basic, tr("名称"), Text(node.codeLabel));
        row(basic, tr("游戏内名称"), QStringLiteral("—"));
        row(basic, tr("标识"), Text(node.name));
        row(basic, tr("游戏类"), node.frameType.empty() ? QStringLiteral("—") : Text(node.frameType));
    } else {
        row(basic, tr("游戏内名称"), node.name.empty() ? QStringLiteral("—") : Text(node.name));
        row(basic, tr("类型"), TypeText(node));
    }
    if (!node.templateName.empty()) row(basic, tr("继承模板"), Text(node.templateName));
    if (!node.layer.empty()) row(basic, tr("绘制层"), Text(node.layer));
    const ScriptFramePlan* plan = ScriptFrames();
    if (node.scriptFrame && plan && *node.scriptFrame < plan->frames.size()) {
        const ScriptFrame& frame = plan->frames[*node.scriptFrame];
        const auto site = [](const ScriptCallSite& call) {
            return QStringLiteral("%1:%2  %3").arg(Text(call.scriptFile)).arg(call.line).arg(Text(call.function));
        };
        auto* script = group(tr("地图脚本"));
        row(script, tr("创建位置"), site(frame.created));
        row(script, tr("创建调用"), QStringLiteral("%1（%2）").arg(Text(frame.created.call),
            Text(std::string(UiApiFamilyName(frame.family)))));
        if (!frame.variable.empty()) row(script, tr("变量"), Text(frame.variable));
        switch (frame.object) {
        case ScriptObjectKind::TimerDialog: row(script, tr("对象"), tr("计时器窗口")); break;
        case ScriptObjectKind::Dialog: row(script, tr("对象"), tr("对话框")); break;
        case ScriptObjectKind::Leaderboard: row(script, tr("对象"), tr("排行榜")); break;
        case ScriptObjectKind::Multiboard: row(script, tr("对象"), tr("多面板")); break;
        default: break;
        }
        if (!frame.buttons.empty()) {
            QStringList buttons;
            for (const auto& button : frame.buttons) buttons.push_back(button.empty() ? tr("（运行时文字）") : Text(button));
            row(script, tr("按钮"), buttons.join(QStringLiteral("、")));
        }
        if (frame.board) {
            QString size = tr("%1 行 × %2 列").arg(frame.board->rows).arg(frame.board->columns);
            if (frame.board->minimized) size += tr("，最小化");
            row(script, tr("行列"), size);
        }
        if (frame.leaderboard) {
            // 游戏按无符号数取行数，负数即 24 行。
            const auto rows = std::min(static_cast<std::uint32_t>(frame.leaderboard->size), 24u);
            row(script, tr("项"), tr("%1 项，显示 %2 行").arg(frame.leaderboard->items.size()).arg(rows));
        }
        if (frame.nameDynamic) row(script, tr("控件名"), tr("运行时拼出"));
        QString parent = tr("未确定");
        if (frame.parent.target == ScriptFrameTarget::CodeArea || frame.parent.target == ScriptFrameTarget::Named ||
            frame.parent.target == ScriptFrameTarget::Script) {
            parent = Text(frame.parent.name);
        }
        row(script, tr("父控件"), parent);
        row(script, tr("创建后显示"), !frame.visible ? tr("默认显示") : (*frame.visible ? tr("显示") : tr("隐藏")));
        if (!frame.dynamic.empty()) {
            QStringList items;
            for (const auto& item : frame.dynamic) items.push_back(Text(item));
            row(script, tr("未还原的设置"), items.join(QStringLiteral("；")));
        }
        if (frame.runtimeCalls > 0) {
            QStringList lines;
            for (const auto& call : frame.runtimeSamples) lines.push_back(QStringLiteral("%1 %2").arg(Text(call.call)).arg(call.line));
            row(script, tr("运行时调用"), tr("%1 处：%2").arg(frame.runtimeCalls).arg(lines.join(QStringLiteral("，"))));
        }
    } else if (node.uiFrame) {
        const UiDescription* description = Description(Text(node.uiFrame->description));
        const auto named = [](const std::string& label, const std::string& id) {
            return label.empty() ? Text(id) : QStringLiteral("%1  %2").arg(Text(label), Text(id));
        };
        auto* own = group(tr("界面"));
        row(own, tr("界面"), description ? named(description->label, description->id) : Text(node.uiFrame->description));
        row(own, tr("文件"), Text(node.sourceFile));
        if (const UiFrame* frame = description ? FindUiFrame(*description, node.uiFrame->frame) : nullptr) {
            row(own, tr("控件"), named(frame->label, frame->id));
            row(own, tr("种类"), UiTypeText(frame->type));
            if (node.uiFrame->instance) row(own, tr("阵列序号"), QString::number(*node.uiFrame->instance));
            if (frame->value.binding) row(own, tr("进度取自"), BindingText(*frame->value.binding));
            if (frame->visible.binding) row(own, tr("显示取自"), BindingText(*frame->visible.binding));
            if (!frame->text.empty()) row(own, tr("文字模板"), Text(frame->text));
        }
        row(own, tr("当前显示"), node.hidden ? tr("隐藏") : tr("显示"));
        if (node.fill) row(own, tr("当前进度"), Number(*node.fill));
    } else if (!node.sourceFile.empty()) {
        row(basic, tr("定义位置"), QStringLiteral("%1:%2").arg(Text(node.sourceFile)).arg(node.sourceLine));
        if (definitions_ && definitions_->library) {
            for (const auto& file : definitions_->library->Files()) {
                if (file->file.path != node.sourceFile) continue;
                row(basic, tr("定义来源"), file->origin == "map" ? tr("地图界面目录") : tr("游戏界面目录"));
                break;
            }
        }
    }
    row(basic, tr("布局"), LayoutText(node.layout));
    if (node.codePlaced) row(basic, tr("定位来源"), tr("游戏代码（1.27a）"));
    if (!node.autoFillBy.empty()) row(basic, tr("由父控件铺满"), Text(node.autoFillBy));
    if (node.layout == LayoutState::Placed || node.layout == LayoutState::SizeFromContent) {
        const FrameRect& r = node.rect;
        row(basic, tr("位置"), tr("左 %1  下 %2  右 %3  上 %4").arg(Number(r.left), Number(r.bottom), Number(r.right), Number(r.top)));
        row(basic, tr("尺寸"), QStringLiteral("%1 × %2").arg(Number(r.Width()), Number(r.Height())));
        const QRect pixels = GameUiPixelRect(r, tree_->Options(), Aspect());
        row(basic, tr("像素"), tr("%1×1080：x %2  y %3  宽 %4  高 %5").arg(qRound(1080.0 * Aspect()))
            .arg(pixels.x()).arg(pixels.y()).arg(pixels.width()).arg(pixels.height()));
    }
    if (!node.anchors.empty()) {
        auto* anchors = group(tr("锚点"));
        for (const auto& anchor : node.anchors) {
            QString target = anchor.relativeName.empty() ? tr("父控件") : Text(anchor.relativeName);
            if (anchor.relativeNode == kNoNode && !anchor.relativeName.empty()) target += tr("（未找到）");
            row(anchors, Text(std::string(FramePointName(anchor.point))), tr("%1 的 %2  偏移 %3, %4")
                .arg(target, Text(std::string(FramePointName(anchor.relativePoint))), Number(anchor.x), Number(anchor.y)));
        }
    }
    const auto texture = [&](const QString& title, const TextureInfo& info) {
        auto* item = group(title);
        row(item, tr("路径"), Text(info.path));
        if (info.key != info.path) row(item, tr("皮肤键"), Text(info.key));
        row(item, tr("路径来源"), SkinSourceText(info.source));
        row(item, tr("文件来源"), origin(info.path));
        if (const auto image = textures_.constFind(GameUiAssetKey(Text(info.path))); image != textures_.constEnd())
            row(item, tr("图像尺寸"), QStringLiteral("%1 × %2").arg(image->width()).arg(image->height()));
        row(item, tr("贴图坐标"), QStringLiteral("%1, %2, %3, %4").arg(Number(info.texCoord[0]), Number(info.texCoord[1]),
            Number(info.texCoord[2]), Number(info.texCoord[3])));
        if (!info.alphaMode.empty()) row(item, tr("混合"), Text(info.alphaMode));
    };
    if (node.texture) texture(tr("贴图"), *node.texture);
    if (node.background) texture(tr("底图"), *node.background);
    if (node.edge) texture(tr("边框贴图"), *node.edge);
    if (node.text) {
        const TextInfo& info = *node.text;
        auto* item = group(tr("文字"));
        if (!info.key.empty()) row(item, tr("文字键"), Text(info.key));
        row(item, tr("显示文字"), Text(info.text));
        if (!info.source.empty()) row(item, tr("文字来源"), TextSourceText(info.source));
        if (!info.fontKey.empty()) row(item, tr("字体键"), Text(info.fontKey));
        if (!info.fontPath.empty()) {
            const QString family = fontFamilies_.value(GameUiAssetKey(Text(info.fontPath)));
            row(item, tr("字体文件"), Text(info.fontPath));
            row(item, tr("字体来源"), family.isEmpty() ? origin(info.fontPath) : QStringLiteral("%1 · %2").arg(origin(info.fontPath), family));
        }
        row(item, tr("字号"), Number(info.fontHeight));
        row(item, tr("颜色"), ColorText(info.color));
        if (!info.justifyH.empty() || !info.justifyV.empty())
            row(item, tr("对齐"), QStringLiteral("%1 %2").arg(Text(info.justifyH), Text(info.justifyV)).trimmed());
    }
    if (rawFields_ && rawFields_->isChecked() && !node.properties.empty()) {
        auto* raw = group(tr("原始字段"));
        for (const auto& property : node.properties) row(raw, Text(property.key), PropertyValues(property));
    }
}

std::vector<Diagnostic> GameUiModule::Problems() const {
    std::vector<Diagnostic> problems;
    const auto add = [&](DiagnosticSeverity severity, const char* code, const QString& message) {
        problems.push_back(Diagnostic{severity, "GameUi", code, message.toStdString(), std::nullopt});
    };
    if (project_.war3GameRoot_.empty()) add(DiagnosticSeverity::Error, "GAMEUI_GAME_ROOT_MISSING", tr("未设置游戏目录"));
    if (!sourceNote_.isEmpty()) add(DiagnosticSeverity::Info, "GAMEUI_MAP_LAYER", sourceNote_);
    problems.insert(problems.end(), loadDiagnostics_.begin(), loadDiagnostics_.end());
    problems.insert(problems.end(), sceneDiagnostics_.begin(), sceneDiagnostics_.end());
    problems.insert(problems.end(), simulationDiagnostics_.begin(), simulationDiagnostics_.end());
    if (simulation_) problems.insert(problems.end(), simulation_->diagnostics.begin(), simulation_->diagnostics.end());
    problems.insert(problems.end(), authoringDiagnostics_.begin(), authoringDiagnostics_.end());
    const auto& simulated = TriggerSimulationProblems();
    problems.insert(problems.end(), simulated.begin(), simulated.end());
    if (const ScriptFramePlan* plan = ScriptFrames(); plan && roots_.contains(QString::fromLatin1(kScriptRoot.data(),
            static_cast<qsizetype>(kScriptRoot.size())), Qt::CaseInsensitive)) {
        const auto calls = ScriptCallDiagnostics(*plan);
        problems.insert(problems.end(), calls.begin(), calls.end());
    }
    for (auto it = assetErrors_.constBegin(); it != assetErrors_.constEnd(); ++it)
        add(DiagnosticSeverity::Warning, "GAMEUI_ASSET_UNREADABLE", QStringLiteral("%1：%2").arg(it.key(), it.value()));
    return problems;
}

void GameUiModule::UpdateProblems() {
    if (!problemModel_) return;
    problemModel_->removeRows(0, problemModel_->rowCount());
    for (const auto& problem : Problems()) {
        problemModel_->appendRow({new QStandardItem(SeverityText(problem.severity)), new QStandardItem(Text(problem.message)),
            new QStandardItem(Text(problem.code))});
    }
    if (problems_) problems_->SetState(IsLoading() ? tr("读取中…") : QString());
}

} // namespace War3Map::Modules

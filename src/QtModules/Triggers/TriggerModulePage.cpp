#include "TriggerModule.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "Widgets/ColumnLayout.h"
#include "Widgets/ResponsiveToolBar.h"
#include "Modules/ModuleHost.h"
#include "ObjectNames.h"
#include "TriggerModels.h"
#include "TriggerSentenceDelegate.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "Widgets/SearchField.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "TriggerCodeChecking.h"
#include "TriggerLuaScripts.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTabWidget>
#include <QTreeView>
#include <QUndoStack>
#include <QVBoxLayout>

// 触发器模块的页面搭建与信号连接（布局对应原型 TriggerWorkspace：左目录、上信息条、右逻辑区）。
namespace War3Map::Modules {
void TriggerModule::BuildPage() {
    page_ = new QWidget;
    page_->setObjectName("triggerWorkspace");
    auto* layout = new QVBoxLayout(page_);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto* heading = new QVBoxLayout;
    heading->setSpacing(3);
    auto* title = new QLabel(tr("触发器"), page_);
    title->setObjectName("workspaceTitle");
    heading->addWidget(title);
    summary_ = new QLabel(page_);
    summary_->setObjectName("subtitle");
    summary_->setWordWrap(true);
    heading->addWidget(summary_);
    layout->addLayout(heading);

    auto* commandBar = new UiKit::ResponsiveToolBar(page_);
    commandBar->setObjectName("triggerCommandBar");
    auto* commands = commandBar->ContentLayout();
    commands->setContentsMargins(8, 7, 8, 7);
    commands->setSpacing(6);
    addEventButton_ = new QPushButton(tr("新事件"), commandBar);
    addConditionButton_ = new QPushButton(tr("新条件"), commandBar);
    addActionButton_ = new QPushButton(tr("新动作"), commandBar);
    editEcaButton_ = new QPushButton(tr("编辑参数"), commandBar);
    addEventButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/zap")));
    addConditionButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/list-filter")));
    addActionButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    editEcaButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/pencil")));
    addActionButton_->setObjectName("primaryButton");
    editEcaButton_->setObjectName("accentButton");
    for (QPushButton* button : {addEventButton_, addConditionButton_, addActionButton_, editEcaButton_}) {
        button->setEnabled(false);
        commands->addWidget(button);
    }
    auto* divider = new QFrame(commandBar);
    divider->setFrameShape(QFrame::VLine);
    divider->setObjectName("commandDivider");
    commands->addWidget(divider);
    // 撤销 / 重做按钮保留在命令条上，显示将要撤销的动作名称。
    auto* undoButton = new QPushButton(tr("撤销"), commandBar);
    auto* redoButton = new QPushButton(tr("重做"), commandBar);
    undoButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/undo-2")));
    redoButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/redo-2")));
    undoButton->setEnabled(false);
    redoButton->setEnabled(false);
    commands->addWidget(undoButton);
    commands->addWidget(redoButton);
    auto* variablesButton = new QPushButton(tr("变量…"), commandBar);
    variablesButton->setObjectName("quietButton");
    variablesButton->setToolTip(tr("管理全局变量（新建/重命名/删除，可撤销）"));
    commands->addWidget(variablesButton);
    auto* replaceButton = new QPushButton(tr("查找/替换…"), commandBar);
    replaceButton->setObjectName(QStringLiteral("quietButton"));
    replaceButton->setProperty("triggerFindReplaceButton", true);
    replaceButton->setToolTip(tr("查找传统触发器参数和引用；预览后一次撤销整批变量或物体替换"));
    connect(replaceButton, &QPushButton::clicked, this, &TriggerModule::ShowFindReplace);
    commands->addWidget(replaceButton);
    mapScriptButton_ = new QPushButton(tr("地图脚本"), commandBar);
    mapScriptButton_->setObjectName("triggerMapScriptButton");
    mapScriptButton_->setToolTip(tr("只读查看 war3map.j"));
    commands->addWidget(mapScriptButton_);
    auto* globalCode = new QPushButton(tr("全局代码"), commandBar);
    globalCode->setObjectName("triggerGlobalCodeButton");
    globalCode->setToolTip(tr("编辑地图头部的 JASS / vJass 全局自定义代码（WCT）；原生 Lua 地图按其来源语言编辑。"));
    commands->addWidget(globalCode);
    connect(globalCode, &QPushButton::clicked, this, [this] { (void)ShowGlobalCustomCode(); });
    commands->addStretch();
    auto* libraries = new QPushButton(tr("函数库…"), commandBar);
    libraries->setObjectName("quietButton");
    libraries->setToolTip(tr("启用 / 排序函数库，查看本图用到的包与同名定义冲突"));
    commands->addWidget(libraries);
    auto* selectUiRoot = new QPushButton(tr("UI 数据根"), commandBar);
    selectUiRoot->setObjectName("quietButton");
    commands->addWidget(selectUiRoot);
    connect(libraries, &QPushButton::clicked, this, &TriggerModule::OpenUiLibraries);
    layout->addWidget(commandBar);

    auto* splitter = new QSplitter(page_);
    splitter->setObjectName("triggerSplitter");
    splitter->setChildrenCollapsible(false);

    auto* explorerPane = new QWidget(splitter);
    explorerPane->setObjectName("workspacePane");
    auto* explorerLayout = new QVBoxLayout(explorerPane);
    explorerLayout->setContentsMargins(0, 0, 10, 0);
    explorerLayout->setSpacing(7);
    auto* explorerHeading = new QHBoxLayout;
    explorerHeading->setSpacing(2);
    auto* explorerTitle = new QLabel(tr("触发器"), explorerPane);
    explorerTitle->setObjectName("paneTitle");
    explorerHeading->addWidget(explorerTitle);
    explorerHeading->addStretch();
    auto* newTrigger = new QToolButton(explorerPane);
    newTrigger->setObjectName("triggerNewTrigger");
    UiKit::SetToolButtonSvgIcon(*newTrigger, QStringLiteral("lucide/plus"), tr("新建"), Qt::ToolButtonTextBesideIcon);
    newTrigger->setToolTip(tr("新建触发器 · Insert / Ctrl+T；下拉可新建自定义代码、注释或类别"));
    newTrigger->setAutoRaise(true);
    auto* newMenu = new QMenu(newTrigger);
    newMenu->setObjectName("triggerNewItemMenu");
    AddNewItemActions(*newMenu);
    newTrigger->setMenu(newMenu);
    newTrigger->setPopupMode(QToolButton::MenuButtonPopup);
    auto* newCategory = new QToolButton(explorerPane);
    newCategory->setObjectName("triggerNewCategory");
    UiKit::SetToolButtonSvgIcon(*newCategory, QStringLiteral("lucide/folder-plus"), tr("新建类别"));
    newCategory->setToolTip(tr("新建类别 · Ctrl+G"));
    newCategory->setAutoRaise(true);
    explorerHeading->addWidget(newTrigger);
    auto* newCustom = new QToolButton(explorerPane);
    newCustom->setObjectName("triggerNewCustomCode");
    UiKit::SetToolButtonSvgIcon(*newCustom, QStringLiteral("lucide/file-code-2"), tr("新建自定义代码"));
    newCustom->setToolTip(tr("新建自定义代码"));
    newCustom->setAutoRaise(true);
    explorerHeading->addWidget(newCustom);
    explorerHeading->addWidget(newCategory);
    explorerLayout->addLayout(explorerHeading);
    explorerSearch_ = new QLineEdit(explorerPane);
    explorerSearch_->setObjectName("triggerSearch");
    explorerSearch_->setPlaceholderText(tr("搜索触发器"));
    explorerSearch_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(explorerSearch_);
    explorerLayout->addWidget(explorerSearch_);
    explorerView_ = new QTreeView(explorerPane);
    explorerView_->setObjectName("triggerExplorer");
    explorerModel_ = new TriggerExplorerModel(explorerView_);
    filter_ = new QSortFilterProxyModel(explorerView_);
    filter_->setSourceModel(explorerModel_);
    filter_->setRecursiveFilteringEnabled(true);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    filter_->setFilterKeyColumn(0);
    explorerView_->setModel(filter_);
    explorerView_->setHeaderHidden(true);
    explorerView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    explorerView_->setSelectionMode(QAbstractItemView::SingleSelection);
    explorerView_->setUniformRowHeights(true);
    explorerView_->setIndentation(16);
    explorerView_->setIconSize(QSize(16, 16)); // 原版图标是 16 像素点阵图，按整数倍显示
    // 原型行为：F2 / 双击触发器就地重命名；双击类别仍是展开 / 折叠。
    explorerView_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    explorerView_->setDragEnabled(true);
    explorerView_->setAcceptDrops(true);
    explorerView_->setDropIndicatorShown(true);
    explorerView_->setDragDropMode(QAbstractItemView::InternalMove);
    explorerView_->setDefaultDropAction(Qt::MoveAction);
    explorerView_->header()->setStretchLastSection(false);
    explorerView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    explorerView_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    explorerLayout->addWidget(explorerView_, 1);
    splitter->addWidget(explorerPane);

    auto* logicPane = new QWidget(splitter);
    logicPane->setObjectName("workspacePane");
    auto* logicLayout = new QVBoxLayout(logicPane);
    logicLayout->setContentsMargins(10, 0, 10, 0);
    logicLayout->setSpacing(7);
    auto* logicHeading = new QHBoxLayout;
    logicTitle_ = new QLabel(tr("触发器逻辑"), logicPane);
    logicTitle_->setObjectName("paneTitle");
    logicHint_ = new QLabel(logicPane);
    logicHint_->setObjectName("paneHint");
    logicHeading->addWidget(logicTitle_);
    convertCodeButton_ = new QPushButton(tr("转换为自定义代码…"), logicPane);
    convertCodeButton_->setObjectName("triggerConvertCode");
    convertCodeButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/file-code-2")));
    convertCodeButton_->setToolTip(tr("将当前传统触发器转换为 JASS / vJass；确认后可撤销。"));
    convertCodeButton_->hide();
    logicHeading->addWidget(convertCodeButton_);
    recoverCodeButton_ = new QPushButton(tr("转回 T…"), logicPane);
    recoverCodeButton_->setObjectName("triggerRecoverCode");
    recoverCodeButton_->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/list-tree")));
    recoverCodeButton_->setToolTip(tr("把代码转回传统事件 / 条件 / 动作；只有按 T 重新生成与代码一致时才转回，可撤销。"));
    recoverCodeButton_->hide();
    logicHeading->addWidget(recoverCodeButton_);
    logicHeading->addStretch();
    showRawValues_ = new QCheckBox(tr("显示原始字段"), logicPane);
    showRawValues_->setObjectName("triggerRawValues");
    logicHeading->addWidget(showRawValues_);
    logicHeading->addWidget(logicHint_);
    logicLayout->addLayout(logicHeading);

    // 信息条（对应原版触发器面板上方）：允许 / 初始打开 / 地图初始化时运行 + 可展开的注释。
    propertiesBox_ = new QFrame(logicPane);
    propertiesBox_->setObjectName("triggerMetaBar");
    auto* propertiesLayout = new QVBoxLayout(propertiesBox_);
    propertiesLayout->setContentsMargins(8, 5, 8, 5);
    propertiesLayout->setSpacing(5);
    auto* toggles = new QHBoxLayout;
    toggles->setSpacing(14);
    enabledCheck_ = new QCheckBox(tr("允许"), propertiesBox_);
    initiallyOnCheck_ = new QCheckBox(tr("初始打开"), propertiesBox_);
    runOnInitCheck_ = new QCheckBox(tr("地图初始化时运行"), propertiesBox_);
    enabledCheck_->setObjectName("triggerEnabledCheck");
    initiallyOnCheck_->setObjectName("triggerInitiallyOnCheck");
    enabledCheck_->setToolTip(tr("取消后触发器不生成代码，也不会被创建"));
    initiallyOnCheck_->setToolTip(tr("取消后触发器在地图初始化时被关闭，需要用“开启触发器”动作打开"));
    runOnInitCheck_->setToolTip(tr("在地图初始化阶段自动运行该触发器"));
    metaNote_ = new QLabel(propertiesBox_);
    metaNote_->setObjectName("triggerMetaNote");
    commentToggle_ = new QToolButton(propertiesBox_);
    commentToggle_->setObjectName("triggerCommentToggle");
    commentToggle_->setCheckable(true);
    commentToggle_->setAutoRaise(true);
    commentToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    commentToggle_->setToolTip(tr("触发器注释"));
    toggles->addWidget(enabledCheck_);
    toggles->addWidget(initiallyOnCheck_);
    toggles->addWidget(runOnInitCheck_);
    toggles->addWidget(metaNote_);
    toggles->addStretch();
    toggles->addWidget(commentToggle_);
    propertiesLayout->addLayout(toggles);
    connect(&project_, &QtProject::EditorProject::TriggerDocumentAboutToChange, this, [this] {
        scriptCapabilityCached_ = false;
    });
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, [this] {
        scriptCapabilityCached_ = false;
        if (page_) UpdateMetaBar(CurrentTrigger());
    });
    commentEdit_ = new QPlainTextEdit(propertiesBox_);
    commentEdit_->setObjectName("triggerCommentEdit");
    commentEdit_->setPlaceholderText(tr("触发器注释"));
    commentEdit_->setTabChangesFocus(true);
    commentEdit_->setFixedHeight(56);
    commentEdit_->installEventFilter(this);
    commentEdit_->hide();
    propertiesLayout->addWidget(commentEdit_);
    propertiesBox_->setEnabled(false);
    logicLayout->addWidget(propertiesBox_);

    ecaView_ = new QTreeView(logicPane);
    ecaView_->setObjectName("triggerLogicTree");
    ecaModel_ = new TriggerEcaModel(ecaView_);
    ecaView_->setModel(ecaModel_);
    ecaView_->setAlternatingRowColors(true);
    ecaView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ecaView_->setSelectionMode(QAbstractItemView::SingleSelection);
    // 拖放：同分支重排或移到其他分支（含空的“否则”）；禁止移入自身子块，类型必须匹配。
    ecaView_->setDragEnabled(true);
    ecaView_->setAcceptDrops(true);
    ecaView_->setDropIndicatorShown(true);
    ecaView_->setDragDropMode(QAbstractItemView::InternalMove);
    ecaView_->setDefaultDropAction(Qt::MoveAction);
    // 语句行把参数画成可点击的记号；点击打开取值面板（原型 ValueToken + SharedValuePicker）。
    sentenceDelegate_ = new TriggerSentenceDelegate(ecaView_);
    ecaView_->setItemDelegateForColumn(0, sentenceDelegate_);
    sentenceDelegate_->SetActivate([this](const QModelIndex& index, std::size_t position, const QRect& anchor) {
        const auto* eca = ecaModel_->EcaAt(index);
        if (eca == nullptr) return;
        ecaView_->setCurrentIndex(index);
        OpenValuePanel(eca->id, position, anchor);
    });
    ecaModel_->SetMoveHandler([this](const EcaMoveRequest& request) {
        QTimer::singleShot(0, this, [this, request]() { ApplyEcaMove(request); });
    });
    ecaView_->setUniformRowHeights(true);
    ecaView_->setIndentation(18);
    ecaView_->setIconSize(QSize(18, 18));
    ecaView_->setAnimated(true);
    // 默认把余量交给语句；两列仍可拖宽或双击适配，手动调整后不再自动覆盖。
    auto* logicHeader = ecaView_->header();
    logicHeader->setMaximumSectionSize(kMaximumLogicColumnWidth);
    UiKit::ConfigureResizableColumns(logicHeader, {320, kLogicTypeColumnWidth});
    logicHeader->setStretchLastSection(false);
    connect(logicHeader, &QHeaderView::sectionResized, this, [this](int, int, int) {
        if (updatingLogicColumns_) return;
        manualLogicColumns_ = true;
        // 文档定位可能还在等待地图载入，不能用旧缓存覆盖之后手动调整的列宽。
        if (!pendingViewState_.isEmpty()) pendingViewState_["logicColumnWidths"] = UiKit::SaveColumnWidths(ecaView_->header());
    });
    logicViewport_ = ecaView_->viewport();
    logicViewport_->installEventFilter(this);
    logicLayout->addWidget(ecaView_, 1);
    codeActions_ = new QWidget(logicPane);
    codeActions_->setObjectName("triggerCodeActions");
    auto* codeActionsLayout = new QHBoxLayout(codeActions_);
    codeActionsLayout->setContentsMargins(0, 0, 0, 0);
    applyCodeButton_ = new QPushButton(tr("应用代码"), codeActions_);
    applyCodeButton_->setObjectName("triggerApplyCode");
    discardCodeButton_ = new QPushButton(tr("放弃草稿"), codeActions_);
    discardCodeButton_->setObjectName("triggerDiscardCode");
    codeDraftNote_ = new QLabel(codeActions_);
    codeDraftNote_->setObjectName("triggerCodeDraftNote");
    codeDraftNote_->setTextFormat(Qt::PlainText);
    codeActionsLayout->addWidget(applyCodeButton_);
    codeActionsLayout->addWidget(discardCodeButton_);
    codeActionsLayout->addWidget(codeDraftNote_, 1);
    codeActions_->hide();
    logicLayout->addWidget(codeActions_);
    // 自定义代码与全局头部都经共用 sealed 事务写 WCT；输入草稿使用控件自身撤销。
    codeTools_ = new QWidget(logicPane);
    auto* tools = new QHBoxLayout(codeTools_); tools->setContentsMargins(0, 0, 0, 0);
    codeLanguage_ = new QComboBox(codeTools_); codeLanguage_->setObjectName("triggerCodeLanguage");
    codeLanguage_->addItems({QStringLiteral("JASS"), QStringLiteral("vJass"), QStringLiteral("Lua 5.3")});
    codeLanguage_->setCurrentIndex(1);
    auto* checkCode = new QPushButton(tr("检查代码"), codeTools_); checkCode->setObjectName("triggerCheckCode");
    checkCode->setToolTip(tr("Ctrl+Shift+B；完整保存校验仍由保存链执行"));
    tools->addWidget(codeLanguage_); tools->addWidget(checkCode); tools->addStretch();
    auto* keys = new QLabel(tr("Ctrl+Space 补全 · F12 定义 · Shift+F12 引用"), codeTools_); tools->addWidget(keys);
    codeTools_->hide(); logicLayout->addWidget(codeTools_);
    scriptView_ = new UiKit::CodeView(logicPane);
    scriptView_->setObjectName("triggerScriptView");
    scriptView_->SetEditable(true);
    scriptView_->SetDecoder(DecodeMapCodeForView);
    scriptView_->SetLanguage(UiKit::CodeLanguage::VJass);
    scriptView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    scriptView_->setFont(QFont(QStringLiteral("Consolas"), 11));
    scriptView_->setToolTip(tr("自定义代码；原生 Lua 地图自动使用 Lua 5.3，应用后进入地图撤销栈，保存时走同一编译校验链。"));
    scriptView_->hide();
    logicLayout->addWidget(scriptView_, 1);
    codeProblems_ = new UiKit::CodeProblemsView(logicPane); codeProblems_->SetView(scriptView_); codeProblems_->hide();
    logicLayout->addWidget(codeProblems_);
    connect(scriptView_, &UiKit::CodeView::TextLoaded, this, [this] {
        if (!codeTarget_ || codeDraftPending_ || !codeBaselinePlan_ || codeBaselinePlan_->Changed()
            || !codeDisplayLoadGeneration_ || *codeDisplayLoadGeneration_ != scriptView_->Generation()) return;
        const auto target = codeBaselinePlan_->Target();
        if (target.kind != codeTarget_->kind || target.triggerId != codeTarget_->triggerId) return;
        // 只接收当前异步载入；旧回执不能把后来的作者草稿重新标成未改。
        codeDisplayBaselineText_ = scriptView_->toPlainText();
        codeDisplayLoadGeneration_.reset();
        scriptView_->SetEditable(CodeAuthoringAllowed());
        UpdateCodeDraftState();
    });
    connect(scriptView_, &UiKit::CodeView::AnalysisReady, this, [this] {
        if (!codeTarget_ || codeDraftPending_ || !project_.triggerDocument_) return;
        const auto& payload = scriptView_->CheckResult().ownedProjectIndex;
        if (!payload) return;
        const Trigger::RawText* live = nullptr;
        if (codeTarget_->kind == Trigger::CodeBlockKind::GlobalHeader) live = &project_.triggerDocument_->wct.customCode;
        else if (const auto* trigger = Trigger::TriggerDocumentEditor::FindTrigger(*project_.triggerDocument_, codeTarget_->triggerId))
            live = &trigger->customText;
        if (!live || scriptView_->toPlainText().toStdString() != Trigger::TriggerCodeEditor::NormalizedText(*live)) return;
        project_.AdoptCodeCheckIndex(std::static_pointer_cast<const Services::CodeCheckResult>(payload));
    });
    connect(checkCode, &QPushButton::clicked, scriptView_, &UiKit::CodeView::RequestCheck);
    connect(codeLanguage_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (codeNativeLua_ && index != 2) {
            codeLanguage_->setCurrentIndex(2); return;
        }
        scriptView_->SetLanguage(index == 2 ? UiKit::CodeLanguage::Lua : index == 1 ? UiKit::CodeLanguage::VJass : UiKit::CodeLanguage::Jass);
        ConfigureCodeChecking();
    });
    connect(scriptView_, &UiKit::CodeView::JumpRequested, this, [this](const QString& name, const QString& source, int line, int column) {
        if (source == scriptView_->SourceName() && line > 0) scriptView_->GoToLine(line, column);
        else {
            for (const auto& frozen : scriptView_->CheckResult().externalSources) if (frozen.name == source) return;
            ShowStatus(source.isEmpty() ? tr("%1 的定义尚无唯一来源；请使用已载入声明上下文。").arg(name) : tr("定义位于 %1:%2；该来源未包含在本次导航清单中。").arg(source).arg(line), 8000);
        }
    });
    connect(scriptView_, &QPlainTextEdit::textChanged, this, &TriggerModule::UpdateCodeDraftState);
    connect(applyCodeButton_, &QPushButton::clicked, this, [this] { (void)CommitCodeDraft(); });
    connect(discardCodeButton_, &QPushButton::clicked, this, &TriggerModule::DiscardPendingCodeDraft);
    connect(convertCodeButton_, &QPushButton::clicked, this, [this] { (void)ConvertCurrentTriggerToCustomCode(); });
    connect(recoverCodeButton_, &QPushButton::clicked, this, [this] { (void)ConvertCurrentCodeToTrigger(); });
    BuildMapScriptView(logicPane, logicLayout);
    BuildBlueprint(logicPane, logicLayout, logicHeading);
    splitter->addWidget(logicPane);

    // WE 式布局：左侧触发器树 + 右侧大逻辑区；参数编辑走双击 / 按钮弹出的对话框。
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 1100});
    workspaceTabs_ = new QTabWidget(page_);
    workspaceTabs_->setObjectName("triggerWorkspaceTabs");
    workspaceTabs_->addTab(splitter, tr("触发器"));
    luaScripts_ = new TriggerLuaScripts(project_, workspaceTabs_);
    workspaceTabs_->addTab(luaScripts_, tr("Lua 文件"));
    layout->addWidget(workspaceTabs_, 1);
    connect(luaScripts_, &TriggerLuaScripts::DraftEdited, this, &TriggerModule::DraftEdited);
    connect(luaScripts_, &TriggerLuaScripts::RevealAssetRequested, this, [this](const QString& path) {
        if (host_) host_->Navigate({"assets", "asset", path});
    });
    connect(workspaceTabs_, &QTabWidget::currentChanged, this, [this, commandBar](int index) {
        if (index == lastWorkspaceTab_) return;
        if (!CommitPendingEdits()) {
            const QSignalBlocker blocked(workspaceTabs_);
            workspaceTabs_->setCurrentIndex(lastWorkspaceTab_);
            return;
        }
        lastWorkspaceTab_ = index;
        commandBar->setVisible(index == 0);
        if (host_) host_->UndoStackChanged("triggers");
    });

    // 参数类型及原始身份交由同一 N1 服务；不在页面里跨七类猜测或重做 WTS/父链。
    ecaModel_->SetIconProvider(project_.iconProvider_.get());
    explorerModel_->SetIconProvider(project_.iconProvider_.get());
    referenceResolver_ = [this](std::string_view raw, std::string_view type) {
        return ResolveProjectReferencePresentation(project_, raw, type);
    };
    Trigger::PresentationOptions nameOptions;
    nameOptions.referenceResolver = referenceResolver_;
    ecaModel_->SetPresentationOptions(std::move(nameOptions));
    connect(showRawValues_, &QCheckBox::toggled, this, [this](bool raw) {
        const auto* selected = ecaModel_->EcaAt(ecaView_->currentIndex());
        const auto id = selected ? selected->id : 0;
        Trigger::PresentationOptions options;
        options.referenceResolver = referenceResolver_;
        options.showRawValues = raw;
        ecaModel_->SetPresentationOptions(std::move(options));
        if (id) ecaView_->setCurrentIndex(ecaModel_->IndexForStableId(id));
        SyncBlueprint();
    });
    connect(commentEdit_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!suppressPropertySignals_) {
            emit DraftEdited();
        }
    });
    for (QCheckBox* check : {enabledCheck_, initiallyOnCheck_, runOnInitCheck_}) {
        connect(check, &QCheckBox::toggled, this, [this](bool) { CommitProperties(); });
    }
    connect(commentToggle_, &QToolButton::toggled, this, [this](bool open) {
        commentEdit_->setVisible(open);
        if (open) {
            commentEdit_->setFocus();
        } else {
            CommitProperties();
            UpdateMetaBar(CurrentTrigger());
        }
    });
    connect(newTrigger, &QToolButton::clicked, this, &TriggerModule::NewTrigger);
    connect(newCustom, &QToolButton::clicked, this, &TriggerModule::NewCustomTrigger);
    connect(newCategory, &QToolButton::clicked, this, &TriggerModule::NewCategory);
    connect(explorerSearch_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_->setFilterFixedString(text);
        ApplyExplorerExpansion();
    });
    explorerModel_->SetRenameHandler([this](const TriggerDirectoryItem& item, const QString& name) {
        return RenameDirectoryItem(item, name);
    });
    explorerModel_->SetMoveHandler([this](const TriggerDirectoryMove& move) {
        // 拖放事件仍在进行：排队到事件循环，避免在视图的 dropEvent 中重置模型。
        QTimer::singleShot(0, this, [this, move]() { ApplyDirectoryMove(move); });
    });
    connect(explorerView_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        UpdateCategoryIcons();
        if (refreshingExplorer_ || !explorerSearch_->text().isEmpty()) return;
        if (const auto id = explorerModel_->CategoryIdAt(filter_->mapToSource(index))) collapsedCategories_.erase(*id);
    });
    connect(explorerView_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        UpdateCategoryIcons();
        if (refreshingExplorer_ || !explorerSearch_->text().isEmpty()) return;
        if (const auto id = explorerModel_->CategoryIdAt(filter_->mapToSource(index))) collapsedCategories_.insert(*id);
    });
    connect(explorerView_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (explorerModel_->TriggerAt(filter_->mapToSource(index)) != nullptr) {
            explorerView_->edit(index.siblingAtColumn(0));
        }
    });
    connect(selectUiRoot, &QPushButton::clicked, this, &TriggerModule::UiRootRequested);
    connect(addEventButton_, &QPushButton::clicked, this, [this]() { AddEca(0); });
    connect(addConditionButton_, &QPushButton::clicked, this, [this]() { AddEca(1); });
    connect(addActionButton_, &QPushButton::clicked, this, [this]() { AddEca(2); });
    connect(editEcaButton_, &QPushButton::clicked, this, &TriggerModule::EditSelectedEca);
    if (QUndoStack* stack = project_.triggerUndoStack_) {
        connect(undoButton, &QPushButton::clicked, stack, &QUndoStack::undo);
        connect(redoButton, &QPushButton::clicked, stack, &QUndoStack::redo);
        connect(stack, &QUndoStack::canUndoChanged, undoButton, &QPushButton::setEnabled);
        connect(stack, &QUndoStack::canRedoChanged, redoButton, &QPushButton::setEnabled);
        connect(stack, &QUndoStack::undoTextChanged, undoButton, [undoButton](const QString& text) {
            undoButton->setText(text.isEmpty() ? QObject::tr("撤销") : QObject::tr("撤销 · %1").arg(text));
        });
        connect(stack, &QUndoStack::redoTextChanged, redoButton, [redoButton](const QString& text) {
            redoButton->setText(text.isEmpty() ? QObject::tr("重做") : QObject::tr("重做 · %1").arg(text));
        });
    }
    connect(explorerView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex& current, const QModelIndex&) {
            if (refreshingExplorer_) {
                return;
            }
            const auto item = explorerModel_->ItemAt(filter_->mapToSource(current));
            if (!CommitPendingEdits()) {
                const QSignalBlocker blocker(explorerView_->selectionModel());
                const auto previous = selectedTrigger_ ? explorerModel_->IndexForTriggerId(selectedTrigger_)
                    : selectedCategory_ ? explorerModel_->IndexForCategoryId(*selectedCategory_) : QModelIndex{};
                explorerView_->setCurrentIndex(filter_->mapFromSource(previous));
                return;
            }
            selectedTrigger_ = item && !item->category ? item->triggerId : 0;
            selectedCategory_ = item && item->category ? std::optional<std::int32_t>(item->categoryId) : std::nullopt;
            if (item) selectionCategory_ = item->categoryId;
            {
                // 提交可能重置目录模型；按已捕获的稳定身份恢复这次导航的目标。
                const QSignalBlocker blocker(explorerView_->selectionModel());
                const auto source = selectedTrigger_ ? explorerModel_->IndexForTriggerId(selectedTrigger_)
                    : selectedCategory_ ? explorerModel_->IndexForCategoryId(*selectedCategory_) : QModelIndex{};
                explorerView_->setCurrentIndex(filter_->mapFromSource(source));
            }
            ShowSelectedTrigger();
        });
    explorerView_->installEventFilter(this);
    connect(ecaView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
        [this](const QModelIndex&, const QModelIndex&) { ShowSelectedEca(); });
    connect(variablesButton, &QPushButton::clicked, this, [this]() {
        if (project_.triggerDocument_ != nullptr && host_ != nullptr) {
            host_->ActivateModule("variables");
        }
    });
    connect(ecaView_, &QTreeView::doubleClicked, this, [this](const QModelIndex&) { EditSelectedEca(); });
    ecaView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ecaView_, &QTreeView::customContextMenuRequested, this, &TriggerModule::ShowEcaContextMenu);
    explorerView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(explorerView_, &QTreeView::customContextMenuRequested, this, &TriggerModule::ShowTreeContextMenu);
}

} // namespace War3Map::Modules

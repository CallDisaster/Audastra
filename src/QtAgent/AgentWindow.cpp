#include "AgentWindow.h"

#include "AgentConversationView.h"
#include "AgentSessionStore.h"
#include "AnimatedCounter.h"
#include "ComposerWidgets.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"
#include "Widgets/PopoverSurface.h"
#include "Widgets/SearchField.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHideEvent>
#include <QInputDialog>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace War3Map::Agent {
namespace {

constexpr qsizetype kMaximumHistoryMessages = 20;
constexpr qsizetype kMaximumHistoryCharacters = 512 * 1024;
constexpr qsizetype kMaximumAttachments = 8;
constexpr qsizetype kMaximumInspectorRows = 5;

// 输入框左下的访问权限档位。实际能否执行仍由共用工具服务按设置验证。
struct PermissionLevel {
    const char* id;
    const char* icon;
    const char* title;
    const char* description;
    bool edits;
    bool save;
    bool commands;
};
constexpr std::array<PermissionLevel, 4> kPermissionLevels{{
    {"read", "lucide/lock", "只读", "只查询地图内容，不做任何修改", false, false, false},
    {"edit", "lucide/pencil", "可编辑", "可以修改触发器、变量和物编，不写入地图文件", true, false, false},
    {"save", "lucide/save", "编辑并保存", "可以修改并保存地图，保存前自动备份", true, true, false},
    {"full", "lucide/shield-alert", "完全访问", "另外允许运行外部程序，例如脚本编译器", true, true, true},
}};

std::size_t PermissionIndex(const AgentSettings& settings) {
    if (settings.allowCommands) return 3;
    if (settings.allowSave) return 2;
    if (settings.allowEdits) return 1;
    return 0;
}

const UiKit::Tokens::ThemeTokens& ResolveAgentTheme(const QWidget* widget) {
    const QColor windowColor = widget->palette().color(QPalette::Window);
    for (const auto& candidate : UiKit::Tokens::kThemes) {
        if (UiKit::ToQColor(candidate.colors.bg) == windowColor) return candidate;
    }
    if (windowColor.lightness() > 140) return UiKit::ThemeManager::Theme(QStringLiteral("paper"));
    if (windowColor.blue() - windowColor.red() >= 10) return UiKit::ThemeManager::Theme(QStringLiteral("midnight"));
    return UiKit::ThemeManager::Theme(QStringLiteral("graphite"));
}

QString ShortToolLabel(const QJsonObject& event) {
    QString title = event.value(QStringLiteral("title")).toString();
    const QString toolName = event.value(QStringLiteral("tool_name")).toString();
    if (!toolName.isEmpty()) {
        static const QHash<QString, QString> labels{
            {QStringLiteral("editor_state"), QStringLiteral("读取编辑器状态")},
            {QStringLiteral("objects_list"), QStringLiteral("查找物编对象")},
            {QStringLiteral("objects_get"), QStringLiteral("读取对象字段")},
            {QStringLiteral("objects_text"), QStringLiteral("读取对象字段文本")},
            {QStringLiteral("objects_set_fields"), QStringLiteral("修改对象字段")},
            {QStringLiteral("objects_apply_text"), QStringLiteral("按文本修改对象字段")},
            {QStringLiteral("objects_create"), QStringLiteral("创建物编对象")},
            {QStringLiteral("objects_delete"), QStringLiteral("删除物编对象")},
            {QStringLiteral("objects_field_options"), QStringLiteral("查询字段选项")},
            {QStringLiteral("variables_list"), QStringLiteral("浏览变量")},
            {QStringLiteral("variables_get"), QStringLiteral("读取变量")},
            {QStringLiteral("variables_create"), QStringLiteral("创建变量")},
            {QStringLiteral("variables_update"), QStringLiteral("修改变量")},
            {QStringLiteral("variables_rename"), QStringLiteral("重命名变量")},
            {QStringLiteral("variables_delete"), QStringLiteral("删除变量")},
            {QStringLiteral("entities_query"), QStringLiteral("查询地图实体")},
            {QStringLiteral("entities_set_fields"), QStringLiteral("修改实体字段")},
            {QStringLiteral("entities_rotate"), QStringLiteral("调整实体朝向")},
            {QStringLiteral("entities_transform"), QStringLiteral("移动或缩放实体")},
            {QStringLiteral("entities_copy"), QStringLiteral("复制预放实体")},
            {QStringLiteral("sounds_list"), QStringLiteral("查询声音定义")},
            {QStringLiteral("sounds_get"), QStringLiteral("读取声音定义")},
            {QStringLiteral("sounds_update"), QStringLiteral("修改声音参数")},
            {QStringLiteral("imports_list"), QStringLiteral("查询导入资源")},
            {QStringLiteral("imports_get"), QStringLiteral("读取导入信息")},
            {QStringLiteral("editor_query_trigger_graph"), QStringLiteral("查询触发器结构")},
            {QStringLiteral("editor_edit_trigger_graph"), QStringLiteral("编辑触发器")},
            {QStringLiteral("editor_get_ui_catalog"), QStringLiteral("查询触发器函数库")},
            {QStringLiteral("editor_save_map"), QStringLiteral("保存地图")},
            {QStringLiteral("editor_true_save"), QStringLiteral("原版编辑器保存")}};
        title = labels.value(toolName, title.size() > 40 || title.contains(QLatin1Char('\n')) || title.isEmpty() ? toolName : title);
    }
    if (title.isEmpty()) title = toolName;
    if (title.isEmpty()) title = event.value(QStringLiteral("target")).toString();
    return title.isEmpty() ? QStringLiteral("工具调用") : title.simplified();
}

class PromptEdit final : public QPlainTextEdit {
public:
    explicit PromptEdit(QWidget* parent) : QPlainTextEdit(parent) {}
    std::function<void()> submit;
    std::function<void()> queue;
    std::function<void()> resized;

protected:
    void inputMethodEvent(QInputMethodEvent* event) override {
        composing_ = !event->preeditString().isEmpty();
        QPlainTextEdit::inputMethodEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        // Enter 发送、Shift+Enter 换行、Ctrl+Enter 排队（空闲时直接发送）；输入法组字时交给输入法。
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (enter && composing_) { event->accept(); return; } // 组字中的 Enter 归输入法，不换行也不发送。
        if (enter) {
            if (modifiers == Qt::ShiftModifier) { insertPlainText(QStringLiteral("\n")); event->accept(); return; }
            if (modifiers == Qt::ControlModifier) { if (queue) queue(); event->accept(); return; }
            if (modifiers == Qt::NoModifier) { if (submit) submit(); event->accept(); return; }
        }
        QPlainTextEdit::keyPressEvent(event);
    }
    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        if (resized) resized();
    }
private:
    bool composing_ = false;
};

bool IsExactCount(const QJsonValue& value) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble(-1.0);
    return number >= 0.0 && number <= 9007199254740991.0 && number == std::floor(number);
}
qint64 Count(const QJsonValue& value) { return IsExactCount(value) ? static_cast<qint64>(value.toDouble()) : 0; }
qint64 AddCount(qint64 total, qint64 increment) {
    return increment > std::numeric_limits<qint64>::max() - total ? std::numeric_limits<qint64>::max() : total + increment;
}
QString DurationText(qint64 milliseconds) {
    if (milliseconds < 1000) return QStringLiteral("%1 ms").arg(milliseconds);
    if (milliseconds < 60000) return QStringLiteral("%1 s").arg(static_cast<double>(milliseconds) / 1000.0, 0, 'f', 1);
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
QString HistoryText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const auto& value : content.toArray()) {
        const auto block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) parts.append(block.value(QStringLiteral("text")).toString());
    }
    return parts.join(QLatin1Char('\n'));
}

QFrame* MakeInspectorDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("agentInspectorDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedHeight(1);
    return divider;
}

} // namespace

struct AgentWindow::Private {
    struct Tool {
        QElapsedTimer elapsed;
        QJsonObject event;
        QString state;
        QString duration;
        bool finished = false;
    };
    struct ChangeRecord {
        QString kind;
        QString objectId;
        QString title;
    };
    struct ContextLegendRow {
        QLabel* tokens = nullptr;
        QLabel* percent = nullptr;
    };
    explicit Private(AgentWindow* owner) : q(owner) {}
    AgentWindow* q;
    AgentSettings settings;
    PresentationMode mode = PresentationMode::Detached;

    // 三栏桌面骨架：窄宽度或内嵌侧栏模式下自动折叠左右两栏，保留中央对话与输入胶囊。
    QFrame* leftSidebar = nullptr;
    QFrame* centerWorkspace = nullptr;
    QFrame* rightInspector = nullptr;
    bool leftRailCollapsed = false;
    bool rightRailCollapsed = false;
    QToolButton* expandLeftRail = nullptr;
    QToolButton* toggleRightRail = nullptr;

    // 左侧会话与导航栏
    QPushButton* sidebarNew = nullptr;
    QLineEdit* sidebarSearch = nullptr;
    UiKit::ElidedLabel* sidebarMapTitle = nullptr;
    QLabel* sidebarSessionCount = nullptr;
    QListWidget* sidebarSessionList = nullptr;
    QLabel* sidebarStatusDot = nullptr;
    UiKit::ElidedLabel* sidebarAccount = nullptr;

    // 右侧环境与上下文检查栏
    QLabel* envModeValue = nullptr;
    UiKit::ElidedLabel* envMapValue = nullptr;
    UiKit::ElidedLabel* envIdentityValue = nullptr;
    QToolButton* envPermissionBtn = nullptr;
    QToolButton* envFollowBtn = nullptr;
    QLabel* inspectorContextSource = nullptr;
    QLabel* inspectorContextPercent = nullptr;
    QLabel* inspectorContextTokens = nullptr;
    ContextSegmentBar* inspectorSegmentBar = nullptr;
    std::array<ContextLegendRow, 4> contextRows{};
    QLabel* inspectorBuiltinDot = nullptr;
    QLabel* inspectorBuiltinState = nullptr;
    QLabel* inspectorMcpDot = nullptr;
    QPushButton* inspectorMcpBtn = nullptr;
    QLabel* inspectorDiffSummary = nullptr;
    QWidget* inspectorChangesContainer = nullptr;
    QVBoxLayout* inspectorChangesLayout = nullptr;
    QLabel* inspectorToolsBadge = nullptr;
    QWidget* inspectorToolsContainer = nullptr;
    QVBoxLayout* inspectorToolsLayout = nullptr;
    QList<ChangeRecord> recentChanges;
    QStringList toolOrder;

    // 中央会话与输入区（完整保留测试与浮层契约）
    QToolButton* back = nullptr;
    QToolButton* newConversation = nullptr;
    QToolButton* sessionsButton = nullptr;
    QToolButton* followButton = nullptr;
    UiKit::PopoverSurface* sessionsPopover = nullptr;
    QListWidget* sessionList = nullptr;
    QPushButton* renameSession = nullptr;
    QPushButton* archiveSession = nullptr;
    QWidget* renameRow = nullptr;
    QLineEdit* sessionTitle = nullptr;
    QJsonArray sessions;
    QString selectedSession;
    QLabel* mapSummary = nullptr;
    std::array<QAction*, 3> modeActions{};
    UiKit::ElidedLabel* map = nullptr;
    QLabel* historyNotice = nullptr;
    AgentConversationView* conversation = nullptr;
    QWidget* composerAxis = nullptr;
    QWidget* activity = nullptr;
    QLabel* activityDot = nullptr;
    QWidget* changeSummary = nullptr;
    QWidget* outputSummary = nullptr;
    QWidget* attachmentRow = nullptr;
    QHBoxLayout* attachmentLayout = nullptr;
    QStringList attachments;
    // 运行中按 Ctrl+Enter 排队的消息：本轮正常结束后依次发送；失败或停止时暂停等待作者继续。
    struct QueuedPrompt { QString request, shown; };
    QList<QueuedPrompt> queued;
    QLabel* queueNotice = nullptr;
    PromptEdit* prompt = nullptr;
    QToolButton* attach = nullptr;
    QComboBox* models = nullptr;
    QToolButton* addModel = nullptr;
    QLabel* context = nullptr;
    ContextRing* ring = nullptr;
    UiKit::PopoverSurface* contextPopover = nullptr;
    QLabel* contextHeading = nullptr;
    QLabel* contextSourceBadge = nullptr;
    ContextSegmentBar* popoverSegmentBar = nullptr;
    QLabel* contextFraction = nullptr;
    QLabel* contextTokens = nullptr;
    QLabel* contextBreakdown = nullptr;
    QLabel* contextSource = nullptr;
    QJsonObject actualUsage;
    QToolButton* permissions = nullptr;
    QMenu* permissionMenu = nullptr;
    std::array<PermissionOption*, 4> permissionOptions{};
    QPushButton* send = nullptr;
    QPushButton* stop = nullptr;
    QLabel* status = nullptr;
    QLabel* elapsed = nullptr;
    QLabel* usage = nullptr;
    std::array<AnimatedCounter*, 4> counters{};
    QWidget* changeDetails = nullptr;
    bool compactSummary = false;
    AnimatedCounter* output = nullptr;
    std::array<qint64, 4> totals{};
    qint64 outputCharacters = 0;
    QHash<QString, Tool> tools;
    QTimer flushTimer;
    QTimer elapsedTimer;
    QElapsedTimer runElapsed;
    QString pending;
    bool busy = false;
    bool assistantOpen = false;
    bool stopped = false;
    bool resizingPrompt = false;
    bool applyingStyleSheet = false;
    QString appliedStyleSheet;

    void Build();
    void BuildLeftSidebar(QHBoxLayout* root);
    void BuildRightInspector(QHBoxLayout* root);
    void ApplyAgentStyleSheet();
    void ModelChanged();
    void UpdateAvailability();
    void UpdateGeometry();
    void UpdatePromptHeight();
    void UpdateSummary();
    void UpdatePermissions();
    void UpdateSendIcons();
    void UpdateInspectorEnvironment();
    void UpdateInspectorChanges();
    void UpdateInspectorTools();
    void FilterSidebarSessions();
    void ChooseAttachments();
    void AddAttachment(const QString& path);
    void RemoveAttachment(const QString& path);
    void Submit();
    void Queue();
    bool TakePrompt(QueuedPrompt& out);
    void Dispatch(const QueuedPrompt& prompt);
    void UpdateQueue(bool paused = false);
    void ContinueQueue(bool resumable);
    void SetStatus(const QString& text, bool failed = false);
    void BeginAssistant();
    void Flush();
    void UpdateTool(const QJsonObject& event, const QString& type);
    void UpdateChanges(const QJsonObject& event);
    void UpdateElapsed();
    void RestoreHistory(const QJsonObject& event);
    void BuildSessionPopover();
    void UpdateSessions();
    void BuildContextPopover();
    void UpdateContext();
};

void AgentWindow::Private::BuildLeftSidebar(QHBoxLayout* root) {
    leftSidebar = new QFrame(q);
    leftSidebar->setObjectName(QStringLiteral("agentLeftSidebar"));
    leftSidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* railLayout = new QVBoxLayout(leftSidebar);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(0);

    auto* railHeader = new QFrame(leftSidebar);
    railHeader->setObjectName(QStringLiteral("agentLeftRailHeader"));
    railHeader->setFixedHeight(42);
    auto* railHeaderLayout = new QHBoxLayout(railHeader);
    railHeaderLayout->setContentsMargins(12, 0, 8, 0);
    railHeaderLayout->setSpacing(6);
    auto* brandIcon = new QLabel(railHeader);
    brandIcon->setFixedSize(16, 16);
    brandIcon->setAlignment(Qt::AlignCenter);
    brandIcon->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/sparkles"), QColor(110, 231, 183)).pixmap(15, 15));
    railHeaderLayout->addWidget(brandIcon);
    auto* brandTitle = new QLabel(QStringLiteral("AI 工作区"), railHeader);
    brandTitle->setObjectName(QStringLiteral("agentRailTitle"));
    railHeaderLayout->addWidget(brandTitle, 1);
    auto* collapseLeft = new QToolButton(railHeader);
    collapseLeft->setObjectName(QStringLiteral("agentCollapseLeftRail"));
    collapseLeft->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/columns-3")));
    collapseLeft->setIconSize(QSize(15, 15));
    collapseLeft->setToolTip(QStringLiteral("收起会话栏"));
    collapseLeft->setAccessibleName(QStringLiteral("收起会话栏"));
    collapseLeft->setCursor(Qt::PointingHandCursor);
    QObject::connect(collapseLeft, &QToolButton::clicked, q, [this] {
        leftRailCollapsed = true;
        UpdateGeometry();
    });
    railHeaderLayout->addWidget(collapseLeft);
    railLayout->addWidget(railHeader);

    auto* navBody = new QWidget(leftSidebar);
    auto* navLayout = new QVBoxLayout(navBody);
    navLayout->setContentsMargins(10, 10, 10, 8);
    navLayout->setSpacing(8);

    sidebarNew = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/plus")), QStringLiteral("新建会话"), navBody);
    sidebarNew->setObjectName(QStringLiteral("agentSidebarNew"));
    sidebarNew->setIconSize(QSize(15, 15));
    sidebarNew->setFixedHeight(32);
    sidebarNew->setCursor(Qt::PointingHandCursor);
    QObject::connect(sidebarNew, &QPushButton::clicked, q, [this] {
        if (!busy) emit q->NewConversationRequested();
    });
    navLayout->addWidget(sidebarNew);

    sidebarSearch = new QLineEdit(navBody);
    sidebarSearch->setObjectName(QStringLiteral("agentSidebarSearch"));
    sidebarSearch->setPlaceholderText(QStringLiteral("搜索会话…"));
    sidebarSearch->setClearButtonEnabled(true);
    sidebarSearch->setFixedHeight(28);
    UiKit::InstallSearchIcon(sidebarSearch);
    QObject::connect(sidebarSearch, &QLineEdit::textChanged, q, [this] { FilterSidebarSessions(); });
    navLayout->addWidget(sidebarSearch);

    navLayout->addSpacing(4);
    auto* groupHeader = new QHBoxLayout;
    groupHeader->setContentsMargins(4, 2, 4, 0);
    groupHeader->setSpacing(5);
    auto* mapIcon = new QLabel(navBody);
    mapIcon->setFixedSize(13, 13);
    mapIcon->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/map")).pixmap(13, 13));
    groupHeader->addWidget(mapIcon);
    sidebarMapTitle = new UiKit::ElidedLabel(QStringLiteral("当前地图"), navBody);
    sidebarMapTitle->setObjectName(QStringLiteral("agentSidebarGroupTitle"));
    sidebarMapTitle->setMinimumWidth(0);
    sidebarMapTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    groupHeader->addWidget(sidebarMapTitle, 1);
    sidebarSessionCount = new QLabel(QStringLiteral("0"), navBody);
    sidebarSessionCount->setObjectName(QStringLiteral("agentSidebarGroupCount"));
    sidebarSessionCount->setFont(UiKit::MonoFont(11));
    groupHeader->addWidget(sidebarSessionCount);
    navLayout->addLayout(groupHeader);

    sidebarSessionList = new QListWidget(navBody);
    sidebarSessionList->setObjectName(QStringLiteral("agentSidebarSessionList"));
    sidebarSessionList->setAccessibleName(QStringLiteral("侧栏会话列表"));
    sidebarSessionList->setFrameShape(QFrame::NoFrame);
    sidebarSessionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarSessionList->setSpacing(2);
    sidebarSessionList->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(sidebarSessionList, &QListWidget::itemClicked, q, [this](QListWidgetItem* item) {
        if (busy || !item) return;
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty() && id != selectedSession) emit q->ConversationSwitchRequested(id);
    });
    QObject::connect(sidebarSessionList, &QListWidget::itemActivated, q, [this](QListWidgetItem* item) {
        if (busy || !item) return;
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty() && id != selectedSession) emit q->ConversationSwitchRequested(id);
    });
    QObject::connect(sidebarSessionList, &QWidget::customContextMenuRequested, q, [this](const QPoint& pos) {
        if (busy) return;
        QListWidgetItem* item = sidebarSessionList->itemAt(pos);
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) return;
        const QString currentTitle = item->data(Qt::UserRole + 1).toString();
        QMenu menu(sidebarSessionList);
        QAction* renameAction = menu.addAction(QStringLiteral("重命名会话…"));
        QAction* archiveAction = menu.addAction(QStringLiteral("移至归档…"));
        QAction* picked = menu.exec(sidebarSessionList->viewport()->mapToGlobal(pos));
        if (picked == renameAction) {
            bool ok = false;
            const QString updated = QInputDialog::getText(q, QStringLiteral("重命名会话"),
                QStringLiteral("会话标题："), QLineEdit::Normal, currentTitle, &ok).trimmed();
            if (ok && !updated.isEmpty() && updated != currentTitle) emit q->ConversationRenameRequested(id, updated);
        } else if (picked == archiveAction) {
            if (QMessageBox::question(q, QStringLiteral("删除会话"),
                QStringLiteral("将“%1”移到本地归档？完整记录会保留。").arg(currentTitle),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
                emit q->ConversationArchiveRequested(id);
            }
        }
    });
    navLayout->addWidget(sidebarSessionList, 1);
    railLayout->addWidget(navBody, 1);

    auto* footer = new QFrame(leftSidebar);
    footer->setObjectName(QStringLiteral("agentSidebarFooter"));
    footer->setFixedHeight(40);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 0, 8, 0);
    footerLayout->setSpacing(8);
    sidebarStatusDot = new QLabel(footer);
    sidebarStatusDot->setObjectName(QStringLiteral("agentSidebarStatusDot"));
    sidebarStatusDot->setFixedSize(7, 7);
    footerLayout->addWidget(sidebarStatusDot);
    sidebarAccount = new UiKit::ElidedLabel(QStringLiteral("未启用 AI"), footer);
    sidebarAccount->setObjectName(QStringLiteral("agentSidebarAccount"));
    sidebarAccount->setMinimumWidth(0);
    sidebarAccount->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footerLayout->addWidget(sidebarAccount, 1);
    auto* footerSettings = new QToolButton(footer);
    footerSettings->setObjectName(QStringLiteral("agentSidebarSettings"));
    footerSettings->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/settings-2")));
    footerSettings->setIconSize(QSize(15, 15));
    footerSettings->setToolTip(QStringLiteral("AI 与 MCP 设置"));
    footerSettings->setCursor(Qt::PointingHandCursor);
    QObject::connect(footerSettings, &QToolButton::clicked, q, &AgentWindow::SettingsRequested);
    footerLayout->addWidget(footerSettings);
    railLayout->addWidget(footer);

    root->addWidget(leftSidebar);
}

void AgentWindow::Private::BuildRightInspector(QHBoxLayout* root) {
    rightInspector = new QFrame(q);
    rightInspector->setObjectName(QStringLiteral("agentRightInspector"));
    rightInspector->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto* railLayout = new QVBoxLayout(rightInspector);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(0);

    auto* railHeader = new QFrame(rightInspector);
    railHeader->setObjectName(QStringLiteral("agentRightRailHeader"));
    railHeader->setFixedHeight(42);
    auto* railHeaderLayout = new QHBoxLayout(railHeader);
    railHeaderLayout->setContentsMargins(14, 0, 8, 0);
    railHeaderLayout->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("状态与上下文"), railHeader);
    title->setObjectName(QStringLiteral("agentRailTitle"));
    railHeaderLayout->addWidget(title, 1);
    auto* collapseRight = new QToolButton(railHeader);
    collapseRight->setObjectName(QStringLiteral("agentCollapseRightRail"));
    collapseRight->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/panel-right")));
    collapseRight->setIconSize(QSize(15, 15));
    collapseRight->setToolTip(QStringLiteral("收起检查栏"));
    collapseRight->setAccessibleName(QStringLiteral("收起检查栏"));
    collapseRight->setCursor(Qt::PointingHandCursor);
    QObject::connect(collapseRight, &QToolButton::clicked, q, [this] {
        rightRailCollapsed = true;
        UpdateGeometry();
    });
    railHeaderLayout->addWidget(collapseRight);
    railLayout->addWidget(railHeader);

    auto* scroll = new QScrollArea(rightInspector);
    scroll->setObjectName(QStringLiteral("agentInspectorScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("agentInspectorBody"));
    auto* sections = new QVBoxLayout(body);
    sections->setContentsMargins(14, 12, 14, 16);
    sections->setSpacing(12);

    // 1. 环境
    {
        auto* heading = new QLabel(QStringLiteral("环境"), body);
        heading->setObjectName(QStringLiteral("agentInspectorHeading"));
        sections->addWidget(heading);

        auto* envBox = new QVBoxLayout;
        envBox->setContentsMargins(0, 0, 0, 0);
        envBox->setSpacing(7);

        auto addIconRow = [&](const QString& iconName, QWidget* primary, QWidget* trailing = nullptr) {
            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(8);
            auto* icon = new QLabel(body);
            icon->setFixedSize(14, 14);
            icon->setAlignment(Qt::AlignCenter);
            icon->setPixmap(UiKit::SvgIcon(iconName).pixmap(14, 14));
            row->addWidget(icon);
            row->addWidget(primary, 1);
            if (trailing) row->addWidget(trailing);
            envBox->addLayout(row);
        };

        auto* hostLabel = new QLabel(QStringLiteral("本机编辑器"), body);
        hostLabel->setObjectName(QStringLiteral("agentInspectorLabel"));
        envModeValue = new QLabel(QStringLiteral("独立窗口"), body);
        envModeValue->setObjectName(QStringLiteral("agentInspectorMuted"));
        addIconRow(QStringLiteral("lucide/app-window"), hostLabel, envModeValue);

        envMapValue = new UiKit::ElidedLabel(QStringLiteral("新会话"), body);
        envMapValue->setObjectName(QStringLiteral("agentInspectorLabel"));
        envMapValue->setMinimumWidth(0);
        envMapValue->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        addIconRow(QStringLiteral("lucide/map"), envMapValue);

        envIdentityValue = new UiKit::ElidedLabel(QStringLiteral("未绑定地图修订"), body);
        envIdentityValue->setObjectName(QStringLiteral("agentInspectorMuted"));
        envIdentityValue->setFont(UiKit::MonoFont(11));
        envIdentityValue->setMinimumWidth(0);
        envIdentityValue->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        addIconRow(QStringLiteral("lucide/git-branch"), envIdentityValue);

        auto* permLabel = new QLabel(QStringLiteral("访问权限"), body);
        permLabel->setObjectName(QStringLiteral("agentInspectorLabel"));
        envPermissionBtn = new QToolButton(body);
        envPermissionBtn->setObjectName(QStringLiteral("agentEnvPermissionPill"));
        envPermissionBtn->setPopupMode(QToolButton::InstantPopup);
        envPermissionBtn->setCursor(Qt::PointingHandCursor);
        envPermissionBtn->setFixedHeight(22);
        addIconRow(QStringLiteral("lucide/lock"), permLabel, envPermissionBtn);

        auto* followLabel = new QLabel(QStringLiteral("跟随修改"), body);
        followLabel->setObjectName(QStringLiteral("agentInspectorLabel"));
        envFollowBtn = new QToolButton(body);
        envFollowBtn->setObjectName(QStringLiteral("agentEnvFollowSwitch"));
        envFollowBtn->setCheckable(true);
        envFollowBtn->setCursor(Qt::PointingHandCursor);
        envFollowBtn->setFixedHeight(22);
        QObject::connect(envFollowBtn, &QToolButton::clicked, q, [this](bool checked) {
            emit q->FollowChangesChangeRequested(checked);
        });
        addIconRow(QStringLiteral("lucide/eye"), followLabel, envFollowBtn);

        sections->addLayout(envBox);
    }

    sections->addWidget(MakeInspectorDivider(body));

    // 2. 上下文
    {
        auto* headRow = new QHBoxLayout;
        headRow->setContentsMargins(0, 0, 0, 0);
        auto* heading = new QLabel(QStringLiteral("上下文"), body);
        heading->setObjectName(QStringLiteral("agentInspectorHeading"));
        headRow->addWidget(heading);
        headRow->addStretch();
        inspectorContextSource = new QLabel(body);
        inspectorContextSource->setObjectName(QStringLiteral("agentInspectorMuted"));
        headRow->addWidget(inspectorContextSource);
        sections->addLayout(headRow);

        auto* metricRow = new QHBoxLayout;
        metricRow->setContentsMargins(0, 0, 0, 0);
        inspectorContextPercent = new QLabel(QStringLiteral("暂无用量"), body);
        inspectorContextPercent->setObjectName(QStringLiteral("agentInspectorPrimaryValue"));
        metricRow->addWidget(inspectorContextPercent);
        metricRow->addStretch();
        inspectorContextTokens = new QLabel(QStringLiteral("—"), body);
        inspectorContextTokens->setObjectName(QStringLiteral("agentInspectorMuted"));
        inspectorContextTokens->setFont(UiKit::MonoFont(11));
        metricRow->addWidget(inspectorContextTokens);
        sections->addLayout(metricRow);

        inspectorSegmentBar = new ContextSegmentBar(body);
        sections->addWidget(inspectorSegmentBar);

        struct LegendSpec {
            const char* color;
            const char* label;
        };
        constexpr std::array<LegendSpec, 4> kSpecs{{
            {"#60a5fa", "输入上下文"},
            {"#38bdf8", "缓存命中"},
            {"#c084fc", "模型输出"},
            {"#555963", "剩余空间"},
        }};
        auto* legendBox = new QVBoxLayout;
        legendBox->setContentsMargins(0, 2, 0, 0);
        legendBox->setSpacing(5);
        for (std::size_t i = 0; i < kSpecs.size(); ++i) {
            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(7);
            auto* swatch = new QFrame(body);
            swatch->setFixedSize(7, 7);
            swatch->setStyleSheet(QStringLiteral("background:%1; border-radius:2px; border:0;")
                .arg(QString::fromLatin1(kSpecs[i].color)));
            row->addWidget(swatch);
            auto* name = new QLabel(QString::fromUtf8(kSpecs[i].label), body);
            name->setObjectName(QStringLiteral("agentInspectorMuted"));
            row->addWidget(name, 1);
            auto* tok = new QLabel(QStringLiteral("—"), body);
            tok->setObjectName(QStringLiteral("agentInspectorMuted"));
            tok->setFont(UiKit::MonoFont(11));
            tok->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(tok);
            auto* pct = new QLabel(QStringLiteral("—"), body);
            pct->setObjectName(QStringLiteral("agentInspectorLabel"));
            pct->setFont(UiKit::MonoFont(11));
            pct->setFixedWidth(40);
            pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            row->addWidget(pct);
            contextRows[i] = {tok, pct};
            legendBox->addLayout(row);
        }
        sections->addLayout(legendBox);
    }

    sections->addWidget(MakeInspectorDivider(body));

    // 3. 连接器
    {
        auto* heading = new QLabel(QStringLiteral("连接器"), body);
        heading->setObjectName(QStringLiteral("agentInspectorHeading"));
        sections->addWidget(heading);

        auto* connBox = new QVBoxLayout;
        connBox->setContentsMargins(0, 0, 0, 0);
        connBox->setSpacing(7);

        auto* row1 = new QHBoxLayout;
        row1->setContentsMargins(0, 0, 0, 0);
        row1->setSpacing(8);
        inspectorBuiltinDot = new QLabel(body);
        inspectorBuiltinDot->setObjectName(QStringLiteral("agentConnectorDot"));
        inspectorBuiltinDot->setFixedSize(7, 7);
        row1->addWidget(inspectorBuiltinDot);
        auto* builtinTitle = new QLabel(QStringLiteral("编辑器内置工具"), body);
        builtinTitle->setObjectName(QStringLiteral("agentInspectorLabel"));
        row1->addWidget(builtinTitle, 1);
        inspectorBuiltinState = new QLabel(QStringLiteral("已就绪"), body);
        inspectorBuiltinState->setObjectName(QStringLiteral("agentInspectorMuted"));
        row1->addWidget(inspectorBuiltinState);
        connBox->addLayout(row1);

        auto* row2 = new QHBoxLayout;
        row2->setContentsMargins(0, 0, 0, 0);
        row2->setSpacing(8);
        inspectorMcpDot = new QLabel(body);
        inspectorMcpDot->setObjectName(QStringLiteral("agentConnectorDot"));
        inspectorMcpDot->setFixedSize(7, 7);
        row2->addWidget(inspectorMcpDot);
        auto* mcpTitle = new QLabel(QStringLiteral("MCP 本地服务"), body);
        mcpTitle->setObjectName(QStringLiteral("agentInspectorLabel"));
        row2->addWidget(mcpTitle, 1);
        inspectorMcpBtn = new QPushButton(QStringLiteral("配置"), body);
        inspectorMcpBtn->setObjectName(QStringLiteral("agentInspectorLinkBtn"));
        inspectorMcpBtn->setCursor(Qt::PointingHandCursor);
        QObject::connect(inspectorMcpBtn, &QPushButton::clicked, q, &AgentWindow::SettingsRequested);
        row2->addWidget(inspectorMcpBtn);
        connBox->addLayout(row2);

        sections->addLayout(connBox);
    }

    sections->addWidget(MakeInspectorDivider(body));

    // 4. 更改
    {
        auto* headRow = new QHBoxLayout;
        headRow->setContentsMargins(0, 0, 0, 0);
        auto* heading = new QLabel(QStringLiteral("更改"), body);
        heading->setObjectName(QStringLiteral("agentInspectorHeading"));
        headRow->addWidget(heading);
        headRow->addStretch();
        inspectorDiffSummary = new QLabel(QStringLiteral("无改动"), body);
        inspectorDiffSummary->setObjectName(QStringLiteral("agentInspectorMuted"));
        inspectorDiffSummary->setFont(UiKit::MonoFont(11));
        headRow->addWidget(inspectorDiffSummary);
        sections->addLayout(headRow);

        inspectorChangesContainer = new QWidget(body);
        inspectorChangesContainer->setObjectName(QStringLiteral("agentInspectorListContainer"));
        inspectorChangesLayout = new QVBoxLayout(inspectorChangesContainer);
        inspectorChangesLayout->setContentsMargins(0, 0, 0, 0);
        inspectorChangesLayout->setSpacing(5);
        sections->addWidget(inspectorChangesContainer);
    }

    sections->addWidget(MakeInspectorDivider(body));

    // 5. 工具活动
    {
        auto* headRow = new QHBoxLayout;
        headRow->setContentsMargins(0, 0, 0, 0);
        auto* heading = new QLabel(QStringLiteral("工具活动"), body);
        heading->setObjectName(QStringLiteral("agentInspectorHeading"));
        headRow->addWidget(heading);
        headRow->addStretch();
        inspectorToolsBadge = new QLabel(QStringLiteral("空闲"), body);
        inspectorToolsBadge->setObjectName(QStringLiteral("agentInspectorMuted"));
        inspectorToolsBadge->setFont(UiKit::MonoFont(11));
        headRow->addWidget(inspectorToolsBadge);
        sections->addLayout(headRow);

        inspectorToolsContainer = new QWidget(body);
        inspectorToolsContainer->setObjectName(QStringLiteral("agentInspectorListContainer"));
        inspectorToolsLayout = new QVBoxLayout(inspectorToolsContainer);
        inspectorToolsLayout->setContentsMargins(0, 0, 0, 0);
        inspectorToolsLayout->setSpacing(5);
        sections->addWidget(inspectorToolsContainer);
    }

    sections->addStretch(1);
    scroll->setWidget(body);
    railLayout->addWidget(scroll, 1);

    root->addWidget(rightInspector);
}

void AgentWindow::Private::Build() {
    q->setObjectName(QStringLiteral("agentWindow"));
    q->setWindowTitle(QStringLiteral("AI 工作区 — Audastra"));
    q->setAttribute(Qt::WA_DeleteOnClose, false);
    q->setMinimumSize(360, 420);
    q->resize(1240, 820);

    auto* root = new QHBoxLayout(q);
    root->setSizeConstraint(QLayout::SetNoConstraint);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    BuildLeftSidebar(root);

    centerWorkspace = new QFrame(q);
    centerWorkspace->setObjectName(QStringLiteral("agentCenterWorkspace"));
    auto* centerLayout = new QVBoxLayout(centerWorkspace);
    centerLayout->setSizeConstraint(QLayout::SetNoConstraint);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);

    auto* header = new QFrame(centerWorkspace);
    header->setObjectName(QStringLiteral("agentConversationHeader"));
    header->setFixedHeight(42);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 5, 12, 5);
    headerLayout->setSpacing(4);

    expandLeftRail = new QToolButton(header);
    expandLeftRail->setObjectName(QStringLiteral("agentExpandLeftRail"));
    expandLeftRail->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/columns-3")));
    expandLeftRail->setIconSize(QSize(16, 16));
    expandLeftRail->setToolTip(QStringLiteral("展开会话栏"));
    expandLeftRail->setAccessibleName(QStringLiteral("展开会话栏"));
    expandLeftRail->setCursor(Qt::PointingHandCursor);
    expandLeftRail->hide();
    QObject::connect(expandLeftRail, &QToolButton::clicked, q, [this] {
        leftRailCollapsed = false;
        UpdateGeometry();
    });
    headerLayout->addWidget(expandLeftRail);

    back = new QToolButton(header);
    back->setObjectName(QStringLiteral("agentBack"));
    back->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/arrow-left")));
    back->setIconSize(QSize(17, 17));
    back->setToolTip(QStringLiteral("返回地图编辑"));
    back->setAccessibleName(QStringLiteral("返回地图编辑"));
    back->hide();
    QObject::connect(back, &QToolButton::clicked, q, &AgentWindow::BackRequested);
    headerLayout->addWidget(back);

    auto* title = new QLabel(QStringLiteral("AI"), header);
    title->setObjectName(QStringLiteral("agentHeaderTitle"));
    headerLayout->addWidget(title);

    map = new UiKit::ElidedLabel(QStringLiteral("新会话"), header);
    map->setObjectName(QStringLiteral("agentActiveMap"));
    map->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    map->setMinimumWidth(0);
    headerLayout->addWidget(map, 1);

    sessionsButton = new QToolButton(header);
    sessionsButton->setObjectName(QStringLiteral("agentConversations"));
    sessionsButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/layers")));
    sessionsButton->setIconSize(QSize(16, 16));
    sessionsButton->setToolTip(QStringLiteral("会话"));
    sessionsButton->setAccessibleName(QStringLiteral("本地图会话"));
    QObject::connect(sessionsButton, &QToolButton::clicked, q, [this] {
        if (!busy) sessionsPopover->ToggleAnchored(sessionsButton, UiKit::PopoverSurface::PreferredSide::Below, Qt::AlignRight);
    });
    headerLayout->addWidget(sessionsButton);

    followButton = new QToolButton(header);
    followButton->setObjectName(QStringLiteral("agentFollowChanges"));
    followButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/eye")));
    followButton->setIconSize(QSize(16, 16));
    followButton->setCheckable(true);
    followButton->setToolTip(QStringLiteral("跟随修改"));
    followButton->setAccessibleName(QStringLiteral("跟随修改"));
    QObject::connect(followButton, &QToolButton::clicked, q, [this](bool checked) { emit q->FollowChangesChangeRequested(checked); });
    headerLayout->addWidget(followButton);

    newConversation = new QToolButton(header);
    newConversation->setObjectName(QStringLiteral("agentNewConversation"));
    newConversation->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    newConversation->setIconSize(QSize(16, 16));
    newConversation->setToolTip(QStringLiteral("新会话"));
    newConversation->setAccessibleName(QStringLiteral("新建 AI 会话"));
    QObject::connect(newConversation, &QToolButton::clicked, q, &AgentWindow::NewConversationRequested);
    headerLayout->addWidget(newConversation);

    auto* presentation = new QToolButton(header);
    presentation->setObjectName(QStringLiteral("agentPresentation"));
    presentation->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/square")));
    presentation->setIconSize(QSize(15, 15));
    presentation->setToolTip(QStringLiteral("显示位置"));
    presentation->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(presentation);
    const std::array<QString, 3> labels{QStringLiteral("整页"), QStringLiteral("侧栏"), QStringLiteral("独立窗口")};
    const std::array<PresentationMode, 3> modes{PresentationMode::Page, PresentationMode::Sidebar, PresentationMode::Detached};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        QAction* action = menu->addAction(labels[i]);
        action->setCheckable(true);
        modeActions[i] = action;
        const PresentationMode requested = modes[i];
        QObject::connect(action, &QAction::triggered, q, [this, requested] { emit q->PresentationModeRequested(requested); });
    }
    presentation->setMenu(menu);
    headerLayout->addWidget(presentation);

    auto* settingsButton = new QToolButton(header);
    settingsButton->setObjectName(QStringLiteral("agentSettings"));
    settingsButton->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/settings-2")));
    settingsButton->setIconSize(QSize(16, 16));
    settingsButton->setToolTip(QStringLiteral("AI 与 MCP 设置"));
    settingsButton->setAccessibleName(QStringLiteral("AI 与 MCP 设置"));
    QObject::connect(settingsButton, &QToolButton::clicked, q, &AgentWindow::SettingsRequested);
    headerLayout->addWidget(settingsButton);

    toggleRightRail = new QToolButton(header);
    toggleRightRail->setObjectName(QStringLiteral("agentToggleRightRail"));
    toggleRightRail->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/panel-right")));
    toggleRightRail->setIconSize(QSize(16, 16));
    toggleRightRail->setCheckable(true);
    toggleRightRail->setToolTip(QStringLiteral("状态与上下文检查栏"));
    toggleRightRail->setAccessibleName(QStringLiteral("状态与上下文检查栏"));
    toggleRightRail->setCursor(Qt::PointingHandCursor);
    toggleRightRail->hide();
    QObject::connect(toggleRightRail, &QToolButton::clicked, q, [this](bool checked) {
        rightRailCollapsed = !checked;
        UpdateGeometry();
    });
    headerLayout->addWidget(toggleRightRail);

    centerLayout->addWidget(header);

    mapSummary = new QLabel(centerWorkspace);
    mapSummary->setObjectName(QStringLiteral("agentMapSummary"));
    mapSummary->setContentsMargins(16, 5, 16, 5);
    mapSummary->setWordWrap(true);
    mapSummary->hide();
    centerLayout->addWidget(mapSummary);

    historyNotice = new QLabel(centerWorkspace);
    historyNotice->setObjectName(QStringLiteral("agentHistoryNotice"));
    historyNotice->setWordWrap(true);
    historyNotice->setContentsMargins(16, 6, 16, 6);
    historyNotice->hide();
    centerLayout->addWidget(historyNotice);

    conversation = new AgentConversationView(centerWorkspace);
    QObject::connect(conversation, &AgentConversationView::NavigateRequested, q, &AgentWindow::NavigateRequested);
    QObject::connect(conversation, &AgentConversationView::SuggestionChosen, q, [this](const QString& text) {
        if (busy) return;
        prompt->setPlainText(text);
        QTextCursor cursor = prompt->textCursor();
        cursor.movePosition(QTextCursor::End);
        prompt->setTextCursor(cursor);
        prompt->setFocus();
    });
    centerLayout->addWidget(conversation, 1);

    auto* composerContainer = new QWidget(centerWorkspace);
    composerContainer->setObjectName(QStringLiteral("agentComposerContainer"));
    auto* centered = new QHBoxLayout(composerContainer);
    centered->setContentsMargins(16, 0, 16, 14);
    composerAxis = new QWidget(composerContainer);
    composerAxis->setObjectName(QStringLiteral("agentComposerAxis"));
    composerAxis->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* axis = new QVBoxLayout(composerAxis);
    axis->setSizeConstraint(QLayout::SetNoConstraint);
    axis->setContentsMargins(0, 0, 0, 0);
    axis->setSpacing(6);
    centered->addStretch();
    centered->addWidget(composerAxis, 1);
    centered->addStretch();

    // 输入框上方的活动行：运行状态、耗时，右侧为本轮改动计数。空闲且无改动时隐藏。
    auto* summary = new QWidget(composerAxis);
    summary->setObjectName(QStringLiteral("agentChangeSummary"));
    activity = summary;
    auto* summaryLayout = new QHBoxLayout(summary);
    summaryLayout->setContentsMargins(12, 0, 12, 0);
    summaryLayout->setSpacing(6);
    activityDot = new QLabel(summary);
    activityDot->setObjectName(QStringLiteral("agentActivityDot"));
    activityDot->setFixedSize(7, 7);
    summaryLayout->addWidget(activityDot);
    status = new QLabel(QStringLiteral("就绪"), summary);
    status->setObjectName(QStringLiteral("agentStatus"));
    summaryLayout->addWidget(status);
    elapsed = new QLabel(QStringLiteral("—"), summary);
    elapsed->setObjectName(QStringLiteral("agentElapsed"));
    elapsed->setFont(UiKit::MonoFont(12));
    summaryLayout->addWidget(elapsed);
    summaryLayout->addStretch();
    changeSummary = new QWidget(summary);
    auto* changesLayout = new QHBoxLayout(changeSummary);
    changesLayout->setContentsMargins(0, 0, 0, 0);
    changesLayout->setSpacing(3);
    const std::array<QString, 4> names{QStringLiteral("agentAddedLines"), QStringLiteral("agentRemovedLines"),
        QStringLiteral("agentTriggerNodes"), QStringLiteral("agentFiles")};
    for (std::size_t i = 0; i < counters.size(); ++i) {
        counters[i] = new AnimatedCounter(changeSummary);
        counters[i]->SetMinimumDigits(1);
        counters[i]->setObjectName(names[i]);
        counters[i]->setFont(UiKit::MonoFont(12));
    }
    auto* plus = new QLabel(QStringLiteral("+"), changeSummary);
    plus->setObjectName(QStringLiteral("agentAddedSign"));
    changesLayout->addWidget(plus);
    changesLayout->addWidget(counters[0]);
    changesLayout->addSpacing(4);
    auto* minus = new QLabel(QStringLiteral("−"), changeSummary);
    minus->setObjectName(QStringLiteral("agentRemovedSign"));
    changesLayout->addWidget(minus);
    changesLayout->addWidget(counters[1]);
    // 节点与文件计数在窄侧栏里收起，只留增删行数；完整数字在修改面板里。
    changeDetails = new QWidget(changeSummary);
    auto* detailsLayout = new QHBoxLayout(changeDetails);
    detailsLayout->setContentsMargins(10, 0, 0, 0);
    detailsLayout->setSpacing(3);
    counters[2]->setParent(changeDetails);
    counters[3]->setParent(changeDetails);
    detailsLayout->addWidget(counters[2]);
    detailsLayout->addWidget(new QLabel(QStringLiteral("个触发器节点"), changeDetails));
    detailsLayout->addSpacing(10);
    detailsLayout->addWidget(counters[3]);
    detailsLayout->addWidget(new QLabel(QStringLiteral("个文件"), changeDetails));
    changesLayout->addWidget(changeDetails);
    summaryLayout->addWidget(changeSummary);
    outputSummary = new QWidget(summary);
    auto* outputLayout = new QHBoxLayout(outputSummary);
    outputLayout->setContentsMargins(10, 0, 0, 0);
    outputLayout->setSpacing(3);
    outputLayout->addWidget(new QLabel(QStringLiteral("输出"), outputSummary));
    output = new AnimatedCounter(outputSummary);
    output->SetMinimumDigits(1);
    output->setObjectName(QStringLiteral("agentOutputCharacters"));
    output->setToolTip(QStringLiteral("模型实际流式输出字符，与地图/文件改动分开"));
    outputLayout->addWidget(output);
    outputLayout->addWidget(new QLabel(QStringLiteral("字"), outputSummary));
    summaryLayout->addWidget(outputSummary);
    axis->addWidget(summary);

    auto* card = new QFrame(composerAxis);
    card->setObjectName(QStringLiteral("agentComposer"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 11, 10, 9);
    cardLayout->setSpacing(8);
    attachmentRow = new QWidget(card);
    attachmentRow->setObjectName(QStringLiteral("agentAttachments"));
    attachmentLayout = new QHBoxLayout(attachmentRow);
    attachmentLayout->setContentsMargins(0, 0, 0, 0);
    attachmentLayout->setSpacing(6);
    attachmentLayout->addStretch();
    attachmentRow->hide();
    cardLayout->addWidget(attachmentRow);
    prompt = new PromptEdit(card);
    prompt->setObjectName(QStringLiteral("agentPrompt"));
    prompt->setPlaceholderText(QStringLiteral("描述要完成的地图工作…"));
    prompt->setAccessibleName(QStringLiteral("地图任务输入"));
    prompt->setFrameShape(QFrame::NoFrame);
    prompt->setFixedHeight(46);
    prompt->submit = [this] { Submit(); };
    prompt->queue = [this] { Queue(); };
    prompt->resized = [this] { UpdatePromptHeight(); };
    QObject::connect(prompt, &QPlainTextEdit::textChanged, q, [this] { UpdateAvailability(); UpdatePromptHeight(); });
    queueNotice = new QLabel(card);
    queueNotice->setObjectName(QStringLiteral("agentQueueNotice"));
    queueNotice->setTextFormat(Qt::RichText);
    queueNotice->hide();
    QObject::connect(queueNotice, &QLabel::linkActivated, q, [this](const QString& link) {
        if (link == QStringLiteral("clear")) { queued.clear(); UpdateQueue(); }
        else if (link == QStringLiteral("next") && !busy && !queued.isEmpty()) Dispatch(queued.takeFirst());
    });
    cardLayout->addWidget(queueNotice);
    cardLayout->addWidget(prompt);
    auto* controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(6);

    attach = new QToolButton(card);
    attach->setObjectName(QStringLiteral("agentAttach"));
    attach->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    attach->setIconSize(QSize(16, 16));
    attach->setFixedSize(30, 30);
    attach->setToolTip(QStringLiteral("添加文件"));
    attach->setAccessibleName(QStringLiteral("添加文件"));
    attach->setCursor(Qt::PointingHandCursor);
    QObject::connect(attach, &QToolButton::clicked, q, [this] { ChooseAttachments(); });
    controls->addWidget(attach);

    permissions = new QToolButton(card);
    permissions->setObjectName(QStringLiteral("agentPermissions"));
    permissions->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    permissions->setIconSize(QSize(14, 14));
    permissions->setFixedHeight(30);
    permissions->setPopupMode(QToolButton::InstantPopup);
    permissions->setCursor(Qt::PointingHandCursor);
    permissions->setAccessibleName(QStringLiteral("访问权限"));
    permissionMenu = new QMenu(permissions);
    permissionMenu->setObjectName(QStringLiteral("agentPermissionMenu"));
    auto* menuTitle = new QLabel(QStringLiteral("模式"));
    menuTitle->setObjectName(QStringLiteral("agentPermissionMenuTitle"));
    menuTitle->setContentsMargins(14, 7, 14, 3);
    auto* titleAction = new QWidgetAction(permissionMenu);
    titleAction->setDefaultWidget(menuTitle);
    titleAction->setEnabled(false);
    permissionMenu->addAction(titleAction);
    for (std::size_t i = 0; i < kPermissionLevels.size(); ++i) {
        const auto& level = kPermissionLevels[i];
        auto* option = new PermissionOption(UiKit::SvgIcon(QString::fromLatin1(level.icon)),
            QString::fromUtf8(level.title), QString::fromUtf8(level.description));
        option->SetWarning(level.commands);
        auto* action = new QWidgetAction(permissionMenu);
        action->setDefaultWidget(option);
        permissionMenu->addAction(action);
        permissionOptions[i] = option;
        QObject::connect(option, &PermissionOption::Activated, q, [this, i] {
            permissionMenu->close();
            const auto& chosen = kPermissionLevels[i];
            if (chosen.edits == settings.allowEdits && chosen.save == settings.allowSave
                && chosen.commands == settings.allowCommands) return;
            settings.allowEdits = chosen.edits;
            settings.allowSave = chosen.save;
            settings.allowCommands = chosen.commands;
            UpdatePermissions();
            emit q->PermissionsChangeRequested(chosen.edits, chosen.save, chosen.commands);
        });
    }
    permissions->setMenu(permissionMenu);
    controls->addWidget(permissions);
    controls->addStretch();

    models = new QComboBox(card);
    models->setObjectName(QStringLiteral("agentModel"));
    models->setFixedHeight(30);
    models->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    models->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    models->setCursor(Qt::PointingHandCursor);
    models->setAccessibleName(QStringLiteral("当前 AI 模型"));
    QObject::connect(models, qOverload<int>(&QComboBox::currentIndexChanged), q, [this] { ModelChanged(); });
    controls->addWidget(models);
    addModel = new QToolButton(card);
    addModel->setObjectName(QStringLiteral("agentAddModel"));
    addModel->setText(QStringLiteral("选择模型"));
    addModel->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/chevron-down")));
    addModel->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addModel->setLayoutDirection(Qt::RightToLeft);
    addModel->setIconSize(QSize(14, 14));
    addModel->setFixedHeight(30);
    addModel->setCursor(Qt::PointingHandCursor);
    addModel->setToolTip(QStringLiteral("在设置中添加提供商并启用模型"));
    QObject::connect(addModel, &QToolButton::clicked, q, &AgentWindow::SettingsRequested);
    controls->addWidget(addModel);
    // 数值标签保留给辅助技术与测试；界面上由圆环与右侧上下文分段栏呈现。
    context = new QLabel(card);
    context->setObjectName(QStringLiteral("agentContext"));
    context->hide();
    usage = new QLabel(QStringLiteral("上下文用量未提供"), card);
    usage->setObjectName(QStringLiteral("agentUsage"));
    usage->hide();
    ring = new ContextRing(card);
    BuildContextPopover();
    QObject::connect(ring, &ContextRing::Activated, q, [this] {
        UpdateContext();
        contextPopover->ToggleAnchored(ring, UiKit::PopoverSurface::PreferredSide::Above, Qt::AlignHCenter);
    });
    controls->addWidget(ring);
    stop = new QPushButton(card);
    stop->setObjectName(QStringLiteral("agentStop"));
    stop->setIconSize(QSize(12, 12));
    stop->setFixedSize(30, 30);
    stop->setCursor(Qt::PointingHandCursor);
    stop->setAccessibleName(QStringLiteral("停止当前任务"));
    stop->setToolTip(QStringLiteral("停止当前任务"));
    stop->hide();
    QObject::connect(stop, &QPushButton::clicked, q, [this] {
        if (!busy || stopped) return;
        stopped = true;
        stop->setEnabled(false);
        SetStatus(QStringLiteral("正在停止"));
        emit q->StopRequested();
    });
    controls->addWidget(stop);
    send = new QPushButton(card);
    send->setObjectName(QStringLiteral("agentSend"));
    send->setIconSize(QSize(16, 16));
    send->setFixedSize(30, 30);
    send->setCursor(Qt::PointingHandCursor);
    send->setAccessibleName(QStringLiteral("发送任务"));
    QObject::connect(send, &QPushButton::clicked, q, [this] { Submit(); });
    controls->addWidget(send);
    cardLayout->addLayout(controls);
    axis->addWidget(card);
    centerLayout->addWidget(composerContainer);

    root->addWidget(centerWorkspace, 1);

    BuildRightInspector(root);
    if (envPermissionBtn) envPermissionBtn->setMenu(permissionMenu);

    BuildSessionPopover();
    ApplyAgentStyleSheet();

    flushTimer.setInterval(40);
    flushTimer.setSingleShot(true);
    QObject::connect(&flushTimer, &QTimer::timeout, q, [this] { Flush(); });
    elapsedTimer.setInterval(1000);
    QObject::connect(&elapsedTimer, &QTimer::timeout, q, [this] { UpdateElapsed(); });

    UpdateInspectorChanges();
    UpdateInspectorTools();
    UpdateInspectorEnvironment();
    UpdateSummary();
}

void AgentWindow::Private::ApplyAgentStyleSheet() {
    if (applyingStyleSheet) return;
    const auto& theme = ResolveAgentTheme(q);
    const auto& c = theme.colors;
    const QString bg = UiKit::ToCss(c.bg);
    const QString panel = UiKit::ToCss(c.panel);
    const QString raised = UiKit::ToCss(c.raised);
    const QString hover = UiKit::ToCss(c.hover);
    const QString line = UiKit::ToCss(c.line);
    const QString text = UiKit::ToCss(c.text);
    const QString textBody = UiKit::ToCss(c.textBody);
    const QString muted = UiKit::ToCss(c.muted);
    const QString dim = UiKit::ToCss(c.dim);
    const QString accent = UiKit::ToCss(c.accent);
    const QString accentSoft = UiKit::ToCss(c.accentSoft);
    const QString accentLine = UiKit::ToCss(c.accentLine);
    const QString selection = UiKit::ToCss(c.selection);
    const QString amber = UiKit::ToCss(c.amber);
    const QString red = UiKit::ToCss(c.red);

    QString qss;
    qss.reserve(7200);
    qss += QStringLiteral(
        "QWidget#agentWindow { background:%1; color:%6; }"
        "QFrame#agentLeftSidebar { background:%1; border:0; border-right:1px solid %5; }"
        "QFrame#agentRightInspector { background:%1; border:0; border-left:1px solid %5; }"
        "QFrame#agentCenterWorkspace { background:%2; border:0; }"
        "QFrame#agentLeftRailHeader, QFrame#agentRightRailHeader {"
        " background:%1; border:0; border-bottom:1px solid %5; }"
        "QFrame#agentSidebarFooter { background:%1; border:0; border-top:1px solid %5; }"
        "QLabel#agentRailTitle { font-size:12px; font-weight:600; color:%7; }"
        "QPushButton#agentSidebarNew {"
        " background:%3; border:1px solid %5; border-radius:8px; color:%6;"
        " font-size:12px; font-weight:500; text-align:left; padding:0 10px; }"
        "QPushButton#agentSidebarNew:hover { background:%4; border-color:%12; }"
        "QPushButton#agentSidebarNew:disabled { color:%9; background:transparent; }"
        "QLineEdit#agentSidebarSearch {"
        " background:%2; border:1px solid %5; border-radius:7px; color:%6;"
        " font-size:12px; padding-right:6px; }"
        "QLineEdit#agentSidebarSearch:focus { border-color:%12; background:%3; }"
        "QLabel#agentSidebarGroupTitle { font-size:11px; font-weight:600; color:%8; }"
        "QLabel#agentSidebarGroupCount { font-size:11px; color:%9; }"
        "QListWidget#agentSidebarSessionList { background:transparent; border:0; outline:0; }"
        "QListWidget#agentSidebarSessionList::item { border-radius:7px; padding:0; margin:1px 0; border:1px solid transparent; }"
        "QListWidget#agentSidebarSessionList::item:hover { background:%3; }"
        "QListWidget#agentSidebarSessionList::item:selected { background:%13; border-color:transparent; }"
        "QLabel#agentSidebarStatusDot { border-radius:3px; background:%9; }"
        "QLabel#agentSidebarStatusDot[ready=\"true\"] { background:%10; }"
        "QLabel#agentSidebarAccount { font-size:12px; color:%8; }"
        "QToolButton#agentCollapseLeftRail, QToolButton#agentCollapseRightRail, QToolButton#agentSidebarSettings {"
        " border:1px solid transparent; border-radius:6px; background:transparent; padding:0;"
        " min-width:26px; max-width:26px; min-height:26px; max-height:26px; }"
        "QToolButton#agentCollapseLeftRail:hover, QToolButton#agentCollapseRightRail:hover, QToolButton#agentSidebarSettings:hover {"
        " background:%3; }"
        "QScrollArea#agentInspectorScroll, QWidget#agentInspectorBody { background:transparent; border:0; }"
        "QFrame#agentInspectorDivider { background:%5; border:0; }"
        "QLabel#agentInspectorHeading { font-size:11px; font-weight:600; color:%8; }"
        "QLabel#agentInspectorLabel { font-size:12px; color:%7; }"
        "QLabel#agentInspectorMuted { font-size:12px; color:%8; }"
        "QLabel#agentInspectorPrimaryValue { font-size:13px; font-weight:600; color:%6; }"
        "QToolButton#agentEnvPermissionPill {"
        " background:%3; border:1px solid %5; border-radius:11px; padding:0 9px; font-size:11px; color:%7; }"
        "QToolButton#agentEnvPermissionPill:hover { background:%4; color:%6; }"
        "QToolButton#agentEnvPermissionPill[level=\"full\"] { color:%14; border-color:%14; }"
        "QToolButton#agentEnvPermissionPill::menu-indicator { image:none; width:0; }"
        "QToolButton#agentEnvFollowSwitch {"
        " background:%3; border:1px solid %5; border-radius:11px; padding:0 9px; font-size:11px; color:%8; }"
        "QToolButton#agentEnvFollowSwitch:hover { background:%4; color:%6; }"
        "QToolButton#agentEnvFollowSwitch:checked { background:%11; border-color:%12; color:%10; font-weight:600; }"
        "QLabel#agentConnectorDot { border-radius:3px; background:%9; }"
        "QLabel#agentConnectorDot[active=\"true\"] { background:%10; }"
        "QPushButton#agentInspectorLinkBtn {"
        " border:0; background:transparent; color:%10; font-size:12px; padding:0 2px; }"
        "QPushButton#agentInspectorLinkBtn:hover { text-decoration:underline; }"
        "QFrame#agentInspectorItemRow {"
        " background:%2; border:1px solid %5; border-radius:6px; }"
        "QPushButton#agentInspectorJumpBtn {"
        " border:0; background:transparent; color:%10; font-size:11px; padding:0 4px; }"
        "QPushButton#agentInspectorJumpBtn:hover { color:%6; }"
        "QLabel#agentInspectorToolDot { border-radius:3px; background:%9; }"
        "QLabel#agentInspectorToolDot[stateKind=\"running\"] { background:%14; }"
        "QLabel#agentInspectorToolDot[stateKind=\"ok\"] { background:%10; }"
        "QLabel#agentInspectorToolDot[stateKind=\"error\"] { background:%15; }")
        .arg(bg, panel, raised, hover, line, text, textBody, muted, dim, accent, accentSoft, accentLine, selection, amber, red);

    qss += QStringLiteral(
        "QFrame#agentConversationHeader { background:%2; border:0; border-bottom:1px solid %5; }"
        "QLabel#agentHeaderTitle { font-size:13px; font-weight:600; color:%6; padding-left:2px; }"
        "QLabel#agentActiveMap { font-size:13px; color:%8; padding-left:4px; }"
        "QLabel#agentHistoryNotice, QLabel#agentMapSummary {"
        " font-size:12px; color:%8; background:%1; border-bottom:1px solid %5; }"
        "QLabel#agentMessageAnnotation { font-size:11px; color:%8; }"
        "QToolButton#agentBack, QToolButton#agentConversations, QToolButton#agentFollowChanges, QToolButton#agentNewConversation,"
        " QToolButton#agentPresentation, QToolButton#agentSettings, QToolButton#agentExpandLeftRail, QToolButton#agentToggleRightRail {"
        " border:1px solid transparent; border-radius:7px; background:transparent; padding:0;"
        " min-width:30px; max-width:30px; min-height:30px; max-height:30px; }"
        "QToolButton#agentBack:hover, QToolButton#agentConversations:hover, QToolButton#agentFollowChanges:hover,"
        " QToolButton#agentNewConversation:hover, QToolButton#agentPresentation:hover,"
        " QToolButton#agentSettings:hover, QToolButton#agentExpandLeftRail:hover, QToolButton#agentToggleRightRail:hover {"
        " background:%3; }"
        "QToolButton#agentFollowChanges:checked, QToolButton#agentToggleRightRail:checked {"
        " background:%11; color:%10; border-color:%12; }"
        "QToolButton#agentPresentation::menu-indicator { image:none; width:0; }"
        "QFrame#agentLeftSidebar > QWidget, QListWidget#agentSidebarSessionList QWidget,"
        " QScrollArea#agentInspectorScroll, QScrollArea#agentInspectorScroll > QWidget#qt_scrollarea_viewport,"
        " QWidget#agentInspectorBody, QWidget#agentInspectorListContainer, QWidget#agentInspectorListContainer > QWidget,"
        " QScrollArea#agentConversation, QScrollArea#agentConversation > QWidget#qt_scrollarea_viewport,"
        " QWidget#agentConversationCanvas, QWidget#agentReadingColumn, QWidget#agentEmptyState,"
        " QWidget#agentUserTurn, QWidget#agentAssistantTurn, QWidget#agentDisclosureBody,"
        " QFrame#agentToolRow > QWidget, QFrame#agentInlineChanges > QWidget,"
        " QWidget#agentComposerContainer, QWidget#agentComposerAxis,"
        " QWidget#agentChangeSummary, QWidget#agentChangeSummary QWidget,"
        " QWidget#agentAttachments { border:0; background:transparent; }"
        "QLabel#agentEmptyBadge { background:%3; border:1px solid %5; border-radius:12px; }"
        "QLabel#agentEmptyTitle { font-size:22px; font-weight:600; color:%6; }"
        "QLabel#agentEmptyHint { font-size:13px; color:%8; }"
        "QPushButton#agentSuggestion { border:1px solid %5; border-radius:16px; background:%3;"
        " padding:6px 14px; font-size:12px; color:%7; }"
        "QPushButton#agentSuggestion:hover { color:%6; background:%4; border-color:%12; }"
        "QFrame#agentUserBubble { background:%4; border:1px solid %5; border-radius:16px; }"
        "QFrame#agentAssistantBody { background:transparent; border:0; }"
        "QTextBrowser#agentMessageBody { background:transparent; border:0; padding:0; font-size:14px; color:%7; }"
        "QFrame#agentToolRow { background:transparent; border:0; border-radius:7px; }"
        "QFrame#agentToolRow:hover { background:%3; }"
        "QFrame#agentInlineChanges { background:%1; border:1px solid %5; border-radius:10px; }"
        "QLabel#agentToolTitle { font-size:13px; color:%8; }"
        "QLabel#agentToolTitle[changeRow=\"true\"] { font-weight:500; color:%6; }"
        "QLabel#agentToolState, QLabel#agentToolDuration { font-size:12px; color:%8; }"
        "QLabel#agentToolState[statusKind=\"running\"] { color:%14; }"
        "QLabel#agentToolState[statusKind=\"ok\"] { color:%10; }"
        "QLabel#agentToolState[statusKind=\"error\"] { color:%15; }"
        "QToolButton#agentToolToggle, QToolButton#agentChangeToggle { border:0; background:transparent; padding:0; border-radius:6px; }"
        "QToolButton#agentToolToggle:hover, QToolButton#agentChangeToggle:hover { background:%4; }"
        "QTreeWidget#agentToolDetails, QTreeWidget#agentChangeDetails { border:1px solid %5; border-radius:8px;"
        " background:%1; color:%7; font-size:12px; padding:4px; }"
        "QPlainTextEdit#agentInlineDetailPreview { border:1px solid %5; border-radius:8px; background:%1; color:%7; font-size:12px; }"
        "QPushButton#agentInlineNavigate { border:1px solid %5; border-radius:10px; background:%3; color:%10; padding:1px 8px; font-size:11px; }"
        "QPushButton#agentInlineNavigate:hover { border-color:%12; background:%4; }"
        "QFrame#agentComposer { border:1px solid %5; border-radius:18px; background:%3; }"
        "QPlainTextEdit#agentPrompt { background:transparent; border:0; padding:2px 0; font-size:14px; color:%6; }"
        "QLabel#agentQueueNotice { font-size:12px; color:%8; }"
        "QToolButton#agentAttach { border:1px solid %5; border-radius:15px; background:%2; padding:0;"
        " min-width:30px; max-width:30px; min-height:30px; max-height:30px; }"
        "QToolButton#agentAttach:hover { background:%4; border-color:%12; }"
        "QToolButton#agentPermissions, QToolButton#agentAddModel { border:1px solid transparent; border-radius:15px; background:transparent;"
        " padding:0 10px 0 9px; font-size:12px; color:%8; }"
        "QToolButton#agentPermissions:hover, QToolButton#agentAddModel:hover { background:%4; color:%6; }"
        "QToolButton#agentPermissions[level=\"full\"] { color:%14; }"
        "QToolButton#agentPermissions::menu-indicator { image:none; width:0; }"
        "QComboBox#agentModel { background:transparent; border:1px solid transparent; border-radius:15px; font-size:12px;"
        " color:%8; padding:0 24px 0 10px; min-height:28px; }"
        "QComboBox#agentModel:hover, QComboBox#agentModel:on { background:%4; color:%6; }"
        "QComboBox#agentModel::drop-down { border:0; width:22px; }"
        "QComboBox#agentModel QAbstractItemView { background:%3; color:%6; border:1px solid %5; border-radius:10px; padding:4px;"
        " min-width:196px; font-size:12px; selection-background-color:%13; selection-color:%6; outline:0; }"
        "QMenu#agentPermissionMenu { background:%3; color:%6; border:1px solid %5; border-radius:12px; padding:4px 0 6px 0; }"
        "QLabel#agentPermissionMenuTitle { font-size:11px; font-weight:600; color:%8; }"
        "QLabel#agentPermissionTitle { font-size:13px; font-weight:500; color:%6; }"
        "QLabel#agentPermissionTitle[warning=\"true\"] { color:%14; }"
        "QLabel#agentPermissionDetail { font-size:11px; color:%8; }"
        "QLabel#agentContextHeading { font-size:12px; color:%8; }"
        "QLabel#agentContextSourceBadge { font-size:11px; color:%9; }"
        "QLabel#agentContextFraction { font-size:13px; font-weight:500; color:%6; }"
        "QLabel#agentContextTokens { font-size:12px; color:%8; }"
        "QPushButton#agentSend { border:1px solid transparent; border-radius:15px; padding:0; background:%6;"
        " min-width:30px; max-width:30px; min-height:30px; max-height:30px; }"
        "QPushButton#agentSend:hover { background:%10; }"
        "QPushButton#agentSend:disabled { background:%4; border-color:%5; }"
        "QPushButton#agentStop { border:1px solid %5; border-radius:15px; padding:0; background:%4;"
        " min-width:30px; max-width:30px; min-height:30px; max-height:30px; }"
        "QPushButton#agentStop:hover { border-color:%15; }"
        "QWidget#agentChangeSummary QLabel { font-size:12px; color:%8; }"
        "QLabel#agentAddedSign, QLabel#agentAddedLines { color:%10; }"
        "QLabel#agentRemovedSign, QLabel#agentRemovedLines { color:%15; }"
        "QLabel#agentStatus[failed=\"true\"] { color:%15; }"
        "QLabel#agentActivityDot { border-radius:3px; background:%9; }"
        "QLabel#agentActivityDot[running=\"true\"] { background:%10; }"
        "QLabel#agentActivityDot[failed=\"true\"] { background:%15; }"
        "QFrame#agentAttachmentChip { border:1px solid %5; border-radius:12px; background:%2; }"
        "QFrame#agentAttachmentChip QLabel { font-size:12px; color:%7; }"
        "QFrame#agentAttachmentChip QToolButton { border:0; border-radius:9px; padding:0; background:transparent; }"
        "QFrame#agentAttachmentChip QToolButton:hover { background:%4; }")
        .arg(bg, panel, raised, hover, line, text, textBody, muted, dim, accent, accentSoft, accentLine, selection, amber, red);

    if (qss == appliedStyleSheet) return;
    appliedStyleSheet = qss;
    applyingStyleSheet = true;
    q->setStyleSheet(qss);
    applyingStyleSheet = false;
}

void AgentWindow::Private::BuildSessionPopover() {
    sessionsPopover = new UiKit::PopoverSurface(q);
    sessionsPopover->setObjectName(QStringLiteral("agentSessionPopover"));
    auto* content = sessionsPopover->ContentWidget();
    content->setFixedWidth(320);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(8, 4, 8, 0);
    auto* label = new QLabel(QStringLiteral("本地图会话"), content);
    QFont font = label->font(); font.setPixelSize(14); font.setWeight(QFont::Medium); label->setFont(font);
    heading->addWidget(label);
    heading->addStretch();
    auto* create = new QToolButton(content);
    create->setObjectName(QStringLiteral("agentSessionCreate"));
    create->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    create->setFixedSize(28, 28);
    create->setToolTip(QStringLiteral("新会话"));
    create->setAccessibleName(QStringLiteral("新建本地图会话"));
    QObject::connect(create, &QToolButton::clicked, q, [this] {
        if (busy) return;
        sessionsPopover->Dismiss(); emit q->NewConversationRequested();
    });
    heading->addWidget(create);
    layout->addLayout(heading);
    sessionList = new QListWidget(content);
    sessionList->setObjectName(QStringLiteral("agentSessionList"));
    sessionList->setAccessibleName(QStringLiteral("本地图会话列表"));
    sessionList->setFrameShape(QFrame::NoFrame);
    sessionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sessionList->setSpacing(4);
    sessionList->setMaximumHeight(360);
    sessionList->setMinimumHeight(96);
    layout->addWidget(sessionList);
    QObject::connect(sessionList, &QListWidget::itemClicked, q, [this](QListWidgetItem* item) {
        if (busy || !item) return;
        const auto id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) emit q->ConversationSwitchRequested(id);
    });
    QObject::connect(sessionList, &QListWidget::itemActivated, q, [this](QListWidgetItem* item) {
        if (busy || !item) return;
        const auto id = item->data(Qt::UserRole).toString();
        sessionsPopover->Dismiss();
        if (!id.isEmpty()) emit q->ConversationSwitchRequested(id);
    });
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(4, 0, 4, 4);
    actions->setSpacing(8);
    renameSession = new QPushButton(QStringLiteral("重命名"), content);
    renameSession->setObjectName(QStringLiteral("agentSessionRename"));
    archiveSession = new QPushButton(QStringLiteral("删除"), content);
    archiveSession->setObjectName(QStringLiteral("agentSessionArchive"));
    actions->addWidget(renameSession);
    actions->addStretch();
    actions->addWidget(archiveSession);
    layout->addLayout(actions);
    renameRow = new QWidget(content);
    renameRow->setObjectName(QStringLiteral("agentSessionRenameRow"));
    auto* renameLayout = new QHBoxLayout(renameRow);
    renameLayout->setContentsMargins(4, 0, 4, 4);
    renameLayout->setSpacing(4);
    sessionTitle = new QLineEdit(renameRow);
    sessionTitle->setObjectName(QStringLiteral("agentSessionTitleEdit"));
    sessionTitle->setAccessibleName(QStringLiteral("会话标题"));
    sessionTitle->setMaxLength(256);
    renameLayout->addWidget(sessionTitle, 1);
    auto* confirm = new QToolButton(renameRow);
    confirm->setObjectName(QStringLiteral("agentSessionConfirmRename"));
    confirm->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/check")));
    confirm->setToolTip(QStringLiteral("保存标题"));
    confirm->setAccessibleName(QStringLiteral("保存会话标题"));
    renameLayout->addWidget(confirm);
    auto* cancel = new QToolButton(renameRow);
    cancel->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/x")));
    cancel->setAccessibleName(QStringLiteral("取消重命名"));
    renameLayout->addWidget(cancel);
    QObject::connect(cancel, &QToolButton::clicked, q, [this] { renameRow->hide(); });
    const auto saveTitle = [this] {
        const auto id = sessionTitle->property("sessionId").toString();
        const auto title = sessionTitle->text().trimmed();
        if (busy || id.isEmpty() || title.isEmpty()) return;
        emit q->ConversationRenameRequested(id, title); renameRow->hide();
    };
    QObject::connect(confirm, &QToolButton::clicked, q, saveTitle);
    QObject::connect(sessionTitle, &QLineEdit::returnPressed, q, saveTitle);
    QObject::connect(renameSession, &QPushButton::clicked, q, [this] {
        const auto* item = sessionList->currentItem();
        if (busy || !item || item->data(Qt::UserRole).toString().isEmpty()) return;
        sessionTitle->setProperty("sessionId", item->data(Qt::UserRole));
        sessionTitle->setText(item->data(Qt::UserRole + 1).toString());
        renameRow->show(); sessionTitle->setFocus(); sessionTitle->selectAll();
    });
    QObject::connect(archiveSession, &QPushButton::clicked, q, [this] {
        const auto* item = sessionList->currentItem();
        if (busy || !item) return;
        const auto id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) return;
        const auto title = item->data(Qt::UserRole + 1).toString();
        sessionsPopover->Dismiss();
        if (QMessageBox::question(q, QStringLiteral("删除会话"),
            QStringLiteral("将“%1”移到本地归档？完整记录会保留。").arg(title),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            emit q->ConversationArchiveRequested(id);
    });
    renameRow->hide();
    layout->addWidget(renameRow);
    UpdateSessions();
}

void AgentWindow::Private::UpdateSessions() {
    if (!sessionList) return;
    const QSignalBlocker blocker(sessionList);
    sessionList->clear();
    renameRow->hide();
    if (sessions.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("还没有会话"), sessionList);
        item->setFlags(Qt::NoItemFlags);
    }
    for (const auto& value : sessions) {
        const auto session = value.toObject();
        const auto id = session.value(QStringLiteral("id")).toString();
        auto* item = new QListWidgetItem(sessionList);
        const auto title = session.value(QStringLiteral("title")).toString(QStringLiteral("新会话"));
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 1, title);
        auto* row = new QWidget(sessionList);
        row->setObjectName(QStringLiteral("agentSessionRow"));
        row->setAutoFillBackground(false);
        row->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(8, 8, 8, 8);
        rowLayout->setSpacing(4);
        auto* titleLabel = new UiKit::ElidedLabel(title, row);
        titleLabel->setMinimumWidth(0);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rowLayout->addWidget(titleLabel);
        const auto time = QDateTime::fromString(session.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
        auto* detail = new UiKit::ElidedLabel((time.isValid() ? time.toLocalTime().toString(QStringLiteral("MM-dd HH:mm")) : QStringLiteral("时间未知"))
            + QStringLiteral(" · ") + session.value(QStringLiteral("model_id")).toString(QStringLiteral("尚未使用模型")), row);
        detail->setObjectName(QStringLiteral("agentSessionDetail"));
        detail->setMinimumWidth(0);
        detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        QFont font = detail->font(); font.setPixelSize(12); detail->setFont(font);
        QPalette muted = detail->palette(); muted.setColor(QPalette::WindowText, muted.color(QPalette::PlaceholderText)); detail->setPalette(muted);
        rowLayout->addWidget(detail);
        item->setSizeHint(QSize(280, 60));
        sessionList->setItemWidget(item, row);
        if (id == selectedSession) sessionList->setCurrentItem(item);
    }
    sessionList->setFixedHeight(std::clamp(sessionList->count() * 64 + 4, 96, 360));
    const bool available = !sessions.isEmpty() && !busy;
    renameSession->setEnabled(available);
    archiveSession->setEnabled(available);

    if (sidebarSessionList) {
        const QSignalBlocker sideBlocker(sidebarSessionList);
        sidebarSessionList->clear();
        if (sidebarSessionCount) sidebarSessionCount->setText(QString::number(sessions.size()));
        if (sessions.isEmpty()) {
            auto* emptyItem = new QListWidgetItem(QStringLiteral("暂无已保存会话"), sidebarSessionList);
            emptyItem->setFlags(Qt::NoItemFlags);
        } else {
            const auto& theme = ResolveAgentTheme(q);
            const QColor primaryText = UiKit::ToQColor(theme.colors.text);
            const QColor secondaryText = UiKit::ToQColor(theme.colors.textBody);
            const QColor dimText = UiKit::ToQColor(theme.colors.dim);
            for (const auto& value : sessions) {
                const auto session = value.toObject();
                const auto id = session.value(QStringLiteral("id")).toString();
                const auto title = session.value(QStringLiteral("title")).toString(QStringLiteral("新会话"));
                const auto modelId = session.value(QStringLiteral("model_id")).toString();
                const auto time = QDateTime::fromString(session.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
                const QString meta = (time.isValid() ? time.toLocalTime().toString(QStringLiteral("MM-dd HH:mm")) : QStringLiteral("刚刚"))
                    + (modelId.isEmpty() ? QString() : QStringLiteral(" · ") + modelId);
                auto* item = new QListWidgetItem(sidebarSessionList);
                item->setData(Qt::UserRole, id);
                item->setData(Qt::UserRole + 1, title);
                item->setData(Qt::UserRole + 2, meta);
                item->setToolTip(title + QLatin1Char('\n') + meta);

                auto* row = new QWidget(sidebarSessionList);
                row->setAutoFillBackground(false);
                row->setAttribute(Qt::WA_TransparentForMouseEvents);
                auto* rowLayout = new QVBoxLayout(row);
                rowLayout->setContentsMargins(9, 6, 9, 6);
                rowLayout->setSpacing(2);
                auto* titleLabel = new UiKit::ElidedLabel(title, row);
                titleLabel->setMinimumWidth(0);
                titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                QFont titleFont = titleLabel->font();
                titleFont.setPixelSize(12);
                titleFont.setWeight(id == selectedSession ? QFont::Medium : QFont::Normal);
                titleLabel->setFont(titleFont);
                QPalette titlePal = titleLabel->palette();
                titlePal.setColor(QPalette::WindowText, id == selectedSession ? primaryText : secondaryText);
                titleLabel->setPalette(titlePal);
                rowLayout->addWidget(titleLabel);

                auto* metaLabel = new UiKit::ElidedLabel(meta, row);
                metaLabel->setMinimumWidth(0);
                metaLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                QFont metaFont = metaLabel->font();
                metaFont.setPixelSize(11);
                metaLabel->setFont(metaFont);
                QPalette metaPal = metaLabel->palette();
                metaPal.setColor(QPalette::WindowText, dimText);
                metaLabel->setPalette(metaPal);
                rowLayout->addWidget(metaLabel);

                item->setSizeHint(QSize(180, 46));
                sidebarSessionList->setItemWidget(item, row);
                if (id == selectedSession) sidebarSessionList->setCurrentItem(item);
            }
        }
        FilterSidebarSessions();
    }
}

void AgentWindow::Private::FilterSidebarSessions() {
    if (!sidebarSessionList || !sidebarSearch) return;
    const QString query = sidebarSearch->text().trimmed();
    for (int i = 0; i < sidebarSessionList->count(); ++i) {
        QListWidgetItem* item = sidebarSessionList->item(i);
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) {
            item->setHidden(!query.isEmpty());
            continue;
        }
        const QString title = item->data(Qt::UserRole + 1).toString();
        const QString meta = item->data(Qt::UserRole + 2).toString();
        const bool match = query.isEmpty()
            || title.contains(query, Qt::CaseInsensitive)
            || meta.contains(query, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
}

void AgentWindow::Private::BuildContextPopover() {
    // 悬浮用量卡片（对齐 Claude Code 用量浮层）：标题 + 来源、比例进度条与 token 统计。
    contextPopover = new UiKit::PopoverSurface(q);
    contextPopover->setObjectName(QStringLiteral("agentContextPopover"));
    auto* content = contextPopover->ContentWidget();
    content->setMinimumWidth(256);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(6);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);
    contextHeading = new QLabel(QStringLiteral("上下文窗口"), content);
    contextHeading->setObjectName(QStringLiteral("agentContextHeading"));
    topRow->addWidget(contextHeading);
    topRow->addStretch();
    contextSourceBadge = new QLabel(content);
    contextSourceBadge->setObjectName(QStringLiteral("agentContextSourceBadge"));
    topRow->addWidget(contextSourceBadge);
    layout->addLayout(topRow);

    contextFraction = new QLabel(content);
    contextFraction->setObjectName(QStringLiteral("agentContextFraction"));
    layout->addWidget(contextFraction);

    popoverSegmentBar = new ContextSegmentBar(content);
    popoverSegmentBar->setFixedHeight(6);
    layout->addWidget(popoverSegmentBar);

    contextTokens = new QLabel(content);
    contextTokens->setObjectName(QStringLiteral("agentContextTokens"));
    layout->addWidget(contextTokens);

    // 明细只进提示文字；对象保留给测试和无障碍读取。
    contextBreakdown = new QLabel(content);
    contextBreakdown->setObjectName(QStringLiteral("agentContextBreakdown"));
    contextBreakdown->hide();
    contextSource = new QLabel(content);
    contextSource->setObjectName(QStringLiteral("agentContextSource"));
    contextSource->hide();
}

void AgentWindow::Private::UpdateContext() {
    if (!contextPopover || !ring) return;
    const auto used = ring->Used();
    const auto limit = ring->Limit();
    if (used >= 0 && limit > 0) {
        const auto percentage = std::clamp(qRound(static_cast<double>(used) / static_cast<double>(limit) * 100.0), 0, 100);
        contextFraction->setText(QStringLiteral("已用 %1%（剩余 %2%）").arg(percentage).arg(100 - percentage));
        contextTokens->setText(QStringLiteral("已用 %1，共 %2 tokens").arg(FormatTokens(used), FormatTokens(limit)));
    } else {
        contextFraction->setText(QStringLiteral("暂无用量数据"));
        contextTokens->setText(limit > 0 ? QStringLiteral("上限 %1 tokens").arg(FormatTokens(limit)) : QStringLiteral("尚未选择模型"));
    }
    if (contextSourceBadge) {
        contextSourceBadge->setText(limit > 0
            ? (ring->IsEstimated() ? QStringLiteral("估算上限") : ring->Source())
            : QString());
    }
    QStringList details;
    const std::array<QPair<QString, QString>, 3> fields{{
        {QStringLiteral("input_tokens"), QStringLiteral("输入")}, {QStringLiteral("output_tokens"), QStringLiteral("输出")},
        {QStringLiteral("cache_read_tokens"), QStringLiteral("缓存命中")}}};
    for (const auto& field : fields) {
        auto value = actualUsage.value(field.first);
        if (field.first == QStringLiteral("cache_read_tokens") && !IsExactCount(value)) value = actualUsage.value(QStringLiteral("cached_tokens"));
        if (IsExactCount(value)) details.append(field.second + QLatin1Char(' ') + FormatTokens(Count(value)));
    }
    contextBreakdown->setText(details.join(QStringLiteral(" · ")));
    contextSource->setText(limit > 0 ? QStringLiteral("上限来源：%1%2").arg(ring->Source(), ring->IsEstimated() ? QStringLiteral("（估算）") : QString()) : QString());

    const bool hasInput = IsExactCount(actualUsage.value(QStringLiteral("input_tokens")));
    const bool hasOutput = IsExactCount(actualUsage.value(QStringLiteral("output_tokens")));
    const QJsonValue cacheField = IsExactCount(actualUsage.value(QStringLiteral("cache_read_tokens")))
        ? actualUsage.value(QStringLiteral("cache_read_tokens"))
        : actualUsage.value(QStringLiteral("cached_tokens"));
    const bool hasCache = IsExactCount(cacheField);
    const qint64 inputTotal = hasInput ? Count(actualUsage.value(QStringLiteral("input_tokens"))) : 0;
    const qint64 cacheRead = hasCache ? Count(cacheField) : 0;
    const qint64 outputTotal = hasOutput ? Count(actualUsage.value(QStringLiteral("output_tokens"))) : 0;
    const qint64 uncachedInput = hasInput ? std::max<qint64>(0, inputTotal - std::min(inputTotal, cacheRead))
        : (used >= 0 ? std::max<qint64>(0, used - cacheRead - outputTotal) : 0);

    if (popoverSegmentBar) {
        popoverSegmentBar->SetBreakdown(limit, used, uncachedInput, cacheRead, outputTotal);
    }

    QStringList tip;
    if (used >= 0 && limit > 0) tip.append(QStringLiteral("%1 / %2 tokens").arg(QLocale().toString(used), QLocale().toString(limit)));
    if (!details.isEmpty()) tip.append(contextBreakdown->text());
    if (!contextSource->text().isEmpty()) tip.append(contextSource->text());
    tip.append(QStringLiteral("接近上限时会自动压缩较早的回合，完整记录保留在本地。"));
    contextPopover->ContentWidget()->setToolTip(tip.join(QLatin1Char('\n')));
    contextPopover->ContentWidget()->adjustSize();

    // 同步更新右侧检查栏的真实上下文分段条与四项明细
    if (inspectorSegmentBar) {
        inspectorSegmentBar->SetBreakdown(limit, used, uncachedInput, cacheRead, outputTotal);
        if (inspectorContextSource) {
            inspectorContextSource->setText(limit > 0
                ? (ring->IsEstimated() ? QStringLiteral("估算上限") : ring->Source())
                : QString());
        }
        if (used >= 0 && limit > 0) {
            const double pct = std::clamp(static_cast<double>(used) / static_cast<double>(limit) * 100.0, 0.0, 100.0);
            inspectorContextPercent->setText(QStringLiteral("已使用 %1%").arg(qRound(pct)));
            inspectorContextTokens->setText(QStringLiteral("%1 / %2").arg(FormatTokens(used), FormatTokens(limit)));
            const qint64 freeTokens = std::max<qint64>(0, limit - used);
            const auto formatPct = [limit](qint64 part) {
                if (limit <= 0) return QStringLiteral("—");
                const double ratio = std::clamp(static_cast<double>(part) / static_cast<double>(limit) * 100.0, 0.0, 100.0);
                return QStringLiteral("%1%").arg(ratio, 0, 'f', ratio >= 10.0 || ratio == 0.0 ? 0 : 1);
            };
            const std::array<qint64, 4> values{uncachedInput, cacheRead, outputTotal, freeTokens};
            const std::array<bool, 4> known{hasInput || used >= 0, hasCache, hasOutput, true};
            for (std::size_t i = 0; i < contextRows.size(); ++i) {
                if (!contextRows[i].tokens || !contextRows[i].percent) continue;
                contextRows[i].tokens->setText(known[i] ? FormatTokens(values[i]) : QStringLiteral("—"));
                contextRows[i].percent->setText(known[i] ? formatPct(values[i]) : QStringLiteral("—"));
            }
        } else {
            inspectorContextPercent->setText(QStringLiteral("暂无用量"));
            inspectorContextTokens->setText(limit > 0 ? QStringLiteral("上限 %1").arg(FormatTokens(limit)) : QStringLiteral("未选模型"));
            for (std::size_t i = 0; i < contextRows.size(); ++i) {
                if (!contextRows[i].tokens || !contextRows[i].percent) continue;
                if (i == 3 && limit > 0) {
                    contextRows[i].tokens->setText(FormatTokens(limit));
                    contextRows[i].percent->setText(QStringLiteral("100%"));
                } else {
                    contextRows[i].tokens->setText(QStringLiteral("—"));
                    contextRows[i].percent->setText(QStringLiteral("—"));
                }
            }
        }
    }
}

void AgentWindow::Private::UpdateInspectorEnvironment() {
    if (!rightInspector) return;
    if (envModeValue) {
        envModeValue->setText(mode == PresentationMode::Page ? QStringLiteral("整页")
            : mode == PresentationMode::Sidebar ? QStringLiteral("侧栏") : QStringLiteral("独立窗口"));
    }
    const QString mapName = map ? map->text() : QStringLiteral("新会话");
    const QString identity = map ? map->property("mapIdentity").toString() : QString();
    if (sidebarMapTitle) {
        sidebarMapTitle->setText(mapName == QStringLiteral("新会话") ? QStringLiteral("当前工作区") : mapName);
        sidebarMapTitle->setToolTip(identity);
    }
    if (envMapValue) {
        envMapValue->setText(mapName);
        envMapValue->setToolTip(identity);
    }
    if (envIdentityValue) {
        const QString summaryText = mapSummary ? mapSummary->text().trimmed() : QString();
        const QString shown = !identity.isEmpty() ? identity : (!summaryText.isEmpty() ? summaryText : QStringLiteral("未绑定地图修订"));
        envIdentityValue->setText(shown);
        envIdentityValue->setToolTip(shown);
    }
    if (envFollowBtn) {
        const QSignalBlocker blocker(envFollowBtn);
        envFollowBtn->setChecked(settings.followChanges);
        envFollowBtn->setText(settings.followChanges ? QStringLiteral("已开启") : QStringLiteral("关闭"));
    }
    if (inspectorBuiltinDot && inspectorBuiltinState) {
        const bool active = settings.aiEnabled;
        inspectorBuiltinDot->setProperty("active", active);
        inspectorBuiltinDot->style()->unpolish(inspectorBuiltinDot);
        inspectorBuiltinDot->style()->polish(inspectorBuiltinDot);
        inspectorBuiltinState->setText(!settings.aiEnabled ? QStringLiteral("未启用")
            : settings.disabledTools.isEmpty() ? QStringLiteral("已就绪")
            : QStringLiteral("禁用 %1 项").arg(settings.disabledTools.size()));
    }
    if (inspectorMcpDot && inspectorMcpBtn) {
        inspectorMcpDot->setProperty("active", settings.mcpEnabled);
        inspectorMcpDot->style()->unpolish(inspectorMcpDot);
        inspectorMcpDot->style()->polish(inspectorMcpDot);
        inspectorMcpBtn->setText(settings.mcpEnabled
            ? QStringLiteral("127.0.0.1:%1").arg(settings.mcpPort)
            : QStringLiteral("未开启 · 配置"));
    }
    if (sidebarAccount && sidebarStatusDot) {
        const bool ready = settings.aiEnabled && models && models->currentIndex() >= 0;
        sidebarStatusDot->setProperty("ready", ready);
        sidebarStatusDot->style()->unpolish(sidebarStatusDot);
        sidebarStatusDot->style()->polish(sidebarStatusDot);
        sidebarAccount->setText(ready ? models->currentText()
            : settings.aiEnabled ? QStringLiteral("尚未选择模型") : QStringLiteral("未启用 AI"));
        sidebarAccount->setToolTip(sidebarAccount->text());
    }
}

void AgentWindow::Private::UpdateInspectorChanges() {
    if (!inspectorChangesLayout || !inspectorDiffSummary) return;
    while (QLayoutItem* child = inspectorChangesLayout->takeAt(0)) {
        if (QWidget* w = child->widget()) w->deleteLater();
        delete child;
    }
    const bool hasTotals = std::any_of(totals.cbegin(), totals.cend(), [](qint64 v) { return v != 0; });
    if (hasTotals) {
        inspectorDiffSummary->setText(QStringLiteral("+%1  −%2").arg(totals[0]).arg(totals[1]));
    } else {
        inspectorDiffSummary->setText(QStringLiteral("无改动"));
    }
    if (recentChanges.isEmpty()) {
        auto* empty = new QLabel(hasTotals
            ? QStringLiteral("%1 个节点 · %2 个文件").arg(totals[2]).arg(totals[3])
            : QStringLiteral("本轮暂无地图改动"), inspectorChangesContainer);
        empty->setObjectName(QStringLiteral("agentInspectorMuted"));
        inspectorChangesLayout->addWidget(empty);
        return;
    }
    const qsizetype start = std::max<qsizetype>(0, recentChanges.size() - kMaximumInspectorRows);
    for (qsizetype i = recentChanges.size() - 1; i >= start; --i) {
        const auto& rec = recentChanges.at(i);
        auto* rowFrame = new QFrame(inspectorChangesContainer);
        rowFrame->setObjectName(QStringLiteral("agentInspectorItemRow"));
        auto* rowLayout = new QHBoxLayout(rowFrame);
        rowLayout->setContentsMargins(8, 5, 6, 5);
        rowLayout->setSpacing(6);
        auto* icon = new QLabel(rowFrame);
        icon->setFixedSize(13, 13);
        icon->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/file-code-2"), QColor(110, 231, 183)).pixmap(13, 13));
        rowLayout->addWidget(icon);
        auto* label = new UiKit::ElidedLabel(rec.title, rowFrame);
        label->setObjectName(QStringLiteral("agentInspectorLabel"));
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setToolTip(rec.title);
        rowLayout->addWidget(label, 1);
        if (!rec.kind.isEmpty() && !rec.objectId.isEmpty()) {
            auto* jump = new QPushButton(QStringLiteral("转到"), rowFrame);
            jump->setObjectName(QStringLiteral("agentInspectorJumpBtn"));
            jump->setCursor(Qt::PointingHandCursor);
            const QString kind = rec.kind;
            const QString objectId = rec.objectId;
            QObject::connect(jump, &QPushButton::clicked, q, [this, kind, objectId] {
                emit q->NavigateRequested(kind, objectId);
            });
            rowLayout->addWidget(jump);
        }
        inspectorChangesLayout->addWidget(rowFrame);
    }
}

void AgentWindow::Private::UpdateInspectorTools() {
    if (!inspectorToolsLayout || !inspectorToolsBadge) return;
    while (QLayoutItem* child = inspectorToolsLayout->takeAt(0)) {
        if (QWidget* w = child->widget()) w->deleteLater();
        delete child;
    }
    int runningCount = 0;
    for (auto it = tools.constBegin(); it != tools.constEnd(); ++it) {
        if (!it.value().finished) ++runningCount;
    }
    if (toolOrder.isEmpty()) {
        inspectorToolsBadge->setText(QStringLiteral("空闲"));
        auto* empty = new QLabel(QStringLiteral("暂无工具调用"), inspectorToolsContainer);
        empty->setObjectName(QStringLiteral("agentInspectorMuted"));
        inspectorToolsLayout->addWidget(empty);
        return;
    }
    inspectorToolsBadge->setText(runningCount > 0
        ? QStringLiteral("运行中 %1").arg(runningCount)
        : QStringLiteral("已完成 %1").arg(toolOrder.size()));

    const qsizetype start = std::max<qsizetype>(0, toolOrder.size() - kMaximumInspectorRows);
    for (qsizetype i = toolOrder.size() - 1; i >= start; --i) {
        const auto found = tools.constFind(toolOrder.at(i));
        if (found == tools.constEnd()) continue;
        const Tool& tool = found.value();
        auto* rowWidget = new QWidget(inspectorToolsContainer);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(7);
        auto* dot = new QLabel(rowWidget);
        dot->setObjectName(QStringLiteral("agentInspectorToolDot"));
        dot->setFixedSize(7, 7);
        const QString stateKind = !tool.finished ? QStringLiteral("running")
            : tool.state.contains(QStringLiteral("失败")) ? QStringLiteral("error") : QStringLiteral("ok");
        dot->setProperty("stateKind", stateKind);
        rowLayout->addWidget(dot);
        const QString shortTitle = ShortToolLabel(tool.event);
        auto* name = new UiKit::ElidedLabel(shortTitle, rowWidget);
        name->setObjectName(QStringLiteral("agentInspectorLabel"));
        name->setMinimumWidth(0);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name->setToolTip(shortTitle + QLatin1Char('\n') + tool.state);
        rowLayout->addWidget(name, 1);
        auto* dur = new QLabel(tool.duration.isEmpty() ? tool.state : tool.duration, rowWidget);
        dur->setObjectName(QStringLiteral("agentInspectorMuted"));
        dur->setFont(UiKit::MonoFont(11));
        rowLayout->addWidget(dur);
        inspectorToolsLayout->addWidget(rowWidget);
    }
}

void AgentWindow::Private::ModelChanged() {
    // 宽度跟随当前模型名，只占所需位置；过长名称省略显示，完整名在提示里。
    QFont font = models->font();
    font.setPixelSize(12);
    const QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(models->currentText());
    models->setFixedWidth(std::clamp(textWidth + 56, 96, 240));
    if (QAbstractItemView* popupView = models->view()) {
        popupView->setTextElideMode(Qt::ElideNone);
        int popupWidth = 196;
        for (int i = 0; i < models->count(); ++i) {
            popupWidth = std::max(popupWidth, metrics.horizontalAdvance(models->itemText(i)) + 56);
        }
        popupView->setMinimumWidth(std::clamp(popupWidth, 196, 320));
    }
    const QVariantMap selected = models->currentData().toMap();
    context->clear();
    ring->SetLimit(0, true, QString());
    for (const auto& provider : settings.providers) {
        if (provider.id != selected.value(QStringLiteral("provider_id")).toString()) continue;
        for (const auto& model : provider.models) {
            if (model.id != selected.value(QStringLiteral("model_id")).toString()) continue;
            const ContextWindow window = ResolveContextWindow(model);
            const double k = static_cast<double>(window.tokens) / 1024.0;
            context->setText(QStringLiteral("%1K%2").arg(k, 0, 'f', k == std::floor(k) ? 0 : 1)
                .arg(window.estimated ? QStringLiteral(" · 估算") : QString()));
            context->setToolTip(QStringLiteral("%1 tokens\n来源：%2").arg(QLocale().toString(window.tokens), window.source));
            ring->SetLimit(window.tokens, window.estimated, window.source);
            models->setToolTip(models->currentText() + QLatin1Char('\n') + context->toolTip());
        }
    }
    UpdateAvailability();
    UpdateContext();
    UpdateInspectorEnvironment();
}

void AgentWindow::Private::UpdateAvailability() {
    const bool configured = settings.aiEnabled && models->currentIndex() >= 0;
    const bool hasModels = settings.aiEnabled && models->count() > 0;
    models->setEnabled(hasModels && !busy);
    models->setVisible(hasModels);
    addModel->setVisible(!hasModels);
    addModel->setText(settings.aiEnabled ? QStringLiteral("选择模型") : QStringLiteral("启用 AI"));
    send->setEnabled(configured && !busy && !prompt->toPlainText().trimmed().isEmpty());
    send->setVisible(!busy);
    prompt->setEnabled(true); // 运行中仍可输入并用 Ctrl+Enter 排队
    attach->setEnabled(!busy);
    permissions->setEnabled(!busy);
    if (envPermissionBtn) envPermissionBtn->setEnabled(!busy);
    newConversation->setEnabled(!busy);
    if (sidebarNew) sidebarNew->setEnabled(!busy);
    sessionsButton->setEnabled(!busy);
    sessionsButton->setToolTip(busy ? QStringLiteral("任务运行中不能切换会话，请先停止并等待结束") : QStringLiteral("会话"));
    newConversation->setToolTip(busy ? QStringLiteral("任务运行中不能新建会话，请先停止并等待结束") : QStringLiteral("新会话"));
    if (busy) sessionsPopover->Dismiss();
    QString reason;
    if (!settings.aiEnabled) reason = QStringLiteral("在设置中启用 AI 并添加模型后即可开始");
    else if (models->currentIndex() < 0) reason = QStringLiteral("在设置中添加提供商并启用模型后即可开始");
    prompt->setPlaceholderText(reason.isEmpty() ? QStringLiteral("描述要完成的地图工作…") : reason);
    if (!busy && !configured) SetStatus(reason);
    send->setToolTip(configured ? QStringLiteral("发送 · Enter；换行 · Shift+Enter；运行中排队 · Ctrl+Enter") : reason);
    UpdateSendIcons();
    UpdateSummary();
    UpdateInspectorEnvironment();
}

void AgentWindow::Private::UpdateSendIcons() {
    const auto& theme = ResolveAgentTheme(q);
    const QColor ink = UiKit::ToQColor(theme.colors.bg);
    QColor faded = UiKit::ToQColor(theme.colors.muted);
    faded.setAlpha(140);
    send->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/arrow-up"), send->isEnabled() ? ink : faded));
    stop->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/square"), UiKit::ToQColor(theme.colors.text)));
}

void AgentWindow::Private::UpdatePermissions() {
    const std::size_t index = PermissionIndex(settings);
    const auto& level = kPermissionLevels[index];
    permissions->setText(QString::fromUtf8(level.title));
    permissions->setIcon(level.commands ? UiKit::SvgIcon(QString::fromLatin1(level.icon), QColor(234, 179, 100))
                                        : UiKit::SvgIcon(QString::fromLatin1(level.icon)));
    permissions->setProperty("level", QString::fromLatin1(level.id));
    permissions->style()->unpolish(permissions);
    permissions->style()->polish(permissions);
    if (envPermissionBtn) {
        envPermissionBtn->setText(QString::fromUtf8(level.title));
        envPermissionBtn->setProperty("level", QString::fromLatin1(level.id));
        envPermissionBtn->style()->unpolish(envPermissionBtn);
        envPermissionBtn->style()->polish(envPermissionBtn);
    }
    QStringList actual;
    actual << QStringLiteral("查询");
    if (settings.allowEdits) actual << QStringLiteral("编辑");
    if (settings.allowSave) actual << QStringLiteral("保存");
    if (settings.allowCommands) actual << QStringLiteral("外部命令");
    const QString tip = QStringLiteral("访问权限：%1\n实际权限由共用工具服务验证").arg(actual.join(QStringLiteral("、")));
    permissions->setToolTip(tip);
    if (envPermissionBtn) envPermissionBtn->setToolTip(tip);
    for (std::size_t i = 0; i < permissionOptions.size(); ++i) permissionOptions[i]->SetChecked(i == index);
}

void AgentWindow::Private::ChooseAttachments() {
    const QStringList files = QFileDialog::getOpenFileNames(q, QStringLiteral("添加文件"));
    for (const auto& file : files) AddAttachment(file);
    prompt->setFocus();
}

void AgentWindow::Private::AddAttachment(const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (absolute.isEmpty() || attachments.contains(absolute) || attachments.size() >= kMaximumAttachments) return;
    attachments.append(absolute);
    auto* chip = new QFrame(attachmentRow);
    chip->setObjectName(QStringLiteral("agentAttachmentChip"));
    chip->setProperty("path", absolute);
    chip->setToolTip(absolute);
    auto* layout = new QHBoxLayout(chip);
    layout->setContentsMargins(10, 3, 4, 3);
    layout->setSpacing(4);
    auto* icon = new QLabel(chip);
    icon->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/file")).pixmap(14, 14));
    layout->addWidget(icon);
    auto* name = new UiKit::ElidedLabel(QFileInfo(absolute).fileName(), chip);
    name->setMaximumWidth(200);
    layout->addWidget(name);
    auto* remove = new QToolButton(chip);
    remove->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/x")));
    remove->setIconSize(QSize(12, 12));
    remove->setFixedSize(18, 18);
    remove->setToolTip(QStringLiteral("移除"));
    remove->setAccessibleName(QStringLiteral("移除附件 %1").arg(QFileInfo(absolute).fileName()));
    QObject::connect(remove, &QToolButton::clicked, q, [this, absolute] { RemoveAttachment(absolute); });
    layout->addWidget(remove);
    attachmentLayout->insertWidget(attachmentLayout->count() - 1, chip);
    attachmentRow->show();
}

void AgentWindow::Private::RemoveAttachment(const QString& path) {
    attachments.removeAll(path);
    for (auto* chip : attachmentRow->findChildren<QFrame*>(QStringLiteral("agentAttachmentChip"), Qt::FindDirectChildrenOnly)) {
        if (chip->property("path").toString() == path) chip->deleteLater();
    }
    attachmentRow->setVisible(!attachments.isEmpty());
}

void AgentWindow::Private::UpdateGeometry() {
    if (conversation == nullptr || composerAxis == nullptr || centerWorkspace == nullptr) return;

    const bool allowRightRail = mode != PresentationMode::Sidebar && q->width() >= 980;
    const bool allowLeftRail = mode != PresentationMode::Sidebar && q->width() >= 1180;
    const bool showLeft = allowLeftRail && !leftRailCollapsed;
    const bool showRight = allowRightRail && !rightRailCollapsed;
    const int leftWidth = q->width() >= 1480 ? 236 : 206;
    const int rightWidth = q->width() >= 1480 ? 288 : 252;

    if (leftSidebar) {
        leftSidebar->setFixedWidth(leftWidth);
        leftSidebar->setVisible(showLeft);
    }
    if (rightInspector) {
        rightInspector->setFixedWidth(rightWidth);
        rightInspector->setVisible(showRight);
    }
    if (expandLeftRail) expandLeftRail->setVisible(allowLeftRail && leftRailCollapsed);
    if (toggleRightRail) {
        toggleRightRail->setVisible(allowRightRail);
        const QSignalBlocker blocker(toggleRightRail);
        toggleRightRail->setChecked(showRight);
    }

    if (q->layout()) q->layout()->activate();
    if (centerWorkspace->layout()) centerWorkspace->layout()->activate();

    const int centerWidth = std::max(280, q->width() - (showLeft ? leftWidth : 0) - (showRight ? rightWidth : 0));
    const int viewportWidth = (conversation->isVisible() && conversation->viewport()->width() > 0)
        ? std::min(centerWidth, conversation->viewport()->width())
        : centerWidth;
    const int available = std::max(220, viewportWidth - 32);
    const int body = mode == PresentationMode::Sidebar || centerWidth < 740 ? available
        : std::min(available, std::clamp(static_cast<int>(centerWidth * 0.88), 680, 760));
    conversation->SetContentWidth(body);
    // 宽页切换为侧栏时，上一尺寸不能成为父布局的新最小宽度。
    composerAxis->setMinimumWidth(0);
    composerAxis->setMaximumWidth(std::min(centerWidth - 24, body + 32));
    ring->setVisible(true);
    QFont modelFont = models->font();
    modelFont.setPixelSize(12);
    const int requestedModelWidth = QFontMetrics(modelFont).horizontalAdvance(models->currentText()) + 56;
    const int modelLimit = centerWidth < 460 ? std::max(96, centerWidth - 240) : 240;
    models->setFixedWidth(std::clamp(requestedModelWidth, 96, modelLimit));
    const bool compact = centerWidth < 560;
    changeDetails->setVisible(!compact);
    if (compact != compactSummary) { compactSummary = compact; UpdateSummary(); }
    permissions->setToolButtonStyle(centerWidth >= 460 ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    elapsed->setToolTip(usage->text());
    UpdatePromptHeight();
}

void AgentWindow::Private::UpdatePromptHeight() {
    if (prompt == nullptr || resizingPrompt) return;
    resizingPrompt = true;
    const int lines = std::max(1, prompt->document()->lineCount());
    const int maximum = std::min(336, std::max(120, q->height() * 2 / 5));
    const int wanted = std::clamp(lines * 24 + 6, 46, maximum);
    if (prompt->height() != wanted) prompt->setFixedHeight(wanted);
    resizingPrompt = false;
}

void AgentWindow::Private::UpdateSummary() {
    const bool hasChanges = std::any_of(totals.cbegin(), totals.cend(), [](qint64 value) { return value != 0; });
    changeSummary->setVisible(hasChanges);
    outputSummary->setVisible(outputCharacters > 0 && !compactSummary);
    // 空闲且没有本轮结果时整行隐藏；未配置的原因已经在输入框占位文字里。
    const bool configured = settings.aiEnabled && models->currentIndex() >= 0;
    const bool idle = !busy && (status->text() == QStringLiteral("就绪") || !configured);
    elapsed->setVisible(runElapsed.isValid());
    activity->setVisible(hasChanges || outputCharacters > 0 || !idle);
}

bool AgentWindow::Private::TakePrompt(QueuedPrompt& out) {
    const QString text = prompt->toPlainText().trimmed();
    if (text.isEmpty()) return false;
    // 附件只以本机路径交给助手；能否读取由助手工具与权限决定，不在这里读文件内容。
    QString request = text;
    QString shown = text;
    if (!attachments.isEmpty()) {
        request += QStringLiteral("\n\n参考文件：");
        QStringList names;
        for (const auto& path : attachments) {
            request += QStringLiteral("\n- ") + QDir::toNativeSeparators(path);
            names << QFileInfo(path).fileName();
        }
        shown += QStringLiteral("\n\n附件：") + names.join(QStringLiteral("、"));
    }
    out = {request, shown};
    prompt->clear();
    for (const auto& path : QStringList(attachments)) RemoveAttachment(path);
    return true;
}

void AgentWindow::Private::Dispatch(const QueuedPrompt& next) {
    if (busy || !settings.aiEnabled || models->currentIndex() < 0) return;
    const QVariantMap selected = models->currentData().toMap();
    Flush();
    conversation->EndMessage();
    conversation->BeginMessage(QStringLiteral("你"), true);
    conversation->AppendText(next.shown);
    conversation->EndMessage();
    assistantOpen = false;
    UpdateQueue();
    q->SetBusy(true);
    emit q->PromptSubmitted(next.request, selected.value(QStringLiteral("provider_id")).toString(), selected.value(QStringLiteral("model_id")).toString());
}

void AgentWindow::Private::Submit() {
    QGuiApplication::inputMethod()->commit();
    if (busy) {
        if (!prompt->toPlainText().trimmed().isEmpty()) SetStatus(QStringLiteral("运行中 · Ctrl+Enter 排队，或先停止"));
        return;
    }
    if (!settings.aiEnabled || models->currentIndex() < 0) return;
    QueuedPrompt next;
    if (TakePrompt(next)) Dispatch(next);
}

void AgentWindow::Private::Queue() {
    QGuiApplication::inputMethod()->commit();
    if (!busy) {
        // 空闲时等同发送；输入为空而有暂停的排队消息时，继续发送下一条。
        if (prompt->toPlainText().trimmed().isEmpty() && !queued.isEmpty()) {
            if (settings.aiEnabled && models->currentIndex() >= 0) Dispatch(queued.takeFirst());
            return;
        }
        Submit();
        return;
    }
    QueuedPrompt next;
    if (TakePrompt(next)) { queued.append(next); UpdateQueue(); }
}

void AgentWindow::Private::UpdateQueue(bool paused) {
    queueNotice->setVisible(!queued.isEmpty());
    if (queued.isEmpty()) return;
    const auto count = QString::number(queued.size());
    queueNotice->setText(paused
        ? QStringLiteral("已排队 %1 条 · 本轮未正常结束，已暂停　<a href=\"next\">发送下一条</a>　<a href=\"clear\">清空</a>").arg(count)
        : QStringLiteral("已排队 %1 条 · 本轮结束后依次发送　<a href=\"clear\">清空</a>").arg(count));
}

void AgentWindow::Private::ContinueQueue(bool resumable) {
    if (queued.isEmpty()) return;
    if (!resumable) { UpdateQueue(true); return; }
    // 控制器在发布 run_end 之后才释放运行状态；下一轮放到事件循环再发。
    QTimer::singleShot(0, q, [this] { if (!busy && !queued.isEmpty()) Dispatch(queued.takeFirst()); });
}

void AgentWindow::Private::SetStatus(const QString& text, bool failed) {
    status->setText(text);
    status->setProperty("failed", failed);
    status->style()->unpolish(status);
    status->style()->polish(status);
    activityDot->setProperty("running", busy && !failed);
    activityDot->setProperty("failed", failed);
    activityDot->style()->unpolish(activityDot);
    activityDot->style()->polish(activityDot);
    UpdateSummary();
}
void AgentWindow::Private::BeginAssistant() {
    conversation->BeginMessage(QStringLiteral("助手"));
    assistantOpen = true;
}
void AgentWindow::Private::Flush() {
    flushTimer.stop();
    if (pending.isEmpty()) return;
    if (!assistantOpen) BeginAssistant();
    conversation->AppendText(pending);
    pending.clear();
}

void AgentWindow::Private::UpdateTool(const QJsonObject& event, const QString& type) {
    const QString id = event.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) { SetStatus(QStringLiteral("工具事件缺少调用身份"), true); return; }
    auto found = tools.find(id);
    if (found == tools.end()) {
        Tool tool;
        tool.elapsed.start();
        found = tools.insert(id, tool);
        toolOrder.append(id);
    }
    Tool& tool = found.value();
    for (auto it = event.constBegin(); it != event.constEnd(); ++it) tool.event.insert(it.key(), it.value());
    if (type == QStringLiteral("tool_end")) {
        tool.finished = true;
        const bool failed = event.value(QStringLiteral("is_error")).toBool()
            || (event.contains(QStringLiteral("success")) && !event.value(QStringLiteral("success")).toBool());
        tool.state = failed ? QStringLiteral("失败") : event.value(QStringLiteral("success")).toBool()
            ? QStringLiteral("成功") : QStringLiteral("已结束");
    } else if (!tool.finished) {
        tool.state = QStringLiteral("运行中");
    }
    tool.duration = DurationText(event.contains(QStringLiteral("duration_ms"))
        ? Count(event.value(QStringLiteral("duration_ms"))) : tool.elapsed.elapsed());
    conversation->UpdateTool(tool.event, tool.state, tool.duration);
    UpdateInspectorTools();
}

void AgentWindow::Private::UpdateChanges(const QJsonObject& event) {
    const std::array<QString, 4> keys{QStringLiteral("added_lines"), QStringLiteral("removed_lines"), QStringLiteral("trigger_nodes"), QStringLiteral("files")};
    const bool absolute = event.value(QStringLiteral("mode")).toString() == QStringLiteral("total");
    for (std::size_t i = 0; i < totals.size(); ++i) {
        if (!IsExactCount(event.value(keys[i]))) continue;
        const qint64 value = Count(event.value(keys[i]));
        totals[i] = absolute ? value : AddCount(totals[i], value);
        counters[i]->SetTarget(totals[i]);
    }
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    for (const auto& val : items) {
        const QJsonObject item = val.toObject();
        QString title = item.value(QStringLiteral("title")).toString().simplified();
        if (title.isEmpty()) title = item.value(QStringLiteral("target")).toString().simplified();
        if (title.isEmpty()) title = QStringLiteral("地图改动");
        recentChanges.append({item.value(QStringLiteral("kind")).toString(),
            item.value(QStringLiteral("object_id")).toString(), title});
        if (recentChanges.size() > 32) recentChanges.removeFirst();
    }
    conversation->AppendChanges(items);
    UpdateSummary();
    UpdateInspectorChanges();
    if (!settings.followChanges) return;
    for (qsizetype i = items.size() - 1; i >= 0; --i) {
        const QJsonObject item = items.at(i).toObject();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString objectId = item.value(QStringLiteral("object_id")).toString();
        if (kind.isEmpty() || objectId.isEmpty()) continue;
        emit q->FollowRequested(kind, objectId);
        return;
    }
}

void AgentWindow::Private::UpdateElapsed() {
    if (runElapsed.isValid()) elapsed->setText(DurationText(runElapsed.elapsed()));
}

void AgentWindow::Private::RestoreHistory(const QJsonObject& event) {
    q->ClearConversation();
    QJsonArray messages;
    for (const auto& value : event.value(QStringLiteral("messages")).toArray())
        if (value.isObject() && !IsHiddenAgentMessage(value.toObject())) messages.append(value);
    const qsizetype first = std::max<qsizetype>(0, messages.size() - kMaximumHistoryMessages);
    historyNotice->setText(first > 0 ? QStringLiteral("已恢复本地会话 · 最近 %1 条，较早记录保留在本地").arg(messages.size() - first)
        : QStringLiteral("已恢复本地会话 · %1 条消息").arg(messages.size()));
    historyNotice->show();
    qsizetype remaining = kMaximumHistoryCharacters;
    bool limited = false;
    QHash<QString, QJsonObject> restored;
    for (qsizetype i = first; i < messages.size(); ++i) {
        const QJsonObject message = messages.at(i).toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        if (role == QStringLiteral("assistant")) {
            for (const auto& value : message.value(QStringLiteral("content")).toArray()) {
                const auto block = value.toObject();
                if (block.value(QStringLiteral("type")).toString() != QStringLiteral("toolCall")) continue;
                if (restored.size() >= 64) { limited = true; break; }
                const QString id = QStringLiteral("history:") + block.value(QStringLiteral("id")).toString();
                QJsonObject restoredEvent{{QStringLiteral("id"), id}, {QStringLiteral("title"), QStringLiteral("历史 · %1").arg(block.value(QStringLiteral("name")).toString())},
                    {QStringLiteral("arguments"), block.value(QStringLiteral("arguments"))}, {QStringLiteral("history"), true}};
                restored.insert(id, restoredEvent);
                conversation->UpdateTool(restoredEvent, QStringLiteral("历史调用"), QStringLiteral("历史"));
            }
        }
        if (role == QStringLiteral("toolResult")) {
            const QString id = QStringLiteral("history:") + message.value(QStringLiteral("toolCallId")).toString();
            if (restored.size() >= 64 && !restored.contains(id)) { limited = true; continue; }
            QJsonObject restoredEvent = restored.value(id);
            restoredEvent.insert(QStringLiteral("id"), id);
            if (!restoredEvent.contains(QStringLiteral("title"))) restoredEvent.insert(QStringLiteral("title"), QStringLiteral("历史 · %1").arg(message.value(QStringLiteral("toolName")).toString()));
            restoredEvent.insert(QStringLiteral("result"), message.value(QStringLiteral("content")));
            restoredEvent.insert(QStringLiteral("history"), true);
            restored.insert(id, restoredEvent);
            conversation->UpdateTool(restoredEvent, message.value(QStringLiteral("isError")).toBool()
                ? QStringLiteral("失败（历史）") : QStringLiteral("已结束（历史）"), QStringLiteral("历史"));
            continue;
        }
        if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) continue;
        QString text = HistoryText(message.value(QStringLiteral("content")));
        if (text.isEmpty()) text = message.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) continue;
        conversation->BeginMessage(role == QStringLiteral("user") ? QStringLiteral("你（历史）") : QStringLiteral("助手（历史）"), role == QStringLiteral("user"));
        if (text.size() <= remaining) { conversation->AppendText(text); remaining -= text.size(); }
        else { conversation->AppendText(QStringLiteral("消息超出历史显示预算，完整内容保留在本地。")); limited = true; }
        conversation->EndMessage();
    }
    if (limited) historyNotice->setText(historyNotice->text() + QStringLiteral(" · 已达展示预算"));
    assistantOpen = false;
}

AgentWindow::AgentWindow(QWidget* parent) : QWidget(parent, Qt::Window), d_(std::make_unique<Private>(this)) {
    d_->Build();
    SetPresentationMode(PresentationMode::Detached);
    SetSettings(AgentSettings{});
}
AgentWindow::~AgentWindow() = default;

void AgentWindow::SetSettings(const AgentSettings& settings) {
    const QVariant previous = d_->models->currentData();
    d_->settings = settings;
    d_->followButton->setChecked(settings.followChanges);
    {
        const QSignalBlocker blocker(d_->models);
        d_->models->clear();
        for (const auto& provider : settings.providers) {
            if (!provider.enabled) continue;
            for (const auto& model : provider.models) {
                if (!model.enabled || model.id.isEmpty()) continue;
                const QString modelName = model.name.isEmpty() ? model.id : model.name;
                d_->models->addItem(modelName, QVariantMap{{QStringLiteral("provider_id"), provider.id}, {QStringLiteral("model_id"), model.id}});
                d_->models->setItemData(d_->models->count() - 1, (provider.name.isEmpty() ? provider.id : provider.name) + QStringLiteral(" / ") + modelName, Qt::ToolTipRole);
            }
        }
        const int prior = d_->models->findData(previous);
        if (prior >= 0) d_->models->setCurrentIndex(prior);
    }
    d_->conversation->SetReducedMotion(settings.reducedMotion);
    d_->sessionsPopover->SetReducedMotion(settings.reducedMotion);
    d_->contextPopover->SetReducedMotion(settings.reducedMotion);
    for (auto* counter : d_->counters) counter->SetReducedMotion(settings.reducedMotion);
    d_->output->SetReducedMotion(settings.reducedMotion);
    d_->UpdatePermissions();
    d_->ModelChanged();
    if (!d_->busy && settings.aiEnabled && d_->models->count() > 0) d_->SetStatus(QStringLiteral("就绪"));
    d_->UpdateGeometry();
}
void AgentWindow::SetActiveMap(QString title, QString identity) {
    d_->map->setText(title.isEmpty() ? QStringLiteral("新会话") : title);
    d_->map->setToolTip(identity);
    d_->map->setProperty("mapIdentity", identity);
    d_->UpdateInspectorEnvironment();
}
void AgentWindow::SetMapSummary(QString summary) {
    d_->mapSummary->setText(std::move(summary));
    d_->mapSummary->setVisible(d_->mode == PresentationMode::Page && !d_->mapSummary->text().isEmpty());
    d_->UpdateInspectorEnvironment();
}
void AgentWindow::SetPresentationMode(PresentationMode mode) {
    d_->mode = mode;
    d_->sessionsPopover->Dismiss();
    d_->contextPopover->Dismiss();
    d_->mapSummary->setVisible(mode == PresentationMode::Page && !d_->mapSummary->text().isEmpty());
    d_->back->setVisible(mode != PresentationMode::Detached);
    const QString backText = mode == PresentationMode::Sidebar ? QStringLiteral("收起 AI 侧栏") : QStringLiteral("返回地图编辑");
    d_->back->setToolTip(backText);
    d_->back->setAccessibleName(backText);
    d_->modeActions[0]->setChecked(mode == PresentationMode::Page);
    d_->modeActions[1]->setChecked(mode == PresentationMode::Sidebar);
    d_->modeActions[2]->setChecked(mode == PresentationMode::Detached);
    d_->UpdateInspectorEnvironment();
    d_->UpdateGeometry();
}
AgentWindow::PresentationMode AgentWindow::Mode() const noexcept { return d_->mode; }
void AgentWindow::SetBusy(bool busy) {
    if (d_->busy == busy) return;
    d_->busy = busy;
    d_->stop->setVisible(busy);
    d_->stop->setEnabled(busy);
    if (busy) {
        d_->stopped = false;
        d_->runElapsed.restart();
        d_->elapsedTimer.start();
        d_->SetStatus(QStringLiteral("运行中"));
    } else {
        d_->Flush();
        d_->UpdateElapsed();
        d_->elapsedTimer.stop();
        d_->assistantOpen = false;
        d_->conversation->EndMessage();
        if (d_->status->text() == QStringLiteral("运行中")) d_->SetStatus(QStringLiteral("就绪"));
        for (auto* counter : d_->counters) counter->Finish();
        d_->output->Finish();
        d_->ContinueQueue(!d_->stopped && !d_->status->property("failed").toBool()
            && d_->status->text() != QStringLiteral("已停止"));
    }
    d_->UpdateAvailability();
    d_->UpdateSessions();
}
void AgentWindow::ClearConversation() {
    d_->flushTimer.stop();
    d_->pending.clear();
    d_->assistantOpen = false;
    d_->tools.clear();
    d_->toolOrder.clear();
    d_->recentChanges.clear();
    d_->conversation->Clear();
    d_->historyNotice->hide();
    d_->historyNotice->clear();
    d_->totals.fill(0);
    d_->outputCharacters = 0;
    for (auto* counter : d_->counters) counter->SetValue(0);
    d_->output->SetValue(0);
    d_->usage->setText(QStringLiteral("上下文用量未提供"));
    d_->actualUsage = {};
    d_->ring->ClearUsed();
    d_->UpdateContext();
    d_->UpdateInspectorChanges();
    d_->UpdateInspectorTools();
    d_->UpdateSummary();
    if (!d_->busy) { d_->runElapsed.invalidate(); d_->elapsed->setText(QStringLiteral("—")); }
}

void AgentWindow::AppendEvent(const QJsonObject& event) {
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("conversation_reset")) { ClearConversation(); return; }
    if (type == QStringLiteral("conversations")) {
        d_->sessions = event.value(QStringLiteral("sessions")).toArray();
        d_->selectedSession = event.value(QStringLiteral("selected_id")).toString();
        d_->UpdateSessions(); return;
    }
    if (type == QStringLiteral("history_restored")) { d_->RestoreHistory(event); return; }
    if (type == QStringLiteral("run_start")) { SetBusy(true); return; }
    if (type == QStringLiteral("assistant_delta")) {
        const QString text = event.value(QStringLiteral("text")).toString();
        if (!d_->assistantOpen && d_->pending.isEmpty()) d_->BeginAssistant();
        d_->pending += text;
        d_->outputCharacters = AddCount(d_->outputCharacters, static_cast<qint64>(text.size()));
        d_->output->SetTarget(d_->outputCharacters);
        d_->UpdateSummary();
        if (!d_->flushTimer.isActive()) d_->flushTimer.start();
        return;
    }
    if (type == QStringLiteral("assistant_message")) {
        d_->Flush();
        if (!d_->assistantOpen) d_->BeginAssistant();
        d_->conversation->ReplaceText(event.value(QStringLiteral("text")).toString());
        return;
    }
    if (type == QStringLiteral("tool_start") || type == QStringLiteral("tool_update") || type == QStringLiteral("tool_end")) {
        if (type == QStringLiteral("tool_start")) { d_->Flush(); d_->assistantOpen = false; d_->conversation->EndMessage(); }
        d_->UpdateTool(event, type);
        return;
    }
    if (type == QStringLiteral("changes")) { d_->UpdateChanges(event); return; }
    if (type == QStringLiteral("usage")) {
        const QJsonValue available = event.value(QStringLiteral("available"));
        if (available.isBool() && !available.toBool()) {
            d_->usage->setText(QStringLiteral("上下文用量暂不可用"));
            d_->ring->ClearUsed();
            d_->actualUsage = {};
            d_->UpdateContext();
            d_->UpdateGeometry();
            return;
        }
        d_->actualUsage = event;
        const auto input = event.value(QStringLiteral("input_tokens"));
        const auto output = event.value(QStringLiteral("output_tokens"));
        const auto contextUsed = event.value(QStringLiteral("context_tokens"));
        const auto totalUsed = event.value(QStringLiteral("total_tokens"));
        if (IsExactCount(input) || IsExactCount(output)) {
            d_->usage->setText(QStringLiteral("输入 %1 · 输出 %2 tokens")
                .arg(IsExactCount(input) ? QLocale().toString(Count(input)) : QStringLiteral("—"),
                    IsExactCount(output) ? QLocale().toString(Count(output)) : QStringLiteral("—")));
        }
        if (IsExactCount(contextUsed)) d_->ring->SetUsed(Count(contextUsed));
        else if (IsExactCount(totalUsed)) d_->ring->SetUsed(Count(totalUsed));
        else if (IsExactCount(input) && IsExactCount(output)) d_->ring->SetUsed(AddCount(Count(input), Count(output)));
        else d_->ring->ClearUsed();
        d_->UpdateContext();
        d_->UpdateGeometry();
        return;
    }
    if (type == QStringLiteral("error")) {
        d_->Flush();
        d_->conversation->BeginMessage(QStringLiteral("错误"));
        d_->conversation->AppendText(event.value(QStringLiteral("text")).toString(QStringLiteral("运行失败")));
        d_->conversation->EndMessage();
        d_->assistantOpen = false;
        d_->SetStatus(QStringLiteral("运行失败"), true);
        return;
    }
    if (type == QStringLiteral("run_end")) {
        d_->Flush();
        const bool cancelled = event.value(QStringLiteral("cancelled")).toBool();
        const bool failed = event.value(QStringLiteral("is_error")).toBool()
            || (event.contains(QStringLiteral("success")) && !event.value(QStringLiteral("success")).toBool());
        for (auto it = d_->tools.begin(); it != d_->tools.end(); ++it) {
            auto& tool = it.value();
            if (tool.finished) continue;
            tool.finished = true;
            tool.state = cancelled ? QStringLiteral("已停止") : QStringLiteral("未收到结果");
            tool.duration = DurationText(tool.elapsed.elapsed());
            d_->conversation->UpdateTool(tool.event, tool.state, tool.duration);
        }
        d_->UpdateInspectorTools();
        d_->SetStatus(cancelled ? QStringLiteral("已停止") : failed ? QStringLiteral("运行失败")
            : event.value(QStringLiteral("success")).toBool() ? QStringLiteral("本轮完成") : QStringLiteral("本轮结束"), failed);
        SetBusy(false);
    }
}
void AgentWindow::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); d_->UpdateGeometry(); }
void AgentWindow::closeEvent(QCloseEvent* event) { QWidget::closeEvent(event); }
void AgentWindow::showEvent(QShowEvent* event) { QWidget::showEvent(event); d_->UpdateGeometry(); emit VisibilityChanged(true); }
void AgentWindow::hideEvent(QHideEvent* event) {
    d_->sessionsPopover->Dismiss(); d_->contextPopover->Dismiss();
    QWidget::hideEvent(event); emit VisibilityChanged(false);
}
void AgentWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { emit BackRequested(); event->accept(); return; }
    QWidget::keyPressEvent(event);
}
void AgentWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && !d_->applyingStyleSheet) {
        d_->ApplyAgentStyleSheet();
        d_->UpdateSendIcons();
        d_->UpdateSessions();
    }
}

} // namespace War3Map::Agent

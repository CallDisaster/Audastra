#include "AgentWindow.h"

#include "AgentConversationView.h"
#include "AgentSessionStore.h"
#include "AnimatedCounter.h"
#include "ComposerWidgets.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Widgets/ElidedLabel.h"
#include "Widgets/Motion.h"
#include "Widgets/PopoverSurface.h"

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QHideEvent>
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

} // namespace

struct AgentWindow::Private {
    struct Tool {
        QElapsedTimer elapsed;
        QJsonObject event;
        QString state;
        bool finished = false;
    };
    explicit Private(AgentWindow* owner) : q(owner) {}
    AgentWindow* q;
    AgentSettings settings;
    PresentationMode mode = PresentationMode::Detached;
    QToolButton* back = nullptr;
    QToolButton* newConversation = nullptr;
    QToolButton* sessionsButton = nullptr;
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

    void Build();
    void ModelChanged();
    void UpdateAvailability();
    void UpdateGeometry();
    void UpdatePromptHeight();
    void UpdateSummary();
    void UpdatePermissions();
    void UpdateSendIcons();
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

void AgentWindow::Private::Build() {
    q->setObjectName(QStringLiteral("agentWindow"));
    q->setWindowTitle(QStringLiteral("AI 工作区 — Audastra"));
    q->setAttribute(Qt::WA_DeleteOnClose, false);
    q->setMinimumSize(360, 420);
    q->resize(1080, 820);
    auto* root = new QVBoxLayout(q);
    root->setSizeConstraint(QLayout::SetNoConstraint);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QFrame(q);
    header->setObjectName(QStringLiteral("agentConversationHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 8, 12, 8);
    headerLayout->setSpacing(4);
    back = new QToolButton(header);
    back->setObjectName(QStringLiteral("agentBack"));
    back->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/arrow-left")));
    back->setIconSize(QSize(18, 18));
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
    sessionsButton->setToolTip(QStringLiteral("会话"));
    sessionsButton->setAccessibleName(QStringLiteral("本地图会话"));
    QObject::connect(sessionsButton, &QToolButton::clicked, q, [this] {
        if (!busy) sessionsPopover->ToggleAnchored(sessionsButton, UiKit::PopoverSurface::PreferredSide::Below, Qt::AlignRight);
    });
    headerLayout->addWidget(sessionsButton);
    newConversation = new QToolButton(header);
    newConversation->setObjectName(QStringLiteral("agentNewConversation"));
    newConversation->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/plus")));
    newConversation->setToolTip(QStringLiteral("新会话"));
    newConversation->setAccessibleName(QStringLiteral("新建 AI 会话"));
    QObject::connect(newConversation, &QToolButton::clicked, q, &AgentWindow::NewConversationRequested);
    headerLayout->addWidget(newConversation);
    auto* presentation = new QToolButton(header);
    presentation->setObjectName(QStringLiteral("agentPresentation"));
    presentation->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/square")));
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
    settingsButton->setToolTip(QStringLiteral("AI 与 MCP 设置"));
    settingsButton->setAccessibleName(QStringLiteral("AI 与 MCP 设置"));
    QObject::connect(settingsButton, &QToolButton::clicked, q, &AgentWindow::SettingsRequested);
    headerLayout->addWidget(settingsButton);
    root->addWidget(header);
    mapSummary = new QLabel(q);
    mapSummary->setObjectName(QStringLiteral("agentMapSummary"));
    mapSummary->setContentsMargins(16, 4, 16, 4);
    mapSummary->setWordWrap(true);
    mapSummary->hide();
    root->addWidget(mapSummary);
    BuildSessionPopover();
    historyNotice = new QLabel(q);
    historyNotice->setObjectName(QStringLiteral("agentHistoryNotice"));
    historyNotice->setWordWrap(true);
    historyNotice->setContentsMargins(16, 7, 16, 7);
    historyNotice->hide();
    root->addWidget(historyNotice);

    conversation = new AgentConversationView(q);
    QObject::connect(conversation, &AgentConversationView::NavigateRequested, q, &AgentWindow::NavigateRequested);
    QObject::connect(conversation, &AgentConversationView::SuggestionChosen, q, [this](const QString& text) {
        if (busy) return;
        prompt->setPlainText(text);
        QTextCursor cursor = prompt->textCursor();
        cursor.movePosition(QTextCursor::End);
        prompt->setTextCursor(cursor);
        prompt->setFocus();
    });
    root->addWidget(conversation, 1);

    auto* composerContainer = new QWidget(q);
    auto* centered = new QHBoxLayout(composerContainer);
    centered->setContentsMargins(16, 0, 16, 16);
    composerAxis = new QWidget(composerContainer);
    composerAxis->setObjectName(QStringLiteral("agentComposerAxis"));
    composerAxis->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* axis = new QVBoxLayout(composerAxis);
    axis->setSizeConstraint(QLayout::SetNoConstraint);
    axis->setContentsMargins(0, 0, 0, 0);
    axis->setSpacing(8);
    centered->addStretch();
    centered->addWidget(composerAxis, 1);
    centered->addStretch();

    // 输入框上方的活动行：运行状态、耗时，右侧为本轮改动计数。空闲且无改动时隐藏。
    auto* summary = new QWidget(composerAxis);
    summary->setObjectName(QStringLiteral("agentChangeSummary"));
    activity = summary;
    auto* summaryLayout = new QHBoxLayout(summary);
    summaryLayout->setContentsMargins(14, 0, 14, 0);
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
    cardLayout->setContentsMargins(16, 12, 10, 10);
    cardLayout->setSpacing(10);
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
    prompt->setFixedHeight(48);
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
    attach->setIconSize(QSize(18, 18));
    attach->setFixedSize(32, 32);
    attach->setToolTip(QStringLiteral("添加文件"));
    attach->setAccessibleName(QStringLiteral("添加文件"));
    attach->setCursor(Qt::PointingHandCursor);
    QObject::connect(attach, &QToolButton::clicked, q, [this] { ChooseAttachments(); });
    controls->addWidget(attach);

    permissions = new QToolButton(card);
    permissions->setObjectName(QStringLiteral("agentPermissions"));
    permissions->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    permissions->setIconSize(QSize(15, 15));
    permissions->setFixedHeight(32);
    permissions->setPopupMode(QToolButton::InstantPopup);
    permissions->setCursor(Qt::PointingHandCursor);
    permissions->setAccessibleName(QStringLiteral("访问权限"));
    permissionMenu = new QMenu(permissions);
    permissionMenu->setObjectName(QStringLiteral("agentPermissionMenu"));
    auto* menuTitle = new QLabel(QStringLiteral("助手可以做什么？"));
    menuTitle->setObjectName(QStringLiteral("agentPermissionMenuTitle"));
    menuTitle->setContentsMargins(14, 8, 14, 4);
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
    models->setFixedHeight(32);
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
    addModel->setFixedHeight(32);
    addModel->setCursor(Qt::PointingHandCursor);
    addModel->setToolTip(QStringLiteral("在设置中添加提供商并启用模型"));
    QObject::connect(addModel, &QToolButton::clicked, q, &AgentWindow::SettingsRequested);
    controls->addWidget(addModel);
    // 数值标签保留给辅助技术与测试；界面上由圆环表示。
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
    stop->setFixedSize(32, 32);
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
    send->setIconSize(QSize(17, 17));
    send->setFixedSize(32, 32);
    send->setCursor(Qt::PointingHandCursor);
    send->setAccessibleName(QStringLiteral("发送任务"));
    QObject::connect(send, &QPushButton::clicked, q, [this] { Submit(); });
    controls->addWidget(send);
    cardLayout->addLayout(controls);
    axis->addWidget(card);
    root->addWidget(composerContainer);

    q->setStyleSheet(QStringLiteral(
        "QFrame#agentConversationHeader { border:0; border-bottom:1px solid palette(mid); }"
        "QLabel#agentHeaderTitle { font-size:14px; font-weight:500; }"
        "QLabel#agentActiveMap { font-size:13px; color:palette(placeholder-text); padding-left:2px; }"
        "QLabel#agentHistoryNotice, QLabel#agentMessageAnnotation, QLabel#agentMapSummary { font-size:12px; color:palette(placeholder-text); }"
        "QToolButton#agentBack, QToolButton#agentConversations, QToolButton#agentNewConversation, QToolButton#agentPresentation, QToolButton#agentSettings {"
        " border:0; border-radius:16px; background:transparent; padding:0;"
        " min-width:32px; max-width:32px; min-height:32px; max-height:32px; }"
        "QToolButton#agentBack:hover, QToolButton#agentConversations:hover, QToolButton#agentNewConversation:hover, QToolButton#agentPresentation:hover,"
        " QToolButton#agentSettings:hover { background:rgba(255,255,255,0.07); }"
        "QToolButton#agentPresentation::menu-indicator { image:none; width:0; }"
        "QWidget#agentConversationCanvas, QWidget#agentReadingColumn { background:transparent; }"
        "QScrollArea#agentConversation { border:0; background:transparent; }"
        "QLabel#agentEmptyTitle { font-size:28px; font-weight:500; }"
        "QLabel#agentEmptyHint { font-size:14px; color:palette(placeholder-text); }"
        "QPushButton#agentSuggestion { border:1px solid palette(mid); border-radius:17px; background:transparent;"
        " padding:7px 16px; font-size:13px; color:palette(placeholder-text); }"
        "QPushButton#agentSuggestion:hover { color:palette(text); background:rgba(255,255,255,0.05); }"
        "QFrame#agentUserBubble { background:palette(button); border:0; border-radius:18px; }"
        "QFrame#agentAssistantBody { background:transparent; border:0; }"
        "QTextBrowser#agentMessageBody { background:transparent; border:0; padding:0; font-size:15px; }"
        "QLabel#agentToolTitle { font-size:13px; color:palette(placeholder-text); }"
        "QLabel#agentToolState, QLabel#agentToolDuration { font-size:12px; color:palette(placeholder-text); }"
        "QToolButton#agentToolToggle, QToolButton#agentChangeToggle { border:0; background:transparent; padding:0; border-radius:11px; }"
        "QTreeWidget#agentToolDetails, QTreeWidget#agentChangeDetails { border:1px solid palette(mid); border-radius:10px;"
        " background:transparent; font-size:12px; padding:4px; }"
        "QPlainTextEdit#agentInlineDetailPreview { border:1px solid palette(mid); border-radius:10px; font-size:12px; }"
        "QPushButton#agentInlineNavigate { border:0; background:transparent; color:palette(link); padding:0 4px; font-size:12px; }"
        "QFrame#agentComposer { border:1px solid palette(mid); border-radius:24px; background:palette(button); }"
        "QPlainTextEdit#agentPrompt { background:transparent; border:0; padding:2px 0; font-size:14px; }"
        // 同时约束 QSS 尺寸，避免宿主通用按钮 min-height 覆盖 setFixedSize 后压低圆形按钮。
        "QToolButton#agentAttach { border:1px solid palette(mid); border-radius:16px; background:transparent; padding:0;"
        " min-width:30px; max-width:30px; min-height:30px; max-height:30px; }"
        "QToolButton#agentAttach:hover { background:rgba(255,255,255,0.07); }"
        "QToolButton#agentPermissions, QToolButton#agentAddModel { border:0; border-radius:16px; background:transparent;"
        " padding:0 12px 0 10px; font-size:13px; color:palette(placeholder-text); }"
        "QToolButton#agentPermissions:hover, QToolButton#agentAddModel:hover { background:rgba(255,255,255,0.07); color:palette(text); }"
        "QToolButton#agentPermissions[level=\"full\"] { color:#e0a465; }"
        "QToolButton#agentPermissions::menu-indicator { image:none; width:0; }"
        "QComboBox#agentModel { background:transparent; border:0; border-radius:16px; font-size:13px;"
        " color:palette(placeholder-text); padding:0 26px 0 12px; min-height:32px; }"
        "QComboBox#agentModel:hover, QComboBox#agentModel:on { background:rgba(255,255,255,0.07); color:palette(text); }"
        "QComboBox#agentModel::drop-down { border:0; width:24px; }"
        "QComboBox#agentModel QAbstractItemView { border:1px solid palette(mid); border-radius:10px; padding:4px;"
        " font-size:13px; selection-background-color:rgba(255,255,255,0.08); outline:0; }"
        "QMenu#agentPermissionMenu { border:1px solid palette(mid); border-radius:14px; padding:4px 0 6px 0; }"
        "QLabel#agentPermissionMenuTitle { font-size:12px; color:palette(placeholder-text); }"
        "QLabel#agentPermissionTitle { font-size:13px; font-weight:500; }"
        "QLabel#agentPermissionTitle[warning=\"true\"] { color:#e0a465; }"
        "QLabel#agentPermissionDetail { font-size:12px; color:palette(placeholder-text); }"
        "QPushButton#agentSend, QPushButton#agentStop { border:0; border-radius:16px; padding:0; background:palette(text);"
        " min-width:32px; max-width:32px; min-height:32px; max-height:32px; }"
        "QPushButton#agentSend:hover, QPushButton#agentStop:hover { background:#ffffff; }"
        "QPushButton#agentSend:disabled { background:rgba(255,255,255,0.12); }"
        "QWidget#agentChangeSummary QLabel { font-size:12px; color:palette(placeholder-text); }"
        "QLabel#agentAddedSign, QLabel#agentAddedLines { color:#7fcf9c; }"
        "QLabel#agentRemovedSign, QLabel#agentRemovedLines { color:#e28e86; }"
        "QLabel#agentStatus[failed=\"true\"] { color:#e28e86; }"
        "QLabel#agentActivityDot { border-radius:3px; background:palette(placeholder-text); }"
        "QLabel#agentActivityDot[running=\"true\"] { background:#8ad6b0; }"
        "QLabel#agentActivityDot[failed=\"true\"] { background:#e28e86; }"
        "QFrame#agentAttachmentChip { border:1px solid palette(mid); border-radius:13px; background:transparent; }"
        "QFrame#agentAttachmentChip QLabel { font-size:12px; }"
        "QFrame#agentAttachmentChip QToolButton { border:0; border-radius:9px; padding:0; background:transparent; }"
        "QFrame#agentAttachmentChip QToolButton:hover { background:rgba(255,255,255,0.1); }"
    ));
    flushTimer.setInterval(40);
    flushTimer.setSingleShot(true);
    QObject::connect(&flushTimer, &QTimer::timeout, q, [this] { Flush(); });
    elapsedTimer.setInterval(1000);
    QObject::connect(&elapsedTimer, &QTimer::timeout, q, [this] { UpdateElapsed(); });
    UpdateSummary();
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
            QStringLiteral("将“%1”移到本地归档？完整记录会保留。") .arg(title),
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
    renameSession->setEnabled(available); archiveSession->setEnabled(available);
}

void AgentWindow::Private::BuildContextPopover() {
    // 与 Codex 一致的小卡片：三行居中，只回答“用了多少”。明细（输入/输出/缓存、上限来源、压缩说明）
    // 放在卡片的悬停提示里，不堆成说明文字。
    contextPopover = new UiKit::PopoverSurface(q);
    contextPopover->setObjectName(QStringLiteral("agentContextPopover"));
    auto* content = contextPopover->ContentWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(2);
    const auto line = [content, layout](const QString& name, int pixelSize, bool muted) {
        auto* label = new QLabel(content);
        label->setObjectName(name);
        label->setAlignment(Qt::AlignHCenter);
        QFont font = label->font(); font.setPixelSize(pixelSize); label->setFont(font);
        if (muted) {
            QPalette palette = label->palette();
            palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
            label->setPalette(palette);
        }
        layout->addWidget(label);
        return label;
    };
    contextHeading = line(QStringLiteral("agentContextHeading"), 12, true);
    contextHeading->setText(QStringLiteral("上下文窗口："));
    contextFraction = line(QStringLiteral("agentContextFraction"), 13, false);
    contextTokens = line(QStringLiteral("agentContextTokens"), 13, false);
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
    QStringList tip;
    if (used >= 0 && limit > 0) tip.append(QStringLiteral("%1 / %2 tokens").arg(QLocale().toString(used), QLocale().toString(limit)));
    if (!details.isEmpty()) tip.append(contextBreakdown->text());
    if (!contextSource->text().isEmpty()) tip.append(contextSource->text());
    tip.append(QStringLiteral("接近上限时会自动压缩较早的回合，完整记录保留在本地。"));
    contextPopover->ContentWidget()->setToolTip(tip.join(QLatin1Char('\n')));
    contextPopover->ContentWidget()->adjustSize();
}

void AgentWindow::Private::ModelChanged() {
    // 宽度跟随当前模型名，像 Codex 一样只占所需位置；过长名称省略显示，完整名在提示里。
    // 样式表字号不会反映到 font()，按 13px 实测；左右内边距、下拉箭头和框架共约 60px。
    QFont font = models->font();
    font.setPixelSize(13);
    const int textWidth = QFontMetrics(font).horizontalAdvance(models->currentText());
    models->setFixedWidth(std::clamp(textWidth + 60, 96, 240));
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
    newConversation->setEnabled(!busy);
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
}

void AgentWindow::Private::UpdateSendIcons() {
    // 圆形按钮底色是正文色；箭头取窗口底色，禁用时改用弱化色。
    const QColor ink = q->palette().color(QPalette::Window);
    QColor faded = q->palette().color(QPalette::Text);
    faded.setAlpha(110);
    send->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/arrow-up"), send->isEnabled() ? ink : faded));
    stop->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/square"), ink));
}

void AgentWindow::Private::UpdatePermissions() {
    const std::size_t index = PermissionIndex(settings);
    const auto& level = kPermissionLevels[index];
    permissions->setText(QString::fromUtf8(level.title));
    permissions->setIcon(level.commands ? UiKit::SvgIcon(QString::fromLatin1(level.icon), QColor(224, 164, 101))
                                        : UiKit::SvgIcon(QString::fromLatin1(level.icon)));
    permissions->setProperty("level", QString::fromLatin1(level.id));
    permissions->style()->unpolish(permissions);
    permissions->style()->polish(permissions);
    QStringList actual;
    actual << QStringLiteral("查询");
    if (settings.allowEdits) actual << QStringLiteral("编辑");
    if (settings.allowSave) actual << QStringLiteral("保存");
    if (settings.allowCommands) actual << QStringLiteral("外部命令");
    permissions->setToolTip(QStringLiteral("访问权限：%1\n实际权限由共用工具服务验证").arg(actual.join(QStringLiteral("、"))));
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
    if (conversation == nullptr || composerAxis == nullptr) return;
    const int available = std::max(280, q->width() - 32);
    const int body = mode == PresentationMode::Sidebar || q->width() < 760 ? available
        : std::min(available, std::clamp(static_cast<int>(q->width() * 0.64), 680, 760));
    conversation->SetContentWidth(body);
    // 宽页切换为侧栏时，上一尺寸不能成为父布局的新最小宽度。
    composerAxis->setMinimumWidth(0);
    composerAxis->setMaximumWidth(std::min(q->width() - 24, body + 32));
    ring->setVisible(true);
    QFont modelFont = models->font();
    modelFont.setPixelSize(13);
    const int requestedModelWidth = QFontMetrics(modelFont).horizontalAdvance(models->currentText()) + 60;
    const int modelLimit = q->width() < 460 ? std::max(96, q->width() - 240) : 240;
    models->setFixedWidth(std::clamp(requestedModelWidth, 96, modelLimit));
    const bool compact = q->width() < 560;
    changeDetails->setVisible(!compact);
    if (compact != compactSummary) { compactSummary = compact; UpdateSummary(); }
    permissions->setToolButtonStyle(q->width() >= 460 ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    elapsed->setToolTip(usage->text());
    UpdatePromptHeight();
}

void AgentWindow::Private::UpdatePromptHeight() {
    if (prompt == nullptr || resizingPrompt) return;
    resizingPrompt = true;
    const int lines = std::max(1, prompt->document()->lineCount());
    const int maximum = std::min(336, std::max(120, q->height() * 2 / 5));
    const int wanted = std::clamp(lines * 24 + 8, 48, maximum);
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
    if (found == tools.end()) { Tool tool; tool.elapsed.start(); found = tools.insert(id, tool); }
    Tool& tool = found.value();
    for (auto it = event.constBegin(); it != event.constEnd(); ++it) tool.event.insert(it.key(), it.value());
    if (type == QStringLiteral("tool_end")) {
        tool.finished = true;
        const bool failed = event.value(QStringLiteral("is_error")).toBool()
            || (event.contains(QStringLiteral("success")) && !event.value(QStringLiteral("success")).toBool());
        tool.state = failed ? QStringLiteral("失败") : event.value(QStringLiteral("success")).toBool()
            ? QStringLiteral("成功") : QStringLiteral("已结束");
    } else if (!tool.finished) tool.state = QStringLiteral("运行中");
    conversation->UpdateTool(tool.event, tool.state, DurationText(event.contains(QStringLiteral("duration_ms"))
        ? Count(event.value(QStringLiteral("duration_ms"))) : tool.elapsed.elapsed()));
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
    conversation->AppendChanges(event.value(QStringLiteral("items")).toArray());
    UpdateSummary();
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
}
void AgentWindow::SetMapSummary(QString summary) {
    d_->mapSummary->setText(std::move(summary));
    d_->mapSummary->setVisible(d_->mode == PresentationMode::Page && !d_->mapSummary->text().isEmpty());
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
}
void AgentWindow::ClearConversation() {
    d_->flushTimer.stop();
    d_->pending.clear();
    d_->assistantOpen = false;
    d_->tools.clear();
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
            d_->conversation->UpdateTool(tool.event, tool.state, DurationText(tool.elapsed.elapsed()));
        }
        d_->SetStatus(cancelled ? QStringLiteral("已停止") : failed ? QStringLiteral("运行失败")
            : event.value(QStringLiteral("success")).toBool() ? QStringLiteral("本轮完成") : QStringLiteral("本轮结束"), failed);
        SetBusy(false);
    }
}
void AgentWindow::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); d_->UpdateGeometry(); }
void AgentWindow::closeEvent(QCloseEvent* event) { QWidget::closeEvent(event); }
void AgentWindow::showEvent(QShowEvent* event) { QWidget::showEvent(event); emit VisibilityChanged(true); }
void AgentWindow::hideEvent(QHideEvent* event) {
    d_->sessionsPopover->Dismiss(); d_->contextPopover->Dismiss();
    QWidget::hideEvent(event); emit VisibilityChanged(false);
}
void AgentWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { emit BackRequested(); event->accept(); return; }
    QWidget::keyPressEvent(event);
}

} // namespace War3Map::Agent
